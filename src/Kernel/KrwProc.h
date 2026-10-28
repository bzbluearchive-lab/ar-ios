// KrwProc.h —— 进程级内核读写：pid → proc → task → map → pmap → 用户态页表遍历
//
// 位置：src/Kernel/KrwProc.h
// 依赖：只 include "kfd_glue.h"（纯 C 门面）。**不 include 任何 vendor 头**，
//       也**不 include KfdBackend.h** —— C++ 侧看见的 vendor 类型永远只有 kfd_glue.h 里那些函数
//       （理由见 kfd_glue.h 顶部与 KfdBackend.h 第一节：vendor 是 C，且带函数/数据定义，
//       只能被 kfd_glue.c 一个 TU include）。
//
// 作用：拿到另一个进程（或自己）的 `pmap` 之后，用 kfd 的内核读自己走 ARM64 页表，
//       把用户态 VA 直接翻译成物理页再由 kfdg_phystokv 换回内核 VA，从而读写那个进程的用户态内存。
//       ——这一层不依赖目标进程的 task port，只依赖 kfd 的内核读。
//
// ============================================================================
// 一、调用顺序（全部 inline，失败只打印 + 返回 false/0，不 assert、不抛异常）
// ============================================================================
//   krwproc::Ctx c;
//   if (!krwproc::init(&c, kfdg_handle)) return;        // 取 kernel_proc + 校验 p_pid==0
//   uint64_t proc = krwproc::find_proc(c, target_pid);  // 走 p_list，比对 p_pid
//   uint64_t task = krwproc::proc_task(c, proc);        // proc+0x18 → proc_ro+0x8
//   uint64_t pmap = krwproc::task_pmap(c, task);        // task+0x28 → map+0x40
//   if (!krwproc::selftest_self(c)) return;             // ★ 先在自己身上验通再碰游戏
//   uint8_t buf[16];
//   krwproc::uvm_read(c, pmap, game_va, buf, sizeof(buf));
//   krwproc::uvm_write(c, pmap, game_va, buf, sizeof(buf));   // 默认关闭，见第六节
//
// ============================================================================
// 二、结构体偏移：全部来自本机条目 171（vendor/kfd161/libkfd/info/dynamic_info.h:14134-14215）
// ============================================================================
// 那条 kern_version 是 "Darwin Kernel Version 22.1.0: Thu Oct  6 19:34:00 PDT 2022;
// root:xnu-8792.42.7~1/RELEASE_ARM64_T8112"，device_id "iPad14,4"、build_id "20B101"。
// 下面每个常量后面都标了它在 dynamic_info.h 里的行号，逐条可对。
//
// ★ 只有 proc → proc_ro → pr_task 这两个偏移不在 vendor 表里（表里没有 proc__proc_ro /
//   pr_task 这两个字段），数值来自 Picasso 的 offsets.m（同 xnu-8792 家族），所以单独标成
//   「待真机验证」。selftest_self() 会先把这条链在我们自己进程上走一遍：
//   链错的话它会在第 2 步/根表自校验处停下并打印中间值，不会带着错值去动游戏。
//
// ============================================================================
// 三、ARM64 / 16KB 粒度 / 四级页表的位域推导（★ 与需求里给的那组不同，依据写在下面）
// ============================================================================
// ARMv8-A 用 16KB 粒度时：
//   ① 页大小 16KB → 页内偏移占 VA[13:0]（14 位），即 0x4000；
//   ② 每一级翻译表在内存里都是 16KB 大表 = 16384 字节 / 8 字节 = **2048 项**
//      → 每级索引字段 11 位（2^11 = 2048）；
//   ③ 4 级 × 11 位 = 44 位，加上 14 位页内偏移 = 58 位 > 48 位 VA 宽度，
//      所以**顶层索引被 VA 宽度截断**：48 位 VA（T0SZ=16）下顶层只剩 VA[47] 这 1 位（2 项有效），
//      表本身仍占 16KB，其余 2046 个槽位不使用。
//   于是四级索引分别是：
//      L0 = VA[47:47]（1 位，2 项）
//      L1 = VA[46:36]（11 位，2048 项）
//      L2 = VA[35:25]（11 位，2048 项）
//      L3 = VA[24:14]（11 位，2048 项）
//      页内 = VA[13:0]
//   各级"块描述符"覆盖大小 = 2^(下面位数 + 14)：
//      L0 块 = 2^47（128TB）、L1 块 = 2^36（64GB）、L2 块 = 2^25（32MB）、L3 = 页 2^14（16KB）。
//
//   vendor 常量逐条对上（vendor/kfd161/libkfd/info/static_info.h:289-307）：
//      :291-292  L0 SHIFT=47 INDEX_MASK=0x0000800000000000  → 1 位
//      :296-297  L1 SHIFT=36 INDEX_MASK=0x00007ff000000000  → 11 位
//      :301-302  L2 SHIFT=25 INDEX_MASK=0x0000000ffe000000  → 11 位
//      :306-307  L3 SHIFT=14 INDEX_MASK=0x0000000001ffc000  → 11 位
//      :290/295/300/305  OFFMASK：L0 0x00007fffffffffff、L1 0x0000000fffffffff、
//                        L2 0x0000000001ffffff、L3 0x0000000000003fff
//      :294/299/304  SIZE：L1 0x1000000000（64GB）、L2 0x2000000（32MB）、L3 0x4000（16KB）
//      :289  L0 SIZE 0x800000000000（2^47）
//
//   ★ 与需求里给的「L1=VA[47:42]、L2=VA[41:25]」不一致，按上面的推导写，理由是：
//     (a) 6 位索引在 ARMv8 的任何粒度下都不存在（索引字段只有 9 位/11 位/13 位，加顶层的截断位）；
//     (b) VA[41:25] 是 17 位，不是 11 位，装不进"2048 项的 16KB 表"；
//     (c) 仓库里的 vendor 常量（上面 8 行）与 (a)(b) 冲突，而与上面的推导逐位吻合；
//     (d) 同一份 vendor 代码在真机上已经跑通（phy 版 kfd 的 vtophys 就是按这组位宽走的）。
//   与需求一致的部分：**L3 = VA[24:14]、页内 = VA[13:0]、页大小 16KB** 都对。
//
//   ★ 为什么本文件从 L1 开始、不碰 L0：xnu 的 pmap 根表就是 L1 表。
//     证据：perf.h:242 的 `const u64 ROOT_LEVEL = PMAP_TT_L1_LEVEL;`（从 L1 起遍历），
//     且 perf.h:160/165 用 `phystokv(pmap->ttep) == pmap->tte` 断言过"根表 PA 换回 VA 就是根表 VA"。
//     所以从 L1 起走的前提是 VA 的 bit47 = 0（用户态）；bit47=1 的内核 VA 要走 TTBR1，
//     本文件不处理，walk_page / uvm_read 会直接拒绝并打印。
//
// ============================================================================
// 四、表项的位（static_info.h:256-282，全部逐条引用）
// ============================================================================
//   L1/L2（TTE）：
//     :277 ARM_TTE_VALID      = 0x1        → bit0=1 才算有效
//     :278 ARM_TTE_TYPE_MASK  = 0x2        → bit1 选类型
//     :280 ARM_TTE_TYPE_BLOCK = 0x0        → bit1=0 = 块描述符（L1 64GB / L2 32MB）
//     :279 ARM_TTE_TYPE_TABLE = 0x2        → bit1=1 = 指向下一级表
//     所以：块 ⟺ (tte & 0x3) == 0x1；表 ⟺ (tte & 0x3) == 0x3。
//   L3（PTE）：
//     :257 ARM_PTE_TYPE_VALID = 0x3        → bit[1:0]=0b11 才是页描述符
//     :258 ARM_PTE_TYPE_MASK  = 0x2
//     :259 ARM_TTE_TYPE_L3BLOCK = 0x2      → L3 上"有效"即叶子（没有再下一级）
//   PA 掩码（L1/L2/L3 共用同一个值）：
//     :281 ARM_TTE_TABLE_MASK = 0x0000fffffffff000   → 下一级表的物理地址
//     :282 ARM_TTE_PA_MASK    = 0x0000fffffffff000   → 块/页的物理地址基址
//   所以最终页基址的掩码就是 **0x0000FFFFFFFFF000**（bit47..bit12），
//   低 14 位（VA[13:0]）由 VA 提供；16KB 粒度下 PTE 的 bit[13:12] 恒为 0，
//   因此"掩码之后 | 页内偏移"得到的页基址一定是 16KB 对齐的（本文件还会再 & ~0x3FFF 复核一次）。
//   xnu 的压缩页标记（:274-275）：ARM_PTE_COMPRESSED = 0x8000000000000000（bit63）、
//     ARM_PTE_COMPRESSED_ALT = 0x4000000000000000（bit62）。带这两位时 PA 字段不是物理页号，
//     本文件直接拒绝（读会读到别人的数据，写会写坏别人的页）。
//
// ============================================================================
// 五、PAC / 指针归一（t1sz_boot）
// ============================================================================
// 本机是 iPad14,4 / T8112（M2）。vendor 在 info.h:117 按芯片串给 t1sz_boot 赋值：
//   T8120 / T8130 / T8103 / T8112 / T8122 → 17，其它 → 25。
// 取 17 时（static_info.h:11-13）：
//   ptr_mask      = (1 << (64-17)) - 1 = 0x00007FFFFFFFFFFF
//   pac_mask      = ~ptr_mask          = 0xFFFF800000000000
//   unsign_kaddr(v)= v | pac_mask
// 对**已经是真内核 VA** 的值这一步是幂等的（47 位 VA 的内核段要求 VA[63:47] 全 1，
// 真机日志里的 kernel base 0xfffffe002e7c0000 / current_proc 0xfffffe130188cd50 都满足）；
// 而 task->map / _vm_map__pmap 读出来是**带 PAC**的 —— vendor 自己在 info.h:161-169 与
// 205-213 就是先 unsign 再用的，本文件的 task_pmap 照抄这个做法。
// ★ 换到 t1sz_boot=25 的机器上这个掩码要跟着改；init() 会用 kernel_proc 是否像内核 VA
//   把这种情况挡在开工之前（打印 t1sz_boot 相关的提示），不会带着错掩码继续走。
//
// ============================================================================
// 六、读写链路各自的未验证点（读侧 copy_validate，写侧 perf_kwrite 值域）
// ============================================================================
// 【读侧】kopen 之后生效的 kread 是 perf_kread（perf.h:195-198），它底下用的是 copyout()：
//   vendor 自己的论文写得很清楚 —— "use the PERFMON_CTL_SPECIFY ioctl to read between 1 and
//   65535 bytes from an arbitrary kernel address. In addition, note that the region being read
//   must satisfy the zone_element_bounds_check() in copy_validate(), because this technique uses
//   copyout() under the hood."（vendor/kfd_src/kfd-main/writeups/exploiting-puafs.md:233-237）
//   这句的两个后果，本文件都按最保守的方式处理：
//     ① 单次读的长度上限有硬约束（u16，perf.h:14/21），超了会被 (u16) 静默截断 —— 见下面
//        read_call_max，默认只给 8 字节（真机上唯一被验证过的长度）。
//     ② 被读区间不能跨出所在的 zone 元素（zone_element_bounds_check）。zone 元素的边界我们
//        不知道，所以"分片小 + 每次都在同一页内 + 8 字节对齐"就必然不跨元素边界；
//        也正因为如此，本文件每读一个 16KB 页都先 walk 页表、再在**页内**分片读，
//        绝不会让一次 kread 跨页（跨页就可能同时跨 zone 元素，直接 EFAULT → assert_bsd → abort）。
//   ★ 还有一个只能真机证伪的点：最终读的是"用户页的物理页在内核 physmap 里的影子地址"。
//     如果 copy_validate 在 physmap 上还要求别的条件，selftest_self 的第 3 步（读自己的 canary）
//     就是最早的探针 —— 它会先于任何游戏读写撞上这个问题。
//
// 【写侧】kopen 之后生效的 kwrite 是 perf_kwrite（perf.h:195-198 把 kwrite ops 换成它）：
// 它把每个 8 字节值当 perfmon event number 送进内核（perf.h:56-59），值不被接受时
// assert_bsd 直接 abort（common.h:113-136）。KfdBackend.h 第五节第 3 条对「整字为 0」的写
// 也是同一个理由（那里选择拒写 0 字）。本文件的 uvm_write 是**读-改-写**（kwrite 只接受
// 8 字节倍数的整字，页内不对齐的部分必须靠 RMW），写回的整字里必然包含我们不关心的邻居字节，
// 邻居字节是 0 时没法回避，所以这里不做"拒写 0 字"，改成**总开关默认关闭**：
//   验通之后（拿一个无害页试写 → 回读正确）由调用方把 krwproc::allow_perf_kwrite 置 true。
// 另外 RMW 与目标进程是并发跑的：目标自己同时在写同一块内存时，我们写回的是读到的旧值。
//
// ============================================================================
// 七、真机上最可能出问题的两处（写在代码里的同一份事实）
// ============================================================================
// 1) proc → proc_ro(0x18) → pr_task(0x8) 这两个偏移没有 vendor 表背书（只有 Picasso）。
//    偏移错 → 读 task 时读到别的字段 → task+0x28 读到垃圾 → 在 map/pmap 那条链上打出
//    "读失败/不对齐/根表自校验不过"。所以 selftest_self 必须在**自己进程**上先跑；
//    它一旦在第 2 步失败，就说明这两个偏移要按本机 kernelcache 重新量（本文件不猜）。
// 2) perf 这条链路的两端都还没有真机证据（见第六节）：
//    ① 读端：copyout() + copy_validate() 是否接受"用户页在 physmap 里的影子地址"；
//    ② 写端：ADD_EVENT 把 8 字节值当 perfmon event number，任意值能不能写进去。
//    任一端不通，本文件只剩"页表遍历"这一半可用（遍历本身可以靠 selftest 的第 1/2 步单独验证）。
//    次要但同样真实的：目标进程的页可能已被压缩（PTE 带 bit63/62，本文件拒绝并打印），
//    以及 walk 完到 kread 之间页被改映射（并发，只能靠上层数据自检发现）。
//
// 约定：全程中文 printf（[krw] 前缀）；不用 assert、不抛异常；任何失败都打印"哪一级 / 哪个地址 /
//       读到的值"再返回 false/0。

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "kfd_glue.h"

