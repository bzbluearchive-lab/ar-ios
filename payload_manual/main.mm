// payload_manual/main.mm —— 手动映射用的最小 payload
//
// 三条铁律（全部是踩过的坑）：
//
// 1) 不定义任何新的 ObjC 类：手动映射绕过了 dyld 的 image 注册，
//    自己定义的类不会进 ObjC runtime。
//
// 2) 面包屑必须用**裸系统调用**（svc #0x80），不经过任何外部符号。
//    原因：我们手动映射时要自己解析 chained fixups 的 bind，
//    这一步算错就会把 open/write 绑到错误的地址，一调用就崩。
//    用 svc 直连内核就完全绕开这个不确定性 ——
//    只要面包屑出现，就证明"代码能执行 + 能进内核"，
//    剩下的失败就一定在 ObjC/bind 那一层。
//
// 3) 入口绝不返回：返回会跳回 LR(=0) 导致目标进程崩溃。

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <unistd.h>

// ---------------- Darwin arm64 裸系统调用 ----------------
// svc #0x80，调用号放 x16，参数 x0..x2，返回值在 x0。
// BSD 号：write=4, open=5, close=6
static inline long ar_sys3(long n, long a, long b, long c) {
    register long x16 __asm__("x16") = n;
    register long x0  __asm__("x0")  = a;
    register long x1  __asm__("x1")  = b;
    register long x2  __asm__("x2")  = c;
    __asm__ volatile("svc #0x80"
                     : "+r"(x0)
                     : "r"(x16), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
}

static inline unsigned long ar_strlen(const char* s) {
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

// O_WRONLY(1) | O_CREAT(0x200) | O_APPEND(0x8) = 0x209
static void rawNote(const char* path, const char* text) {
    if (!path || !text) return;
    long fd = ar_sys3(5 /*open*/, (long)path, 0x209, 0644);
    if (fd < 0) return;
    ar_sys3(4 /*write*/, fd, (long)text, (long)ar_strlen(text));
    ar_sys3(6 /*close*/, fd, 0, 0);
}

// ---------------- 第二层：ObjC（这一层依赖 bind，可能是失败点） ----------------
static id  msg0(id o, const char* sel)                        { return ((id(*)(id,SEL))objc_msgSend)(o, sel_registerName(sel)); }
static id  msg1(id o, const char* sel, id a)                  { return ((id(*)(id,SEL,id))objc_msgSend)(o, sel_registerName(sel), a); }
static void msgb(id o, const char* sel, BOOL a)               { ((void(*)(id,SEL,BOOL))objc_msgSend)(o, sel_registerName(sel), a); }
static void msgf(id o, const char* sel, float a)              { ((void(*)(id,SEL,float))objc_msgSend)(o, sel_registerName(sel), a); }

static void buildOverlay(const char* notePath) {
    @autoreleasepool {
        rawNote(notePath, "objc_begin\n");

        id app = msg0((id)objc_getClass("UIApplication"), "sharedApplication");
        if (!app) { rawNote(notePath, "fail_no_uiapplication\n"); return; }

        id scene = nil;
        id scenes = msg0(app, "connectedScenes");
        if (scenes) {
            id arr = msg0(scenes, "allObjects");
            if (arr && msg0(arr, "count")) {
                scene = ((id(*)(id,SEL,unsigned long))objc_msgSend)(arr, sel_registerName("objectAtIndex:"), 0);
            }
        }

        id screen = msg0((id)objc_getClass("UIScreen"), "mainScreen");
        CGRect bounds = ((CGRect(*)(id,SEL))objc_msgSend)(screen, sel_registerName("bounds"));

        id win = msg0((id)objc_getClass("UIWindow"), "alloc");
        if (scene) {
            win = ((id(*)(id,SEL,id))objc_msgSend)(win, sel_registerName("initWithWindowScene:"), scene);
        } else {
            win = ((id(*)(id,SEL,CGRect))objc_msgSend)(win, sel_registerName("initWithFrame:"), bounds);
        }
        if (!win) { rawNote(notePath, "fail_no_window\n"); return; }

        msgf(win, "setWindowLevel:", 20000000.0f);

        id box = msg0((id)objc_getClass("UIView"), "alloc");
        CGRect r = CGRectMake(bounds.size.width / 2 - 60, bounds.size.height / 2 - 60, 120, 120);
        box = ((id(*)(id,SEL,CGRect))objc_msgSend)(box, sel_registerName("initWithFrame:"), r);
        if (box) {
            id color = ((id(*)(id,SEL,double,double,double,double))objc_msgSend)(
                (id)objc_getClass("UIColor"),
                sel_registerName("colorWithRed:green:blue:alpha:"), 1.0, 0.1, 0.2, 0.75);
            msg1(box, "setBackgroundColor:", color);
        }

        id vc = msg0((id)objc_getClass("UIViewController"), "alloc");
        vc = msg0(vc, "init");
        if (vc) {
            id clear = msg0((id)objc_getClass("UIColor"), "clearColor");
            id view = msg0(vc, "view");
            if (view && clear) msg1(view, "setBackgroundColor:", clear);
            if (view && box)   msg1(view, "addSubview:", box);
            msg1(win, "setRootViewController:", vc);
        }

        msgb(win, "setHidden:", NO);
        rawNote(notePath, "window_built\n");
    }
}


// 最小入口：只做裸 syscall 面包屑，然后自旋。
//   - 不碰 ObjC、不碰 bind、不碰 TLS
//   - 用来单独回答一个问题：从"手动映射进来的匿名可执行页"里
//     直接 svc #0x80 能不能成功？
//   能写出 note_ok 就说明整条执行链（映射→执行→进内核）都通，
//   剩下的问题只会在 ObjC 层。
extern "C" __attribute__((visibility("default")))
void arp_note(const char* notePath) {
    rawNote(notePath, "note_ok\n");
    volatile unsigned long n = 0;
    for (;;) { n++; }
}

// 纯自旋：不调用任何外部函数，用来验证"映射进来的代码能否被执行"
extern "C" __attribute__((visibility("default")))
void arp_spin(void) {
    volatile unsigned long n = 0;
    for (;;) { n++; }
}

// 注入方通过 x0 传入面包屑路径
extern "C" __attribute__((visibility("default")))
void arp_entry(const char* notePath) {
    rawNote(notePath, "entry_raw_ok\n");     // 裸 syscall：不依赖任何 bind
    buildOverlay(notePath);
    rawNote(notePath, "entry_done\n");
    for (;;) sleep(3600);                    // 绝不返回
}
