// kfd_glue.h —— libkfd 的 C 门面（C 与 C++ 都能 include）
//
// 位置：src/Kernel/kfd_glue.h
// 作用：把 vendor/kfd161 的 libkfd 收束到**一个** C 翻译单元里，对外只暴露下面这些
//       纯 C 函数。C++ 侧（src/Kernel/KfdBackend.h）只 include 本文件，
//       永远不 include 任何 vendor 头。
//
// 为什么必须这么拆（展开写在 KfdBackend.h 顶部第一节）：
//   ① vendor/kfd161 是 **C** 代码，不是 C++。perf.h:47/62/63/64 那四行
//      `*event = (volatile struct perfmon_event){};` 是 C99 复合字面量；
//      在 C++ 里它变成"用临时量给 volatile 聚合体赋值"，clang 直接报
//      no viable overloaded '='。所以在 .mm 里 include libkfd.h 是错的。
//   ② libkfd.h 里带**函数定义**（kopen / kread / kwrite / kclose）与**数据定义**
//      （dynamic_info.h:111 的 kern_versions[]、static_info.h:10 的 t1sz_boot），
//      所以它只能被一个 TU include；两个 TU 各 include 一次就是成批 duplicate symbol。
//   ③ 于是分工固定为：kfd_glue.c（按 C 编译，唯一 include vendor 的 TU）→ 本头 → C++ 侧。
//
// 约定：
//   * 本文件只 include <stdint.h>，不引入任何 Apple 头 / vendor 头（C 与 C++ 都干净）。
//   * 句柄就是 kopen() 的返回值原样透传；0 表示无效句柄。
//   * 每个函数的失败分支都会 printf 一行 "[kfdg] ..." 讲清原因，不静默返回。
//   * 长度一律是字节数，且必须是 8 的倍数（kfd 内部按 u64 步进，见 krkw.h:8-16）；
//     不是 8 的倍数时直接拒绝，不把不可控参数交给 kfd。

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 打开内核读写通道。
//   参数：puaf_pages —— PUAF 页数，必须落在 [16, 65536]（libkfd.h:173-174 的 assert）。
//         method     —— 门面对外的编号：0 = physpuppet、1 = landa、2 = smith。
//                       ★ 这三个数字与 libkfd 的枚举顺序**不同**（libkfd.h:21-25 里
//                       smith=1、landa=2），实现里按名字映射，调用方只认 0/1/2。
//   返回：kopen() 的句柄；失败返回 0（原因已打印）。
uint64_t kfdg_open(uint64_t puaf_pages, int method);

// 关闭句柄，释放 PUAF / KRKW / perf 的全部资源。handle 为 0 时只打印一行、直接返回。
void kfdg_close(uint64_t handle);

// 内核读：把内核地址 kaddr 处的 size 字节拷到用户态 uaddr。
//   返回：0 = 调用已发出；非 0 = 参数非法（句柄 0 / 指针空 / size 不是 8 的倍数）。
//   ★ kread 本身没有返回值，所以"返回 0"只代表参数合法 + 调用发出，
//     数据对不对必须由调用方自己校验（KfdBackend 里就是 magic / p_pid 两道）。
int kfdg_read(uint64_t handle, uint64_t kaddr, void* uaddr, uint64_t size);

// 内核写：把用户态 uaddr 处的 size 字节写到内核地址 kaddr。
//   ★ 参数顺序与 libkfd.h:38 的 kwrite 一致：(句柄, 用户态源, 内核态目的, 长度)。
//   返回：0 = 调用已发出；非 0 = 参数非法（同上）。
int kfdg_write(uint64_t handle, void* uaddr, uint64_t kaddr, uint64_t size);

// 在 vendor/kfd161 的 kern_versions[] 里按 kern_version 逐字查找（strcmp）。
//   返回：命中下标（>= 0）；未命中 -1（原因已打印，含表内条目数）。
int kfdg_version_index(const char* kern_version);

// 内核基址 = kern_versions[kfd->info.env.vid].kernelcache__kernel_base + perf.kernel_slide
//            （与 perf.h:99-101 同一算法）。0 表示算不出来（原因已打印）。
uint64_t kfdg_kernel_base(uint64_t handle);

// kfd 自己解析出来的三个关键 kaddr（纯读成员，零风险）：
//   kfdg_current_proc —— 本进程 struct proc 的内核地址（info.kaddr.current_proc）
//   kfdg_kernel_proc  —— 内核 launchd 那条 struct proc 的地址（info.kaddr.kernel_proc）
//   kfdg_kernel_slide —— 内核 slide（perf.kernel_slide）
// 句柄为 0 时一律返回 0。
uint64_t kfdg_current_proc(uint64_t handle);
uint64_t kfdg_kernel_proc(uint64_t handle);
uint64_t kfdg_kernel_slide(uint64_t handle);

// 读一个 8 字节字。失败返回 0（失败原因已打印）。
// ★ 0 同时是合法的内核数据，所以调用方不能只靠返回值 0 判断失败 ——
//   KfdBackend 在 kernel_base 里就是"读到 0xfeedfacf 才算成功"。
uint64_t kfdg_read_u64(uint64_t handle, uint64_t kaddr);

#ifdef __cplusplus
}
#endif