namespace krwproc {

// ===========================================================================
// 结构体偏移（唯一来源：dynamic_info.h:14134-14215，条目 171）
// ===========================================================================

inline constexpr uint64_t kProcPListLeNext  = 0x0;    // dynamic_info.h:14154
inline constexpr uint64_t kProcPListLePrev  = 0x8;    // dynamic_info.h:14155（本文件不用；记在这里是因为 vendor 自己是从 le_prev 反向走的，见 kread_sem_open.h:113）
inline constexpr uint64_t kProcPid          = 0x60;   // dynamic_info.h:14156
inline constexpr uint64_t kProcObjectSize   = 0x530;  // dynamic_info.h:14158
inline constexpr uint64_t kTaskMap          = 0x28;   // dynamic_info.h:14169
inline constexpr uint64_t kTaskItkSpace     = 0x300;  // dynamic_info.h:14172（本文件不用，记录备查）
inline constexpr uint64_t kVmMapPmap        = 0x40;   // dynamic_info.h:14194
inline constexpr uint64_t kVmMapObjectSize  = 0xc0;   // dynamic_info.h:14198
inline constexpr uint64_t kPmapTte          = 0x0;    // dynamic_info.h:14152
inline constexpr uint64_t kPmapTtep         = 0x8;    // dynamic_info.h:14153

// ★★ 待真机验证 ★★（vendor 表里没有这两个字段；数值来自 Picasso offsets.m，同 xnu-8792 家族）
inline constexpr uint64_t kProcProcRo       = 0x18;   // 待真机验证：struct proc → struct proc_ro *
inline constexpr uint64_t kProcRoPrTask     = 0x8;    // 待真机验证：struct proc_ro → task_t

// ===========================================================================
// 页表几何（推导见文件顶部第三节）
// ===========================================================================

inline constexpr uint64_t kPageShift      = 14;                      // 16KB 页 → VA[13:0]
inline constexpr uint64_t kPageSize       = 1ull << kPageShift;      // 0x4000
inline constexpr uint64_t kPageMask       = kPageSize - 1;           // 0x3FFF
inline constexpr uint64_t kIndexBits      = 11;                      // log2(2048)
inline constexpr uint64_t kEntriesPerTable = 1ull << kIndexBits;     // 2048 = 16KB / 8B

inline constexpr uint64_t kL1Shift     = 36;                          // static_info.h:296
inline constexpr uint64_t kL1IndexMask = 0x00007FF000000000ull;        // :297  VA[46:36]
inline constexpr uint64_t kL1OffMask   = 0x0000000FFFFFFFFFull;        // :295  VA[35:0]
inline constexpr uint64_t kL2Shift     = 25;                          // :301
inline constexpr uint64_t kL2IndexMask = 0x0000000FFE000000ull;        // :302  VA[35:25]
inline constexpr uint64_t kL2OffMask   = 0x0000000001FFFFFFull;        // :300  VA[24:0]
inline constexpr uint64_t kL3Shift     = 14;                          // :306
inline constexpr uint64_t kL3IndexMask = 0x0000000001FFC000ull;        // :307  VA[24:14]
inline constexpr uint64_t kL3OffMask   = 0x0000000000003FFFull;        // :305  VA[13:0]

// 描述符位（static_info.h:256-282）
inline constexpr uint64_t kTteValid       = 0x1;                       // :277
inline constexpr uint64_t kTteTypeMask    = 0x2;                       // :278
inline constexpr uint64_t kTteTypeBlock   = 0x0;                       // :280
inline constexpr uint64_t kTteTypeTable   = 0x2;                       // :279
inline constexpr uint64_t kTteTableMask   = 0x0000FFFFFFFFFFF000ull;    // :281
inline constexpr uint64_t kTtePaMask      = 0x0000FFFFFFFFFFF000ull;    // :282
inline constexpr uint64_t kPteTypeValid   = 0x3;                       // :257
inline constexpr uint64_t kPteTypeMask    = 0x2;                       // :258
inline constexpr uint64_t kPteTypeL3Block = 0x2;                       // :259
inline constexpr uint64_t kPteCompressed  = 0x8000000000000000ull;      // :275
inline constexpr uint64_t kPteCompressedAlt = 0x4000000000000000ull;    // :274

// ===========================================================================
// PAC / 指针归一（推导见文件顶部第五节）
// ===========================================================================

inline constexpr uint64_t kT1szBoot  = 17;                                 // info.h:117（T8112 → 17）
inline constexpr uint64_t kPtrMask   = (1ull << (64ull - kT1szBoot)) - 1ull; // static_info.h:11
inline constexpr uint64_t kPacMask   = ~kPtrMask;                          // static_info.h:12

// 47 位 VA 的内核段起点：VA[63:47] 全 1 → 0xFFFF800000000000
inline constexpr uint64_t kKvaFloor  = 0xFFFF800000000000ull;

inline uint64_t unsign_kaddr(uint64_t v) { return v | kPacMask; }         // static_info.h:13
inline bool looks_like_kva(uint64_t v) { return v >= kKvaFloor; }

// ===========================================================================
// Ctx
// ===========================================================================

struct Ctx {
    uint64_t kfd;          // kfdg_open() 的句柄；0 = 不可用
    uint64_t kernel_proc;  // pid 0 那条 struct proc 的内核地址（kfdg_kernel_proc）
};

// ===========================================================================
// 小工具：8 字节内核读 / 跟指针
// ===========================================================================

// 8 字节内核读。
// 返回 false 只有三种原因：出参为 NULL、kaddr=0、glue 参数被拒（句柄 0 / size 不是 8 的倍数）。
// ★ kread 本身没有返回值（kfd_glue.h:44-47）：读到的数据对不对必须由调用方校验。
//   本文件的校验手段是"每个指针都查 8 字节对齐 + p_list 上查环 + pmap 根表自校验"。
inline bool kread64(const Ctx& c, uint64_t kaddr, uint64_t* out)
{
    uint64_t v = 0;

    if (out == nullptr) {
        printf("[krw] kread64 失败：出参为 NULL\n");
        return false;
    }
    *out = 0;
    if (c.kfd == 0) {
        printf("[krw] kread64 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[krw] kread64 失败：kaddr=0\n");
        return false;
    }
    if (kfdg_read(c.kfd, kaddr, &v, (uint64_t)sizeof(v)) != 0) {
        printf("[krw] kread64 失败：kfdg_read 参数被拒，kaddr=0x%llx\n",
               (unsigned long long)kaddr);
        return false;
    }
    *out = v;
    return true;
}

// 跟一个从内核结构里读出来的指针：
//   ① raw == 0 → 链断了（必须先判 raw：unsign(0) 会变成 0xFFFF800000000000，那不是 0）；
//   ② 归一：p = raw | pac_mask（对真内核 VA 幂等，对带 PAC 的值才是必须的，见文件顶部第五节）；
//   ③ 要求 8 字节对齐（结构体指针至少 8 对齐；不对齐基本就是偏移错了）。
// what 是调用方给的说明，直接拼进日志里，方便一眼看出是哪一跳断的。
inline bool follow_ptr(const char* what, uint64_t raw, uint64_t* out)
{
    uint64_t p = 0;

    if (out == nullptr) {
        printf("[krw] %s 失败：出参为 NULL\n", what);
        return false;
    }
    *out = 0;
    if (raw == 0) {
        printf("[krw] %s 失败：读到 0（链断了）\n", what);
        return false;
    }
    p = unsign_kaddr(raw);
    if ((p & 0x7ull) != 0) {
        printf("[krw] %s 失败：raw=0x%llx 归一(pac_mask=0x%llx)后=0x%llx，不是 8 字节对齐"
               " → 偏移错了，或 t1sz_boot 不是 %llu\n",
               what,
               (unsigned long long)raw,
               (unsigned long long)kPacMask,
               (unsigned long long)p,
               (unsigned long long)kT1szBoot);
        return false;
    }
    *out = p;
    return true;
}

// ===========================================================================
// init
// ===========================================================================

inline bool init(Ctx* c, uint64_t kfdHandle)
{
    uint64_t w = 0;
    uint64_t kp = 0;
    int32_t pid0 = 0;

    if (c == nullptr) {
        printf("[krw] init 失败：Ctx 指针为 NULL\n");
        return false;
    }
    c->kfd = 0;
    c->kernel_proc = 0;

    if (kfdHandle == 0) {
        printf("[krw] init 失败：kfd 句柄为 0（kfdg_open 还没成功）\n");
        return false;
    }

    kp = kfdg_kernel_proc(kfdHandle);
    if (kp == 0) {
        printf("[krw] init 失败：kfdg_kernel_proc 返回 0（kfd 的 find_proc 没跑成）\n");
        return false;
    }
    if (!looks_like_kva(kp) || ((kp & 0x7ull) != 0)) {
        printf("[krw] init 失败：kernel_proc=0x%llx 不像内核 VA（阈值 0x%llx）或不对齐。"
               "本文件的 PAC 掩码按 t1sz_boot=%llu 写死（info.h:117：T8112 → 17），"
               "换设备要先改 kT1szBoot，别带着错掩码往下走\n",
               (unsigned long long)kp,
               (unsigned long long)kKvaFloor,
               (unsigned long long)kT1szBoot);
        return false;
    }

    c->kfd = kfdHandle;
    c->kernel_proc = kp;

    // 第二道独立证据：kernel_proc 的 p_pid 必须是 0（kernel task 的 proc）。
    // 判据与 KfdBackend.h 的 selftest 第 4 步相同；顺带证明 p_pid 偏移 0x60 与 slide 是对的。
    if (!kread64(*c, kp + kProcPid, &w)) {
        printf("[krw] init 失败：读 kernel_proc->p_pid 失败（kaddr=0x%llx，proc=0x%llx）\n",
               (unsigned long long)(kp + kProcPid),
               (unsigned long long)kp);
        c->kfd = 0;
        c->kernel_proc = 0;
        return false;
    }
    pid0 = (int32_t)(uint32_t)(w & 0xffffffffull);
    if (pid0 != 0) {
        printf("[krw] init 失败：kernel_proc->p_pid = %d（期望 0）—— proc 偏移或 kernel slide 不对，"
               "此时任何后续读写都会打偏\n",
               (int)pid0);
        c->kfd = 0;
        c->kernel_proc = 0;
        return false;
    }

    printf("[krw] init 成功：kfd=0x%llx kernel_proc=0x%llx（p_pid=0 校验通过）\n",
           (unsigned long long)kfdHandle,
           (unsigned long long)kp);
    return true;
}

// ===========================================================================
// find_proc：走 p_list，比对 p_pid
// ===========================================================================
// p_list 是**环形**双向链表（LIST_ENTRY）。vendor 自己是从 kernel_proc 出发走 **le_prev**
// 反向找（kread_sem_open.h:104-114：注释写明 kernel_proc 是表尾），本函数走 **le_next**：
// 从表尾出发走正向，第一步就回到表头，同样能在有限步内覆盖全表。
// 防环（三重）：
//   ① 硬上限 4096 个节点；
//   ② 走满一圈回到 kernel_proc → pid 不存在，停；
//   ③ 最近 kCycleRing 个节点出现重复地址（短环）→ 停；
//   另外每次读指针都查 8 字节对齐，le_next 自指也当环处理。

inline constexpr uint64_t kMaxProcScan = 4096;
inline constexpr uint64_t kCycleRing   = 32;

inline uint64_t find_proc(const Ctx& c, int pid)
{
    uint64_t ring[kCycleRing];
    uint64_t ring_pos = 0;
    uint64_t cur = 0;
    uint64_t step = 0;

    if (c.kfd == 0 || c.kernel_proc == 0) {
        printf("[krw] find_proc 失败：Ctx 没 init（kfd=0x%llx kernel_proc=0x%llx）\n",
               (unsigned long long)c.kfd,
               (unsigned long long)c.kernel_proc);
        return 0;
    }

    {
        uint64_t i = 0;
        for (i = 0; i < kCycleRing; i++) {
            ring[i] = 0;
        }
    }

    cur = c.kernel_proc;

    for (step = 0; step < kMaxProcScan; step++) {
        uint64_t w = 0;
        uint64_t raw_next = 0;
        uint64_t next = 0;
        int32_t node_pid = 0;
        uint64_t i = 0;

        // 短环检测：cur 在最近 kCycleRing 个节点里出现过
        for (i = 0; i < kCycleRing; i++) {
            if (ring[i] == cur) {
                printf("[krw] find_proc 失败：p_list 在 0x%llx 处形成短环（第 %llu 步重访，"
                       "最近 %llu 个节点内重复），pid=%d 没找到\n",
                       (unsigned long long)cur,
                       (unsigned long long)step,
                       (unsigned long long)kCycleRing,
                       pid);
                return 0;
            }
        }
        ring[ring_pos] = cur;
        ring_pos = (ring_pos + 1ull) % kCycleRing;

        // 1) p_pid（int32，落在低 32 位）
        if (!kread64(c, cur + kProcPid, &w)) {
            printf("[krw] find_proc 失败：读 p_pid 失败，第 %llu 个节点 proc=0x%llx（kaddr=0x%llx）\n",
                   (unsigned long long)step,
                   (unsigned long long)cur,
                   (unsigned long long)(cur + kProcPid));
            return 0;
        }
        node_pid = (int32_t)(uint32_t)(w & 0xffffffffull);
        if (node_pid == pid) {
            printf("[krw] find_proc 命中：pid=%d proc=0x%llx（第 %llu 个节点）\n",
                   pid,
                   (unsigned long long)cur,
                   (unsigned long long)(step + 1ull));
            return cur;
        }

        // 2) 下一个节点 = p_list.le_next
        if (!kread64(c, cur + kProcPListLeNext, &raw_next)) {
            printf("[krw] find_proc 失败：读 le_next 失败，第 %llu 个节点 proc=0x%llx（kaddr=0x%llx）\n",
                   (unsigned long long)step,
                   (unsigned long long)cur,
                   (unsigned long long)(cur + kProcPListLeNext));
            return 0;
        }
        if (raw_next == 0) {
            printf("[krw] find_proc 失败：第 %llu 个节点 proc=0x%llx 的 le_next=0（链表断裂）\n",
                   (unsigned long long)step,
                   (unsigned long long)cur);
            return 0;
        }
        next = unsign_kaddr(raw_next);
        if ((next & 0x7ull) != 0) {
            printf("[krw] find_proc 失败：第 %llu 个节点 proc=0x%llx 的 le_next raw=0x%llx "
                   "归一后=0x%llx 不对齐（偏移或 PAC 掩码不对）\n",
                   (unsigned long long)step,
                   (unsigned long long)cur,
                   (unsigned long long)raw_next,
                   (unsigned long long)next);
            return 0;
        }
        if (next == cur) {
            printf("[krw] find_proc 失败：proc=0x%llx 的 le_next 自指\n", (unsigned long long)cur);
            return 0;
        }
        if (next == c.kernel_proc) {
            printf("[krw] find_proc 失败：走满一圈回到起点 0x%llx（共 %llu 个节点），"
                   "pid=%d 不在 p_list 上\n",
                   (unsigned long long)c.kernel_proc,
                   (unsigned long long)(step + 1ull),
                   pid);
            return 0;
        }
        cur = next;
    }

    printf("[krw] find_proc 失败：pid=%d 在 %llu 步硬上限内没找到（p_list 可能已经坏了）\n",
           pid,
           (unsigned long long)kMaxProcScan);
    return 0;
}

// ===========================================================================
// proc_task：proc → proc_ro → task（★ 两个偏移待真机验证）
// ===========================================================================

inline uint64_t proc_task(const Ctx& c, uint64_t proc)
{
    uint64_t ro_raw = 0;
    uint64_t ro = 0;
    uint64_t task_raw = 0;
    uint64_t task = 0;

    if (c.kfd == 0) {
        printf("[krw] proc_task 失败：Ctx 没 init（kfd=0）\n");
        return 0;
    }
    if (proc == 0) {
        printf("[krw] proc_task 失败：proc=0\n");
        return 0;
    }

    // 1) proc + 0x18 → struct proc_ro *
    if (!kread64(c, proc + kProcProcRo, &ro_raw)) {
        printf("[krw] proc_task 失败：读 proc_ro 失败 proc=0x%llx（kaddr=0x%llx，偏移 0x%llx 待真机验证）\n",
               (unsigned long long)proc,
               (unsigned long long)(proc + kProcProcRo),
               (unsigned long long)kProcProcRo);
        return 0;
    }
    if (!follow_ptr("proc_task(proc_ro)", ro_raw, &ro)) {
        printf("[krw]   proc=0x%llx +0x%llx 读到 0x%llx（这是 proc_ro 一跳）\n",
               (unsigned long long)proc,
               (unsigned long long)kProcProcRo,
               (unsigned long long)ro_raw);
        return 0;
    }

    // 2) proc_ro + 0x8 → task_t
    if (!kread64(c, ro + kProcRoPrTask, &task_raw)) {
        printf("[krw] proc_task 失败：读 pr_task 失败 proc_ro=0x%llx（kaddr=0x%llx，偏移 0x%llx 待真机验证）\n",
               (unsigned long long)ro,
               (unsigned long long)(ro + kProcRoPrTask),
               (unsigned long long)kProcRoPrTask);
        return 0;
    }
    if (!follow_ptr("proc_task(pr_task)", task_raw, &task)) {
        printf("[krw]   proc_ro=0x%llx +0x%llx 读到 0x%llx（这是 pr_task 一跳）\n",
               (unsigned long long)ro,
               (unsigned long long)kProcRoPrTask,
               (unsigned long long)task_raw);
        return 0;
    }
    if (task == proc || task == ro) {
        printf("[krw] proc_task 失败：task=0x%llx 与 proc/proc_ro 相同（链自指，偏移不对）\n",
               (unsigned long long)task);
        return 0;
    }

    printf("[krw] proc_task：proc=0x%llx --(+0x%llx)--> proc_ro=0x%llx --(+0x%llx)--> task=0x%llx\n",
           (unsigned long long)proc,
           (unsigned long long)kProcProcRo,
           (unsigned long long)ro,
           (unsigned long long)kProcRoPrTask,
           (unsigned long long)task);
    printf("[krw]   注意：上面两个偏移（0x%llx / 0x%llx）来自 Picasso，vendor 表里没有，"
           "属于待真机验证项\n",
           (unsigned long long)kProcProcRo,
           (unsigned long long)kProcRoPrTask);
    return task;
}

// ===========================================================================
// task_pmap：task → map → pmap
// ===========================================================================
// task->map 与 _vm_map__pmap 读出来是**带 PAC** 的指针：vendor 在 info.h:161-162（current_map）、
// 168-169（current_pmap）、205-206（kernel_map）、212-213（kernel_pmap）里都是先 unsign_kaddr
// 再用的，这里照抄。

inline uint64_t task_pmap(const Ctx& c, uint64_t task)
{
    uint64_t map_raw = 0;
    uint64_t map = 0;
    uint64_t pmap_raw = 0;
    uint64_t pmap = 0;

    if (c.kfd == 0) {
        printf("[krw] task_pmap 失败：Ctx 没 init（kfd=0）\n");
        return 0;
    }
    if (task == 0) {
        printf("[krw] task_pmap 失败：task=0\n");
        return 0;
    }

    // 1) task + 0x28 → vm_map_t（带 PAC）
    if (!kread64(c, task + kTaskMap, &map_raw)) {
        printf("[krw] task_pmap 失败：读 task->map 失败 task=0x%llx（kaddr=0x%llx）\n",
               (unsigned long long)task,
               (unsigned long long)(task + kTaskMap));
        return 0;
    }
    if (!follow_ptr("task_pmap(map)", map_raw, &map)) {
        printf("[krw]   task=0x%llx +0x%llx 读到 0x%llx\n",
               (unsigned long long)task,
               (unsigned long long)kTaskMap,
               (unsigned long long)map_raw);
        return 0;
    }

    // 2) map + 0x40 → pmap_t（带 PAC）
    if (!kread64(c, map + kVmMapPmap, &pmap_raw)) {
        printf("[krw] task_pmap 失败：读 map->pmap 失败 map=0x%llx（kaddr=0x%llx）\n",
               (unsigned long long)map,
               (unsigned long long)(map + kVmMapPmap));
        return 0;
    }
    if (!follow_ptr("task_pmap(pmap)", pmap_raw, &pmap)) {
        printf("[krw]   map=0x%llx +0x%llx 读到 0x%llx\n",
               (unsigned long long)map,
               (unsigned long long)kVmMapPmap,
               (unsigned long long)pmap_raw);
        return 0;
    }
    if (pmap == map || pmap == task) {
        printf("[krw] task_pmap 失败：pmap=0x%llx 与 map/task 相同（链自指，偏移不对）\n",
               (unsigned long long)pmap);
        return 0;
    }

    printf("[krw] task_pmap：task=0x%llx --(+0x%llx)--> map=0x%llx（raw=0x%llx）"
           " --(+0x%llx)--> pmap=0x%llx（raw=0x%llx）\n",
           (unsigned long long)task,
           (unsigned long long)kTaskMap,
           (unsigned long long)map,
           (unsigned long long)map_raw,
           (unsigned long long)kVmMapPmap,
           (unsigned long long)pmap,
           (unsigned long long)pmap_raw);
    return pmap;
}

// ===========================================================================
// verify_pmap：pmap 根表自校验（与 vendor 的 assert 同一判据）
// ===========================================================================
// perf.h:158-165 在真机上断言的原文是：
//   ttbr[0].va = pmap->tte;  ttbr[0].pa = pmap->ttep;  assert(phystokv(pa) == va);
// 也就是说：把根表的物理地址换算回内核 VA，必须等于根表的 VA。
// 这一条同时验证四件事，所以放在"真正开始遍历"之前当总闸：
//   ① pmap 这个地址是对的（错地址读出来的是两坨垃圾，不可能刚好满足这个等式）；
//   ② pmap__tte / pmap__ttep 两个偏移对；
//   ③ kfdg_phystokv 的换算对（含 ptov_table / gVirtBase 那条路）；
//   ④ perf 已经在 perf_run 里填好了。
// 参数 out_root_va 返回根表（L1 表）的内核 VA，也就是 walk_page 的起点。

inline bool verify_pmap(const Ctx& c, uint64_t pmap, uint64_t* out_root_va)
{
    uint64_t tte = 0;
    uint64_t ttep = 0;
    uint64_t root_from_pa = 0;

    if (out_root_va != nullptr) {
        *out_root_va = 0;
    }
    if (c.kfd == 0) {
        printf("[krw] verify_pmap 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (pmap == 0) {
        printf("[krw] verify_pmap 失败：pmap=0\n");
        return false;
    }
    if ((pmap & 0x7ull) != 0) {
        printf("[krw] verify_pmap 失败：pmap=0x%llx 不是 8 字节对齐\n", (unsigned long long)pmap);
        return false;
    }

    if (!kread64(c, pmap + kPmapTte, &tte)) {
        printf("[krw] verify_pmap 失败：读 pmap->tte 失败 pmap=0x%llx（kaddr=0x%llx）\n",
               (unsigned long long)pmap,
               (unsigned long long)(pmap + kPmapTte));
        return false;
    }
    if (!kread64(c, pmap + kPmapTtep, &ttep)) {
        printf("[krw] verify_pmap 失败：读 pmap->ttep 失败 pmap=0x%llx（kaddr=0x%llx）\n",
               (unsigned long long)pmap,
               (unsigned long long)(pmap + kPmapTtep));
        return false;
    }
    if (tte == 0 || ttep == 0) {
        printf("[krw] verify_pmap 失败：pmap=0x%llx 的 tte=0x%llx ttep=0x%llx（有一个是 0，"
               "根表指针没读到或 pmap 地址不对）\n",
               (unsigned long long)pmap,
               (unsigned long long)tte,
               (unsigned long long)ttep);
        return false;
    }
    if ((tte & 0x7ull) != 0) {
        printf("[krw] verify_pmap 失败：pmap=0x%llx 的 tte=0x%llx 不是 8 字节对齐\n",
               (unsigned long long)pmap,
               (unsigned long long)tte);
        return false;
    }

    root_from_pa = kfdg_phystokv(c.kfd, ttep);
    if (root_from_pa == 0) {
        printf("[krw] verify_pmap 失败：kfdg_phystokv(ttep=0x%llx) 返回 0"
               "（越界或 perf 没填好，原因见上面 [kfdg] 日志）\n",
               (unsigned long long)ttep);
        return false;
    }
    if (root_from_pa != tte) {
        printf("[krw] verify_pmap 失败：phystokv(ttep)=0x%llx != pmap->tte=0x%llx"
               "（pmap=0x%llx ttep=0x%llx）。这就是 vendor perf.h:160/165 的那条 assert；"
               "对不上说明 pmap 地址或 tte/ttep 偏移不对，禁止继续遍历\n",
               (unsigned long long)root_from_pa,
               (unsigned long long)tte,
               (unsigned long long)pmap,
               (unsigned long long)ttep);
        return false;
    }

    printf("[krw] verify_pmap 通过：pmap=0x%llx 根表 tte=0x%llx ttep=0x%llx（phystokv 换算一致）\n",
           (unsigned long long)pmap,
           (unsigned long long)tte,
           (unsigned long long)ttep);
    if (out_root_va != nullptr) {
        *out_root_va = tte;
    }
    return true;
}

// verify_pmap 的结果缓存：uvm_read 会被高频调用（一帧几十上百次），每次都验一遍要多花 3 次
// kread。这里按 pmap 缓存"已验证 + 根表 VA"，同一个 pmap 只验一次。
// ★ 缓存的是 pmap 本身和它的根表，pmap 生命周期内不变；换目标进程时调用
//   invalidate_pmap_cache() 清一次（pmap 被释放后地址被复用属于 ABA，用这个函数兜）。
// static 在头文件里 = 每个 TU 一份，只当缓存用，不影响正确性。
static uint64_t g_verified_pmap = 0;
static uint64_t g_verified_root = 0;

inline void invalidate_pmap_cache(void)
{
    g_verified_pmap = 0;
    g_verified_root = 0;
}

inline bool ensure_pmap_root(const Ctx& c, uint64_t pmap, uint64_t* out_root_va)
{
    uint64_t root = 0;

    if (out_root_va != nullptr) {
        *out_root_va = 0;
    }
    if (pmap != 0 && pmap == g_verified_pmap && g_verified_root != 0) {
        if (out_root_va != nullptr) {
            *out_root_va = g_verified_root;
        }
        return true;
    }
    if (!verify_pmap(c, pmap, &root)) {
        return false;
    }
    g_verified_pmap = pmap;
    g_verified_root = root;
    if (out_root_va != nullptr) {
        *out_root_va = root;
    }
    return true;
}

// ===========================================================================
// walk_page：一个 16KB 页的 VA → PA → 内核 VA
// ===========================================================================
// 起点是根表（L1 表）的内核 VA，逐级：
//   L1 索引 = (VA & 0x00007ff000000000) >> 36   → 表项 e1
//   L2 索引 = (VA & 0x0000000ffe000000) >> 25   → 表项 e2
//   L3 索引 = (VA & 0x0000000001ffc000) >> 14   → 表项 e3
// 每一级：e 的 bit0=1 才有效；bit1=0 是块描述符（到这一级就命中，直接用块基址），
//         bit1=1 是指向下一级表，表项里的物理地址 = e & 0x0000FFFFFFFFF000，
//         再用 kfdg_phystokv 换成内核 VA 才能读下一级表。
// L3 命中后：页基址 = (e3 & 0x0000FFFFFFFFF000)，页内偏移由 VA[13:0] 补。
// 块描述符的覆盖范围比页大，所以命中块时先把块内偏移算出来，再 & ~0x3FFF 取到页。

struct PageMapping {
    uint64_t entry;  // 命中那一级的表项原值
    uint64_t va;     // 输入 VA 所在 16KB 页的起始 VA
    uint64_t pa;     // 该页的物理地址（16KB 对齐）
    uint64_t kva;    // 该物理页对应的内核 VA
    int      level;  // 命中级别：3 = L3 页描述符、2 = L2 块（32MB）、1 = L1 块（64GB）
};

// 命中之后统一收尾：把块/页的物理地址对齐到 16KB，再换算成内核 VA。
inline bool finish_mapping(const Ctx& c,
                           uint64_t va,
                           uint64_t page_va,
                           uint64_t entry,
                           int level,
                           uint64_t full_pa,
                           PageMapping* out)
{
    uint64_t page_pa = 0;
    uint64_t kva = 0;

    if (out == nullptr) {
        printf("[krw] finish_mapping 失败：出参为 NULL\n");
        return false;
    }

    page_pa = full_pa & ~kL3OffMask;   // 16KB 对齐（16KB 粒度下 PTE 的 bit[13:12] 恒为 0）
    if (page_pa == 0) {
        printf("[krw] 页表遍历失败：%d 级命中但物理页基址算出来是 0（va=0x%llx 表项=0x%llx）\n",
               level,
               (unsigned long long)va,
               (unsigned long long)entry);
        return false;
    }

    kva = kfdg_phystokv(c.kfd, page_pa);
    if (kva == 0) {
        printf("[krw] 页表遍历失败：物理页 0x%llx（%d 级命中，表项=0x%llx，va=0x%llx）"
               "换算内核 VA 失败，原因见上面 [kfdg] 日志\n",
               (unsigned long long)page_pa,
               level,
               (unsigned long long)entry,
               (unsigned long long)va);
        return false;
    }

    out->entry = entry;
    out->va = page_va;
    out->pa = page_pa;
    out->kva = kva;
    out->level = level;
    return true;
}

inline bool walk_page(const Ctx& c, uint64_t root_va, uint64_t va, PageMapping* out)
{
    uint64_t page_va = 0;
    uint64_t idx1 = 0;
    uint64_t addr1 = 0;
    uint64_t e1 = 0;
    uint64_t table2_pa = 0;
    uint64_t table2_va = 0;
    uint64_t idx2 = 0;
    uint64_t addr2 = 0;
    uint64_t e2 = 0;
    uint64_t table3_pa = 0;
    uint64_t table3_va = 0;
    uint64_t idx3 = 0;
    uint64_t addr3 = 0;
    uint64_t e3 = 0;
    uint64_t full_pa = 0;

    if (out == nullptr) {
        printf("[krw] walk_page 失败：出参为 NULL\n");
        return false;
    }
    out->entry = 0;
    out->va = 0;
    out->pa = 0;
    out->kva = 0;
    out->level = 0;

    if (c.kfd == 0) {
        printf("[krw] walk_page 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (root_va == 0) {
        printf("[krw] walk_page 失败：root_va=0（根表 VA 没拿到）\n");
        return false;
    }
    if ((root_va & 0x7ull) != 0) {
        printf("[krw] walk_page 失败：root_va=0x%llx 不是 8 字节对齐\n",
               (unsigned long long)root_va);
        return false;
    }
    // 从 L1 起走的前提：这是用户态 VA（bit47=0）。内核 VA 要走 TTBR1，本函数不处理。
    if ((va >> 47) != 0) {
        printf("[krw] walk_page 失败：va=0x%llx 的 bit47=1，不是用户态 VA"
               "（本函数从 L1 根表起走，只做用户态；内核 VA 要走 TTBR1）\n",
               (unsigned long long)va);
        return false;
    }

    page_va = va & ~kL3OffMask;

    // ---- L1（根表）：索引 VA[46:36] ----
    idx1 = (page_va & kL1IndexMask) >> kL1Shift;
    addr1 = root_va + idx1 * 8ull;
    if (!kread64(c, addr1, &e1)) {
        printf("[krw] 页表遍历失败：L1 表项读失败 va=0x%llx root=0x%llx idx1=%llu 表项地址=0x%llx\n",
               (unsigned long long)va,
               (unsigned long long)root_va,
               (unsigned long long)idx1,
               (unsigned long long)addr1);
        return false;
    }
    if ((e1 & kTteValid) != kTteValid) {
        printf("[krw] 页表遍历失败：L1 表项无效（bit0=0）va=0x%llx root=0x%llx idx1=%llu "
               "表项地址=0x%llx 表项值=0x%llx\n",
               (unsigned long long)va,
               (unsigned long long)root_va,
               (unsigned long long)idx1,
               (unsigned long long)addr1,
               (unsigned long long)e1);
        return false;
    }
    if ((e1 & kTteTypeMask) == kTteTypeBlock) {
        // L1 块：覆盖 2^36 = 64GB（OFFMASK 见 static_info.h:295）
        full_pa = ((e1 & kTtePaMask & ~kL1OffMask) | (va & kL1OffMask));
        return finish_mapping(c, va, page_va, e1, 1, full_pa, out);
    }

    // ---- L2 ----
    table2_pa = e1 & kTteTableMask;
    table2_va = kfdg_phystokv(c.kfd, table2_pa);
    if (table2_va == 0) {
        printf("[krw] 页表遍历失败：L1 表项指向的 L2 表物理地址 0x%llx 换算内核 VA 失败"
               "（va=0x%llx l1 表项=0x%llx）\n",
               (unsigned long long)table2_pa,
               (unsigned long long)va,
               (unsigned long long)e1);
        return false;
    }
    if ((table2_va & 0x7ull) != 0) {
        printf("[krw] 页表遍历失败：L2 表内核 VA=0x%llx 不是 8 字节对齐（l1 表项=0x%llx va=0x%llx）\n",
               (unsigned long long)table2_va,
               (unsigned long long)e1,
               (unsigned long long)va);
        return false;
    }
    idx2 = (page_va & kL2IndexMask) >> kL2Shift;
    addr2 = table2_va + idx2 * 8ull;
    if (!kread64(c, addr2, &e2)) {
        printf("[krw] 页表遍历失败：L2 表项读失败 va=0x%llx 表=0x%llx idx2=%llu 表项地址=0x%llx"
               "（l1 表项=0x%llx）\n",
               (unsigned long long)va,
               (unsigned long long)table2_va,
               (unsigned long long)idx2,
               (unsigned long long)addr2,
               (unsigned long long)e1);
        return false;
    }
    if ((e2 & kTteValid) != kTteValid) {
        printf("[krw] 页表遍历失败：L2 表项无效（bit0=0）va=0x%llx 表=0x%llx idx2=%llu "
               "表项地址=0x%llx 表项值=0x%llx（l1 表项=0x%llx）\n",
               (unsigned long long)va,
               (unsigned long long)table2_va,
               (unsigned long long)idx2,
               (unsigned long long)addr2,
               (unsigned long long)e2,
               (unsigned long long)e1);
        return false;
    }
    if ((e2 & kTteTypeMask) == kTteTypeBlock) {
        // L2 块：覆盖 2^25 = 32MB（OFFMASK 见 static_info.h:300）
        full_pa = ((e2 & kTtePaMask & ~kL2OffMask) | (va & kL2OffMask));
        return finish_mapping(c, va, page_va, e2, 2, full_pa, out);
    }

    // ---- L3（叶子）----
    table3_pa = e2 & kTteTableMask;
    table3_va = kfdg_phystokv(c.kfd, table3_pa);
    if (table3_va == 0) {
        printf("[krw] 页表遍历失败：L2 表项指向的 L3 表物理地址 0x%llx 换算内核 VA 失败"
               "（va=0x%llx l2 表项=0x%llx）\n",
               (unsigned long long)table3_pa,
               (unsigned long long)va,
               (unsigned long long)e2);
        return false;
    }
    if ((table3_va & 0x7ull) != 0) {
        printf("[krw] 页表遍历失败：L3 表内核 VA=0x%llx 不是 8 字节对齐（l2 表项=0x%llx va=0x%llx）\n",
               (unsigned long long)table3_va,
               (unsigned long long)e2,
               (unsigned long long)va);
        return false;
    }
    idx3 = (page_va & kL3IndexMask) >> kL3Shift;
    addr3 = table3_va + idx3 * 8ull;
    if (!kread64(c, addr3, &e3)) {
        printf("[krw] 页表遍历失败：L3 表项读失败 va=0x%llx 表=0x%llx idx3=%llu 表项地址=0x%llx"
               "（l2 表项=0x%llx）\n",
               (unsigned long long)va,
               (unsigned long long)table3_va,
               (unsigned long long)idx3,
               (unsigned long long)addr3,
               (unsigned long long)e2);
        return false;
    }
    if ((e3 & kPteTypeValid) != kPteTypeValid) {
        printf("[krw] 页表遍历失败：L3 页描述符无效（bit[1:0] != 0b11）va=0x%llx 表=0x%llx idx3=%llu "
               "表项地址=0x%llx 表项值=0x%llx\n",
               (unsigned long long)va,
               (unsigned long long)table3_va,
               (unsigned long long)idx3,
               (unsigned long long)addr3,
               (unsigned long long)e3);
        return false;
    }
    if ((e3 & (kPteCompressed | kPteCompressedAlt)) != 0) {
        printf("[krw] 页表遍历失败：va=0x%llx 的页被压缩（L3 表项=0x%llx 带 COMPRESSED 位，"
               "static_info.h:274-275）。此时 PA 字段不是物理页号，读会读到别人的数据、"
               "写会写坏别人的页，本文件不处理这条路\n",
               (unsigned long long)va,
               (unsigned long long)e3);
        return false;
    }
    if ((e3 & kPteTypeMask) != kPteTypeL3Block) {
        // 走到这里说明 bit[1:0]=0b11 但 bit1=0 —— 按 static_info.h:257-259 不可能出现，留作防御。
        printf("[krw] 页表遍历失败：L3 表项 0x%llx 有效但不是页描述符（va=0x%llx 表项地址=0x%llx）\n",
               (unsigned long long)e3,
               (unsigned long long)va,
               (unsigned long long)addr3);
        return false;
    }

    // L3 命中：页基址 = 表项 & 0x0000FFFFFFFFF000（bit47..12），低位 [13:0] 由 VA 提供
    full_pa = ((e3 & kTtePaMask & ~kL3OffMask) | (va & kL3OffMask));
    return finish_mapping(c, va, page_va, e3, 3, full_pa, out);
}

// ===========================================================================
// 分页中转缓冲
// ===========================================================================
// kread/kwrite 内部都把目标缓冲当 u64* 用（krkw.h:8-16 的 kread_from_method /
// :21-29 的 kwrite_from_method），所以交给 kfd 的缓冲必须 8 字节对齐、长度必须是 8 的倍数。
// 这里用一块 8 字节对齐、正好一页大的静态中转缓冲：
//   * kfd 只看这块缓冲（永远 8 对齐、长度 8 的倍数、且**不越出当前 16KB 页**）；
//   * 调用方的 buf / uaddr 想怎么对齐都行，靠 memcpy 衔接。
// ★ 单实例 → uvm_read / uvm_write 不是可重入的、也不是线程安全的（本项目单线程使用）。

inline uint8_t* stage_buffer(void)
{
    alignas(8) static uint8_t buf[kPageSize];
    return buf;
}

// 单次交给 kfdg_read 的长度上限（字节）。**默认 8**：
//   * 8 字节是真机上唯一被验证过的长度（[kfd] selftest 读 kernel base 的 magic、读 p_pid 都是 8）；
//   * perf_kread 底下是 copyout()，被读区间不能跨出 zone 元素（见文件顶部第六节），
//     8 字节 + 8 对齐 + 页内 ⇒ 必然不跨元素、也不跨页；
//   * 它是 u16 计数（perf.h:14/21），超 65535 会被静默截断，本变量另有一条 ≤65528 的检查。
// 调大是允许的（比如 4096），但要用 selftest_self 先在真机上验一遍；非法值会被 kread_span
// 拒掉并按 8 执行（打印一行），不会把不可控参数交给 kfd。
inline uint64_t read_call_max = 8;

// 从内核地址 kaddr 读 len 字节到 dst。要求 kaddr / len 都是 8 的倍数（kfd 按 u64 步进，
// krkw.h:8-16），且调用方保证 [kaddr, kaddr+len) 都在同一页里（本文件只在页内调用）。
inline bool kread_span(const Ctx& c, uint64_t kaddr, uint8_t* dst, uint64_t len)
{
    uint64_t step = read_call_max;
    uint64_t done = 0;

    if (c.kfd == 0) {
        printf("[krw] kread_span 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (dst == nullptr) {
        printf("[krw] kread_span 失败：dst=NULL\n");
        return false;
    }
    if (kaddr == 0) {
        printf("[krw] kread_span 失败：kaddr=0\n");
        return false;
    }
    if ((kaddr & 0x7ull) != 0) {
        printf("[krw] kread_span 失败：kaddr=0x%llx 不是 8 字节对齐（kread 按 u64 步进）\n",
               (unsigned long long)kaddr);
        return false;
    }
    if (len == 0) {
        printf("[krw] kread_span 失败：len=0\n");
        return false;
    }
    if ((len & 0x7ull) != 0) {
        printf("[krw] kread_span 失败：len=%llu 不是 8 的倍数（kread 按 u64 步进，余数字节会被丢掉）\n",
               (unsigned long long)len);
        return false;
    }
    if ((step == 0) || ((step & 0x7ull) != 0) || (step > 65528ull)) {
        printf("[krw] kread_span：read_call_max=%llu 非法（要求 8 的倍数且不超过 65528），"
               "这次按 8 字节执行\n",
               (unsigned long long)step);
        step = 8;
    }

    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > step) {
            chunk = step;
        }
        if (kfdg_read(c.kfd, kaddr + done, dst + (size_t)done, chunk) != 0) {
            printf("[krw] kread_span 失败：kfdg_read 参数被拒 kaddr=0x%llx len=%llu 已完成=%llu\n",
                   (unsigned long long)(kaddr + done),
                   (unsigned long long)len,
                   (unsigned long long)done);
            return false;
        }
        done += chunk;
    }
    return true;
}

// ===========================================================================
// uvm_read
// ===========================================================================
// 一次请求可能跨多页，所以按页切：
//   ① 每页先 walk_page 拿到 (pa, kva)；
//   ② 页内需要 [off, off+seg) 这一段，seg = min(剩余长度, 页内剩余)；
//   ③ 为了满足"8 对齐 + 长度 8 的倍数 + 不越页"，把这段扩到 8 字节边界：
//        word_start  = off & ~7（8 对齐，仍在页内）
//        head        = off - word_start（0..7）
//        aligned_len = ceil((head + seg) / 8) * 8（≤ 16KB，因为页尾本身 8 对齐）
//      用 kread_span 把 [kva + word_start, +aligned_len) 读进中转缓冲
//      （kread_span 内部再按 read_call_max 分片），然后 memcpy 其中 head 开始的 seg 字节；
//   ④ 页尾不足 8 字节的尾巴也走同一条路（它被包在 aligned_len 里），不需要单独处理。

inline bool uvm_read(const Ctx& c, uint64_t pmap, uint64_t uaddr, void* buf, size_t len)
{
    uint64_t root_va = 0;
    uint8_t* dst = nullptr;
    uint64_t cur = 0;
    uint64_t done = 0;

    if (c.kfd == 0) {
        printf("[krw] uvm_read 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (pmap == 0) {
        printf("[krw] uvm_read 失败：pmap=0\n");
        return false;
    }
    if (buf == nullptr) {
        printf("[krw] uvm_read 失败：buf=NULL\n");
        return false;
    }
    if (len == 0) {
        printf("[krw] uvm_read 失败：len=0\n");
        return false;
    }
    if ((uaddr >> 47) != 0) {
        printf("[krw] uvm_read 失败：uaddr=0x%llx 的 bit47=1，不是用户态 VA\n",
               (unsigned long long)uaddr);
        return false;
    }
    if (uaddr + (uint64_t)len < uaddr) {
        printf("[krw] uvm_read 失败：uaddr=0x%llx + len=%llu 溢出 64 位\n",
               (unsigned long long)uaddr,
               (unsigned long long)len);
        return false;
    }

    if (!ensure_pmap_root(c, pmap, &root_va)) {
        printf("[krw] uvm_read 失败：pmap=0x%llx 根表自校验没过，不再往下遍历\n",
               (unsigned long long)pmap);
        return false;
    }

    dst = (uint8_t*)buf;
    cur = uaddr;

    while (done < len) {
        PageMapping m;
        uint64_t off = 0;
        uint64_t in_page = 0;
        uint64_t word_start = 0;
        uint64_t head = 0;
        uint64_t aligned_len = 0;
        size_t left = 0;
        size_t seg = 0;
        uint8_t* stage = stage_buffer();

        if (!walk_page(c, root_va, cur, &m)) {
            printf("[krw] uvm_read 失败：页解析失败 va=0x%llx（已完成 %llu/%llu 字节）\n",
                   (unsigned long long)cur,
                   (unsigned long long)done,
                   (unsigned long long)len);
            return false;
        }

        off = cur & kPageMask;
        in_page = kPageSize - off;
        left = len - (size_t)done;
        seg = ((uint64_t)left < in_page) ? left : (size_t)in_page;

        word_start = off & ~0x7ull;
        head = off - word_start;
        aligned_len = ((head + (uint64_t)seg) + 7ull) & ~0x7ull;
        if (aligned_len > kPageSize) {
            // 理论上到不了：word_start + aligned_len ≤ 页尾（页尾本身是 8 的倍数）
            printf("[krw] uvm_read 失败：内部算出的跨度越页 va=0x%llx off=0x%llx seg=%llu "
                   "word_start=0x%llx aligned_len=%llu\n",
                   (unsigned long long)cur,
                   (unsigned long long)off,
                   (unsigned long long)seg,
                   (unsigned long long)word_start,
                   (unsigned long long)aligned_len);
            return false;
        }

        if (!kread_span(c, m.kva + word_start, stage, aligned_len)) {
            printf("[krw] uvm_read 失败：页内读失败 va=0x%llx kva=0x%llx word_start=0x%llx "
                   "aligned_len=%llu（页物理地址=0x%llx 级别=%d）\n",
                   (unsigned long long)cur,
                   (unsigned long long)m.kva,
                   (unsigned long long)word_start,
                   (unsigned long long)aligned_len,
                   (unsigned long long)m.pa,
                   m.level);
            return false;
        }
        memcpy(dst + (size_t)done, stage + head, seg);

        done += (uint64_t)seg;
        cur += (uint64_t)seg;
    }

    return true;
}

// ===========================================================================
// uvm_write
// ===========================================================================
// kwrite 只接受 8 字节倍数的整字，所以页内不对齐的部分必须**读-改-写**：
//   ① 把 [kva + word_start, +len8) 读进中转缓冲（同 uvm_read 的算法）；
//   ② 把调用方这段 seg 字节覆盖进缓冲的 head 位置；
//   ③ 整段 len8 再写回去。
// ★ 总开关默认关闭（allow_perf_kwrite=false），原因见文件顶部第六节：
//   kopen 之后 kwrite 走 perf_kwrite，把每个 8 字节值当 perfmon event number，
//   值不被接受时 assert_bsd → abort。先在无害页上验通再打开。
// ★ 与目标进程并发：目标自己同时写同一块内存时，我们写回的是我们读到的旧值。

inline bool allow_perf_kwrite = false;

inline void warn_once_perf_kwrite(void)
{
    static bool warned = false;

    if (warned) {
        return;
    }
    warned = true;
    printf("[krw] 提示（只打一次）：uvm_write 走 kfdg_write → kwrite，kopen 之后生效的是 perf_kwrite\n");
    printf("[krw]   （perf.h:31-65）。它把每个 8 字节值当 perfmon event number 送进内核（perf.h:56-59），\n");
    printf("[krw]   值不被接受时 assert_bsd 直接 abort（common.h:113-136）。KfdBackend.h 第五节第 3 条\n");
    printf("[krw]   对「整字为 0」的写也是这个理由。另外 RMW 与目标进程并发，写回的是我们读到的旧值。\n");
}

inline bool uvm_write(const Ctx& c, uint64_t pmap, uint64_t uaddr, const void* buf, size_t len)
{
    uint64_t root_va = 0;
    const uint8_t* src = nullptr;
    uint64_t cur = 0;
    uint64_t done = 0;

    if (!allow_perf_kwrite) {
        printf("[krw] uvm_write 拒绝：krwproc::allow_perf_kwrite=false（默认关闭）。"
               "写链路 = perf_kwrite，未在本机验证，见文件顶部第六节\n");
        return false;
    }
    if (c.kfd == 0) {
        printf("[krw] uvm_write 失败：Ctx 没 init（kfd=0）\n");
        return false;
    }
    if (pmap == 0) {
        printf("[krw] uvm_write 失败：pmap=0\n");
        return false;
    }
    if (buf == nullptr) {
        printf("[krw] uvm_write 失败：buf=NULL\n");
        return false;
    }
    if (len == 0) {
        printf("[krw] uvm_write 失败：len=0\n");
        return false;
    }
    if ((uaddr >> 47) != 0) {
        printf("[krw] uvm_write 失败：uaddr=0x%llx 的 bit47=1，不是用户态 VA\n",
               (unsigned long long)uaddr);
        return false;
    }
    if (uaddr + (uint64_t)len < uaddr) {
        printf("[krw] uvm_write 失败：uaddr=0x%llx + len=%llu 溢出 64 位\n",
               (unsigned long long)uaddr,
               (unsigned long long)len);
        return false;
    }

    warn_once_perf_kwrite();

    if (!ensure_pmap_root(c, pmap, &root_va)) {
        printf("[krw] uvm_write 失败：pmap=0x%llx 根表自校验没过，不再往下遍历\n",
               (unsigned long long)pmap);
        return false;
    }

    src = (const uint8_t*)buf;
    cur = uaddr;

    while (done < len) {
        PageMapping m;
        uint64_t off = 0;
        uint64_t in_page = 0;
        uint64_t word_start = 0;
        uint64_t head = 0;
        uint64_t aligned_len = 0;
        size_t left = 0;
        size_t seg = 0;
        uint8_t* stage = stage_buffer();

        if (!walk_page(c, root_va, cur, &m)) {
            printf("[krw] uvm_write 失败：页解析失败 va=0x%llx（已完成 %llu/%llu 字节）\n",
                   (unsigned long long)cur,
                   (unsigned long long)done,
                   (unsigned long long)len);
            return false;
        }

        off = cur & kPageMask;
        in_page = kPageSize - off;
        left = len - (size_t)done;
        seg = ((uint64_t)left < in_page) ? left : (size_t)in_page;

        word_start = off & ~0x7ull;
        head = off - word_start;
        aligned_len = ((head + (uint64_t)seg) + 7ull) & ~0x7ull;
        if (aligned_len > kPageSize) {
            printf("[krw] uvm_write 失败：内部算出的跨度越页 va=0x%llx off=0x%llx seg=%llu "
                   "word_start=0x%llx aligned_len=%llu\n",
                   (unsigned long long)cur,
                   (unsigned long long)off,
                   (unsigned long long)seg,
                   (unsigned long long)word_start,
                   (unsigned long long)aligned_len);
            return false;
        }

        // ① 先读回整段（RMW 的 R）
        if (!kread_span(c, m.kva + word_start, stage, aligned_len)) {
            printf("[krw] uvm_write 失败（RMW 读旧值失败）：va=0x%llx kva=0x%llx aligned_len=%llu"
                   "（页物理地址=0x%llx 级别=%d）\n",
                   (unsigned long long)cur,
                   (unsigned long long)m.kva,
                   (unsigned long long)aligned_len,
                   (unsigned long long)m.pa,
                   m.level);
            return false;
        }
        // ② 覆盖调用方那段字节
        memcpy(stage + head, src + (size_t)done, seg);
        // ③ 整段写回
        if (kfdg_write(c.kfd, stage, m.kva + word_start, aligned_len) != 0) {
            printf("[krw] uvm_write 失败：kwrite 参数被拒 va=0x%llx kva=0x%llx aligned_len=%llu\n",
                   (unsigned long long)cur,
                   (unsigned long long)m.kva,
                   (unsigned long long)aligned_len);
            return false;
        }

        done += (uint64_t)seg;
        cur += (uint64_t)seg;
    }

    return true;
}

// ===========================================================================
// 自检：拿本进程自己的数据做闭环
// ===========================================================================
// 页表遍历错一位，读出来的是别人的数据，而且不会"看起来错"。所以先用自己进程的一个
// 已知全局量走一遍 uvm_read，与直接内存读比对 —— 在碰游戏之前证明
// proc → task → map → pmap → 页表 整条链、以及 16KB 位域推导都是对的。

inline volatile uint64_t g_canary = 0x1122334455667788ull;

// 取址 / 取值都走 noinline：防止编译器把"读自己内存"折叠成常量（折叠掉了就测不到链路）。
__attribute__((noinline)) inline uint64_t canary_address(void)
{
    return (uint64_t)(uintptr_t)(const volatile void*)&g_canary;
}

__attribute__((noinline)) inline uint64_t canary_direct_value(void)
{
    return (uint64_t)g_canary;
}

// 跨页自检用的缓冲：只要 8 对齐，**不要求 16KB 对齐**（对象文件对最大对齐有限制，
// 例如 COFF 只到 8192，写 alignas(16384) 会直接编不过）。缓冲比一页多 16 字节就够：
// 任何长度 > 一页的区间里必定有一个 16KB 边界，且最坏情况下（边界贴着区间头 <8 字节）
// 取下一个边界，仍能保证"边界前 8 字节 + 边界后 8 字节"都落在缓冲内：
//   最坏 b = base + 16391 → b + 8 = base + 16399 ≤ base + 16400 = base + sizeof(g_pagebuf)。
// 放在 __bss（运行时填图样），只占内存不占包体。
alignas(8) inline uint8_t g_pagebuf[kPageSize + 16] = {};

inline bool selftest_self(const Ctx& c)
{
    bool ok = true;
    const int my_pid = (int)getpid();
    uint64_t proc0 = 0;
    uint64_t self = 0;
    uint64_t w = 0;
    uint64_t task = 0;
    uint64_t pmap = 0;
    uint64_t root = 0;

    printf("[krw] ======== selftest_self 开始（本进程 pid=%d）========\n", my_pid);

    if (c.kfd == 0 || c.kernel_proc == 0) {
        printf("[krw] selftest_self 失败：Ctx 没 init（kfd=0x%llx kernel_proc=0x%llx）\n",
               (unsigned long long)c.kfd,
               (unsigned long long)c.kernel_proc);
        printf("[krw] ======== selftest_self 结束：FAIL ========\n");
        return false;
    }

    // ---- 第 1 步：p_list ----
    // (a) find_proc(0) 必须回到 kernel_proc（起点那一条的 p_pid 就是 0，不依赖遍历）；
    proc0 = find_proc(c, 0);
    if (proc0 != c.kernel_proc) {
        printf("[krw] selftest 第 1 步失败：find_proc(0)=0x%llx != kernel_proc=0x%llx\n",
               (unsigned long long)proc0,
               (unsigned long long)c.kernel_proc);
        ok = false;
    }
    // (b) 找自己：pid 是唯一的外部真值，链错就找不到；
    self = find_proc(c, my_pid);
    if (self == 0) {
        printf("[krw] selftest 第 1 步失败：p_list 上没找到自己的 pid=%d\n", my_pid);
        ok = false;
    }
    // (c) 复读一次 p_pid：防"第一个字碰巧相等"。
    if (self != 0) {
        if (!kread64(c, self + kProcPid, &w)) {
            printf("[krw] selftest 第 1 步失败：复读 proc=0x%llx 的 p_pid 失败\n",
                   (unsigned long long)self);
            ok = false;
        } else if ((int32_t)(uint32_t)(w & 0xffffffffull) != (int32_t)my_pid) {
            printf("[krw] selftest 第 1 步失败：proc=0x%llx 的 p_pid=%d 与 getpid()=%d 不一致\n",
                   (unsigned long long)self,
                   (int)(int32_t)(uint32_t)(w & 0xffffffffull),
                   my_pid);
            ok = false;
        } else {
            printf("[krw] selftest 第 1 步通过：pid=%d → proc=0x%llx（p_pid 复读一致）\n",
                   my_pid,
                   (unsigned long long)self);
        }
    }
    if (!ok) {
        printf("[krw] selftest_self 结束：FAIL（p_list 这一关没过，后面几步都依赖它，直接停）\n");
        return false;
    }

    // ---- 第 2 步：proc → task → map → pmap，并做根表自校验 ----
    task = proc_task(c, self);
    if (task == 0) {
        printf("[krw] selftest 第 2 步失败：proc_task(0x%llx) 返回 0"
               "（★ proc→proc_ro/proc_ro→pr_task 两个偏移待真机验证）\n",
               (unsigned long long)self);
        printf("[krw] selftest_self 结束：FAIL\n");
        return false;
    }
    pmap = task_pmap(c, task);
    if (pmap == 0) {
        printf("[krw] selftest 第 2 步失败：task_pmap(0x%llx) 返回 0\n",
               (unsigned long long)task);
        printf("[krw] selftest_self 结束：FAIL\n");
        return false;
    }
    invalidate_pmap_cache();   // 缓存里可能是别的 pmap，这里必须真的验一遍
    if (!verify_pmap(c, pmap, &root)) {
        printf("[krw] selftest 第 2 步失败：pmap=0x%llx 根表自校验没过"
               "（pmap 地址、tte/ttep 偏移、phystokv 三者有一处不对）\n",
               (unsigned long long)pmap);
        printf("[krw] selftest_self 结束：FAIL\n");
        return false;
    }
    printf("[krw] selftest 第 2 步通过：self=0x%llx task=0x%llx pmap=0x%llx root=0x%llx\n",
           (unsigned long long)self,
           (unsigned long long)task,
           (unsigned long long)pmap,
           (unsigned long long)root);

    // ---- 第 3 步：canary 8 字节闭环 ----
    {
        const uint64_t cva = canary_address();
        const uint64_t cval = canary_direct_value();
        uint64_t got = 0;

        if (!uvm_read(c, pmap, cva, &got, (size_t)sizeof(got))) {
            printf("[krw] selftest 第 3 步失败：uvm_read(canary va=0x%llx) 返回 false\n",
                   (unsigned long long)cva);
            ok = false;
        } else {
            printf("[krw] selftest 第 3 步：canary va=0x%llx 直接读=0x%llx uvm_read=0x%llx\n",
                   (unsigned long long)cva,
                   (unsigned long long)cval,
                   (unsigned long long)got);
            if (got != cval) {
                printf("[krw] selftest 第 3 步失败：读回来的值与直接读不一致"
                       "（页表遍历或 memcpy 衔接错了）\n");
                ok = false;
            } else if (got != 0x1122334455667788ull) {
                printf("[krw] selftest 第 3 步失败：读回来的值不是初始常量 0x1122334455667788\n");
                ok = false;
            } else {
                printf("[krw] selftest 第 3 步通过：canary 闭环一致\n");
            }
        }
    }

    // ---- 第 4 步：不对齐读（VA 不 8 对齐、长度 5，考验中转缓冲的 head 处理）----
    {
        const uint64_t cva = canary_address();
        uint8_t got[8];
        uint8_t expect[8];

        memset(got, 0, sizeof(got));
        memset(expect, 0xee, sizeof(expect));
        memcpy(expect, (const void*)(uintptr_t)(cva + 3), 5);

        if (!uvm_read(c, pmap, cva + 3, got, 5)) {
            printf("[krw] selftest 第 4 步失败：uvm_read(不对齐 va=0x%llx len=5) 返回 false\n",
                   (unsigned long long)(cva + 3));
            ok = false;
        } else if (memcmp(got, expect, 5) != 0) {
            printf("[krw] selftest 第 4 步失败：不对齐读回来的 5 字节与直接内存读不一致\n");
            ok = false;
        } else {
            printf("[krw] selftest 第 4 步通过：不对齐读（va+3，5 字节）一致\n");
        }
    }

    // ---- 第 5 步：跨页读（在 g_pagebuf 里找一个 16KB 边界，从边界前 8 字节读 16 字节）----
    {
        uint64_t i = 0;
        uint8_t got[16];
        uint8_t expect[16];
        const uint64_t base = (uint64_t)(uintptr_t)&g_pagebuf[0];
        const uint64_t limit = base + (uint64_t)sizeof(g_pagebuf);
        // 第一个 ≥ base 的 16KB 边界；如果它离缓冲头不到 8 字节（前面凑不出 8 字节），取下一个。
        uint64_t boundary = (base + kPageMask) & ~kPageMask;
        uint64_t pva = 0;

        if (boundary < base + 8ull) {
            boundary += kPageSize;
        }
        if ((boundary < base + 8ull) || (boundary + 8ull > limit)) {
            printf("[krw] selftest 第 5 步失败：g_pagebuf（base=0x%llx len=%llu）里找不到"
                   "可用的 16KB 边界，跨页这一项没法测\n",
                   (unsigned long long)base,
                   (unsigned long long)sizeof(g_pagebuf));
            ok = false;
            boundary = 0;
        }
        pva = (boundary != 0) ? (boundary - 8ull) : 0;

        if (boundary != 0) {
            for (i = 0; i < (uint64_t)sizeof(g_pagebuf); i++) {
                g_pagebuf[i] = (uint8_t)((i * 31ull + 7ull) & 0xffull);
            }
            memset(got, 0, sizeof(got));
            memset(expect, 0, sizeof(expect));
            memcpy(expect, (const void*)(uintptr_t)pva, sizeof(expect));

            if (!uvm_read(c, pmap, pva, got, sizeof(got))) {
                printf("[krw] selftest 第 5 步失败：uvm_read(跨页 va=0x%llx len=16) 返回 false\n",
                       (unsigned long long)pva);
                ok = false;
            } else if (memcmp(got, expect, sizeof(got)) != 0) {
                printf("[krw] selftest 第 5 步失败：跨页读回来的 16 字节与直接内存读不一致"
                       "（分页切分或页内偏移错了）\n");
                ok = false;
            } else {
                printf("[krw] selftest 第 5 步通过：跨页读（0x%llx 起 16 字节，跨 16KB 边界）一致\n",
                       (unsigned long long)pva);
            }
        }
    }

    // ---- 第 6 步：写闭环（默认关闭，见文件顶部第六节）----
    if (!allow_perf_kwrite) {
        printf("[krw] selftest 第 6 步跳过：uvm_write 默认关闭（krwproc::allow_perf_kwrite=false）\n");
    } else {
        const uint64_t cva = canary_address();
        const uint64_t old = canary_direct_value();
        const uint64_t nv = 0xa1b2c3d4e5f60718ull;
        uint64_t now = 0;

        if (!uvm_write(c, pmap, cva, &nv, (size_t)sizeof(nv))) {
            printf("[krw] selftest 第 6 步失败：uvm_write(canary) 返回 false\n");
            ok = false;
        } else {
            now = canary_direct_value();
            printf("[krw] selftest 第 6 步：写入 0x%llx → 直接读回 0x%llx\n",
                   (unsigned long long)nv,
                   (unsigned long long)now);
            if (now != nv) {
                printf("[krw] selftest 第 6 步失败：写入后直接读不回同一个值"
                       "（perf_kwrite 的值域问题，见文件顶部第六节）\n");
                ok = false;
            }
        }
        // 无论成败都尝试还原 canary
        if (!uvm_write(c, pmap, cva, &old, (size_t)sizeof(old))) {
            printf("[krw] selftest 第 6 步：★ 还原 canary 失败，它现在的值不是 0x%llx\n",
                   (unsigned long long)old);
            ok = false;
        } else if (canary_direct_value() != old) {
            printf("[krw] selftest 第 6 步：★ 还原后直接读到的值 0x%llx 与原来 0x%llx 不同\n",
                   (unsigned long long)canary_direct_value(),
                   (unsigned long long)old);
            ok = false;
        } else {
            printf("[krw] selftest 第 6 步通过：写 → 回读 → 还原 全链路一致\n");
        }
    }

    printf("[krw] ======== selftest_self 结束：%s ========\n", ok ? "PASS" : "FAIL");
    return ok;
}

} // namespace krwproc
