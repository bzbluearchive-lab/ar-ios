// payload_manual/main.mm —— 手动映射用的最小 payload
//
// 关键约束：**不定义任何新的 ObjC 类**。
// 手动映射绕过了 dyld，也就绕过了 ObjC 的 image 注册，我们自己定义的类
// 不会进入 runtime。所以这里只用系统已有的类（UIWindow / UIView / NSString 等），
// 通过 objc_msgSend 调用即可。
//
// 当前版本只做两件事，用来验证手动映射是否成功：
//   1. 写面包屑文件（证明代码真的在目标进程里跑起来了）
//   2. 建一个红色方块窗口（证明能在这个进程里画东西）

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <unistd.h>

// objc_msgSend 的强类型包装（避免 ARC 下的可变参数问题）
static id  msg0(id o, const char* sel)                        { return ((id(*)(id,SEL))objc_msgSend)(o, sel_registerName(sel)); }
static id  msg1(id o, const char* sel, id a)                  { return ((id(*)(id,SEL,id))objc_msgSend)(o, sel_registerName(sel), a); }
static void msgb(id o, const char* sel, BOOL a)               { ((void(*)(id,SEL,BOOL))objc_msgSend)(o, sel_registerName(sel), a); }
static void msgf(id o, const char* sel, float a)              { ((void(*)(id,SEL,float))objc_msgSend)(o, sel_registerName(sel), a); }
static void __attribute__((unused)) msgi(id o, const char* sel, long a)               { ((void(*)(id,SEL,long))objc_msgSend)(o, sel_registerName(sel), a); }
static void __attribute__((unused)) msgr(id o, const char* sel, CGRect r)             { ((void(*)(id,SEL,CGRect))objc_msgSend)(o, sel_registerName(sel), r); }

static void writeBreadcrumb(const char* tag) {
    @autoreleasepool {
        NSString* p = [NSTemporaryDirectory() stringByAppendingPathComponent:
                       [NSString stringWithFormat:@"ar_manual_%d.txt", getpid()]];
        NSString* line = [NSString stringWithFormat:@"%@ %s pid=%d\n",
                          [NSDate date], tag, getpid()];
        NSData* d = [line dataUsingEncoding:NSUTF8StringEncoding];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p])
            [[NSFileManager defaultManager] createFileAtPath:p contents:nil attributes:nil];
        NSFileHandle* fh = [NSFileHandle fileHandleForWritingAtPath:p];
        if (fh) { @try { [fh seekToEndOfFile]; [fh writeData:d]; [fh closeFile]; } @catch (...) {} }
    }
}

static void buildOverlay(void) {
    @autoreleasepool {
        id app = msg0((id)objc_getClass("UIApplication"), "sharedApplication");
        if (!app) { writeBreadcrumb("no_uiapplication"); return; }

        id scene = nil;
        id scenes = msg0(app, "connectedScenes");
        if (scenes) {
            id arr = msg0(scenes, "allObjects");
            if (arr && msg0(arr, "count")) {
                scene = ((id(*)(id,SEL,unsigned long))objc_msgSend)(arr, sel_registerName("objectAtIndex:"), 0);
            }
        }

        id win = msg0((id)objc_getClass("UIWindow"), "alloc");
        if (scene) {
            win = ((id(*)(id,SEL,id))objc_msgSend)(win, sel_registerName("initWithWindowScene:"), scene);
        } else {
            id screen = msg0((id)objc_getClass("UIScreen"), "mainScreen");
            CGRect b = ((CGRect(*)(id,SEL))objc_msgSend)(screen, sel_registerName("bounds"));
            win = ((id(*)(id,SEL,CGRect))objc_msgSend)(win, sel_registerName("initWithFrame:"), b);
        }
        if (!win) { writeBreadcrumb("no_window"); return; }

        id screen = msg0((id)objc_getClass("UIScreen"), "mainScreen");
        CGRect bounds = ((CGRect(*)(id,SEL))objc_msgSend)(screen, sel_registerName("bounds"));

        msgf(win, "setWindowLevel:", 20000000.0f);

        // 一个红色方块，居中
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
            id red = msg0((id)objc_getClass("UIColor"), "clearColor");
            id view = msg0(vc, "view");
            if (view && red) msg1(view, "setBackgroundColor:", red);
            if (view && box) msg1(view, "addSubview:", box);
            msg1(win, "setRootViewController:", vc);
        }

        msgb(win, "setHidden:", NO);
        writeBreadcrumb("window_built");
    }
}

// 手动映射后由注入方调用这个入口
extern "C" __attribute__((visibility("default")))
void arp_entry(void) {
    writeBreadcrumb("entry_called");
    buildOverlay();
}
