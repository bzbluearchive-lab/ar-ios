// payload_manual/main.mm —— 手动映射用的最小 payload
//
// 设计原则（每一条都是踩坑换来的）：
//
// 1) 不定义任何 ObjC 类：手动映射绕过 dyld，绕过了 ObjC image 注册，
//    自己定义的类不会进 runtime。
//
// 2) 不用全局变量：手动映射时我们自己的 chained-fixups 解析对 rebase 处理不完整
//    （日志里 rebase=0），依赖全局变量的地址会被解析成 0，
//    往地址 0 写会直接把目标进程写死。所以一切数据都通过寄存器参数传入。
//
// 3) 不用裸 svc：G37 那次用 svc #0x80 直接被系统杀掉（SIGSYS）。
//    需要系统能力时用 bind 解析出来的 libc 函数（bind 已验证正确）。
//
// 4) 入口绝不返回：返回会跳回 LR(=0)，目标进程崩。
//    用自旋代替 return。
//
// 5) 不阻塞、不 sleep：这段代码是在借用/新建的线程上短暂执行的，
//    任何阻塞都会拖慢甚至卡死目标。
//
// 当前内容只做一件事：把几个魔数写进调用方给的内存槽，供注入方回读。
// 这是验证"代码能否执行 + 能否写内存"的最小充分条件。

#include <stdint.h>

#define AR_MAGIC_ENTERED 0xC0FFEE11L
#define AR_MAGIC_DONE    0xDEADBEEFL

// 自旋（不访问内存、不调用任何函数）：
// 用来验证"映射进来的可执行页能否被执行"。
extern "C" __attribute__((visibility("default")))
void arp_spin(void) {
    volatile unsigned long n = 0;
    for (;;) { n++; }
}

// 结果槽协议：
//   slot[0] = AR_MAGIC_ENTERED  代码开始执行
//   slot[1] = 0x1122334455667788 固定值，用于校验写入完整性
//   slot[3] = AR_MAGIC_DONE     全部走完
// slot 由注入方 vm_allocate 后通过 x0 传入，确定可写。
extern "C" __attribute__((visibility("default")))
void arp_probe(void* slot) {
    if (slot) {
        volatile long* p = (volatile long*)slot;
        p[0] = AR_MAGIC_ENTERED;
        p[1] = 0x1122334455667788L;
        p[3] = AR_MAGIC_DONE;
    }
    volatile unsigned long n = 0;
    for (;;) { n++; }
}
