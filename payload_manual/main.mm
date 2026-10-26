// payload_manual/main.mm —— 手动映射用的最小 payload
//
// 两条铁律（都是踩过的坑）：
//
// 1) **不定义任何新的 ObjC 类**。手动映射绕过 dyld，也就绕过了 ObjC 的 image
//    注册，我们自己定义的类不会进 runtime。所以只用系统已有的类。
//
// 2) **入口第一件事必须用原始 open/write 写面包屑**，不许碰 Foundation。
//    arm64 上 x18 是 TLS 指针，注入线程的 x18 未必有效，
//    而 @autoreleasepool / NSString 一上来就要访问 TLS —— 会直接 fault。
//    先用纯 syscall 落盘，才能确认"代码到底有没有跑起来"。
//
// 面包屑路径由注入方通过 x0 传进来（那是目标自己容器里的路径，沙盒可写）。

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// ---------------- 第一层：纯 syscall，不碰 TLS ----------------
static void rawNote(const char* path, const char* text) {
    if (!path || !text) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    (void)w;
    close(fd);
}

// ---------------- 第二层：才用 ObjC ----------------
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

// 注入方通过 x0 传入面包屑路径
extern "C" __attribute__((visibility("default")))
void arp_entry(const char* notePath) {
    rawNote(notePath, "entry_raw_ok\n");     // 只要这行出现，就证明代码真跑了
    buildOverlay(notePath);
    rawNote(notePath, "entry_done\n");
    for (;;) sleep(3600);                    // 绝不返回：返回会跳回 LR(=0)
}
