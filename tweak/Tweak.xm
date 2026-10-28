// tweak/Tweak.xm —— 越狱版注入插件（目标：暗区突围国际服 UAGame）
//
// 为什么越狱之后这条路能通：
//   越狱会打掉 AMFI 的代码签名强制与 JIT 限制，所以 dylib 可以被注入并执行，
//   不再需要之前那套手动映射 + 线程劫持 + 结果槽的绕行方案。
//
// 现阶段目标（按顺序，每步都要在设备上验证）：
//   1. 插件确实进了游戏进程（写面包屑）
//   2. 能在游戏里创建 UIWindow 并显示一个色块（浮窗能力）
//   3. 读到一个已知地址，验证内存读取（为后面读 GWorld 做准备）
//
// 注意：这里刻意不写死任何游戏偏移。偏移必须由 tools/ue4_find_globals.py
// 分析真实二进制后填入，否则就是编造。

#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

// ---------------- 日志 ----------------
static NSString* arLogPath(void) {
    // 越狱环境下优先写全局可读路径，方便从 SSH 直接 cat
    NSArray* cands = @[
        @"/var/mobile/ar_tweak.log",
        @"/var/tmp/ar_tweak.log",
        [NSTemporaryDirectory() stringByAppendingPathComponent:@"ar_tweak.log"],
    ];
    for (NSString* p in cands) {
        NSString* dir = [p stringByDeletingLastPathComponent];
        if ([[NSFileManager defaultManager] fileExistsAtPath:dir]) return p;
    }
    return nil;
}

static void ARLog(NSString* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString* msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);

    NSString* line = [NSString stringWithFormat:@"[%@] %@\n", [NSDate date], msg];
    NSString* path = arLogPath();
    if (path) {
        NSFileHandle* fh = [NSFileHandle fileHandleForWritingAtPath:path];
        if (!fh) {
            [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
            fh = [NSFileHandle fileHandleForWritingAtPath:path];
        }
        if (fh) {
            @try {
                [fh seekToEndOfFile];
                [fh writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
                [fh closeFile];
            } @catch (...) {}
        }
    }
    NSLog(@"[ar] %@", msg);
}

// ---------------- 模块信息 ----------------
static uintptr_t arMainImageBase(void) {
    // 主可执行文件是第 0 个镜像
    return (uintptr_t)_dyld_get_image_header(0);
}

static const char* arMainImagePath(void) {
    return _dyld_get_image_name(0);
}

// 在指定镜像里按名字找符号（导出符号或本地符号）
static uintptr_t arFindSymbol(uintptr_t base, const char* name) {
    const struct mach_header_64* mh = (const struct mach_header_64*)base;
    if (mh->magic != MH_MAGIC_64) return 0;

    const uint8_t* p = (const uint8_t*)mh + sizeof(*mh);
    const struct symtab_command* sym = NULL;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SYMTAB) { sym = (const struct symtab_command*)lc; break; }
        p += lc->cmdsize;
    }
    if (!sym) return 0;

    const struct nlist_64* nl = (const struct nlist_64*)((const uint8_t*)mh + sym->symoff);
    const char* strs = (const char*)((const uint8_t*)mh + sym->stroff);
    for (uint32_t i = 0; i < sym->nsyms; i++) {
        if (nl[i].n_un.n_strx == 0) continue;
        if ((nl[i].n_type & N_TYPE) != N_SECT) continue;
        const char* nm = strs + nl[i].n_un.n_strx;
        if (nm[0] == '_') nm++;
        if (strcmp(nm, name) == 0) return base + nl[i].n_value;
    }
    return 0;
}

// ---------------- 浮窗 ----------------
static UIWindow* g_arWindow = nil;

static void arShowOverlay(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        @try {
            UIWindowScene* scene = nil;
            for (UIScene* s in [UIApplication sharedApplication].connectedScenes) {
                if ([s isKindOfClass:[UIWindowScene class]]) { scene = (UIWindowScene*)s; break; }
            }
            if (!scene) { ARLog(@"没有可用 UIWindowScene，浮窗放弃"); return; }

            g_arWindow = [[UIWindow alloc] initWithWindowScene:scene];
            g_arWindow.frame = scene.coordinateSpace.bounds;
            g_arWindow.windowLevel = 20000000.0;
            g_arWindow.backgroundColor = [UIColor clearColor];

            UIViewController* vc = [UIViewController new];
            vc.view.backgroundColor = [UIColor clearColor];

            // 屏幕中央一个方块，作为“浮窗已生效”的可见证据
            UIView* box = [[UIView alloc] initWithFrame:CGRectMake(
                (g_arWindow.bounds.size.width - 120) / 2,
                (g_arWindow.bounds.size.height - 120) / 2, 120, 120)];
            box.backgroundColor = [UIColor colorWithRed:1.0 green:0.1 blue:0.2 alpha:0.65];
            box.layer.borderColor = [UIColor whiteColor].CGColor;
            box.layer.borderWidth = 2.0;
            [vc.view addSubview:box];

            g_arWindow.rootViewController = vc;
            g_arWindow.hidden = NO;
            ARLog(@"浮窗已创建并显示，bounds=%@", NSStringFromCGRect(g_arWindow.bounds));
        } @catch (NSException* e) {
            ARLog(@"浮窗创建异常: %@", e);
        }
    });
}

// ---------------- 入口 ----------------
%ctor {
    ARLog(@"=== tweak 已加载进进程 pid=%d ===", getpid());
    ARLog(@"镜像路径=%s", arMainImagePath());

    uintptr_t base = arMainImageBase();
    ARLog(@"主镜像基址=0x%lx", (unsigned long)base);

    // 这一步验证“能在游戏进程里读主镜像的符号表”
    uintptr_t mainSym = arFindSymbol(base, "main");
    ARLog(@"符号 main = 0x%lx", (unsigned long)mainSym);

    arShowOverlay();
}
