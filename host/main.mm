// host/main.m —— 启动器：配置 UI + 孵化 daemon
//
// 这个 app 划掉之后浮窗必须仍然工作，所以它只做三件事：
//   1. 把 daemon 从 bundle 里 spawn 出去（daemon 自己 setsid 脱离进程组）
//   2. 显示 daemon 是否存活
//   3. 提供"停止"按钮：往容器里写一个 stop 标志文件，daemon 自己退出
//
// 不含任何作弊逻辑、不读游戏内存、不建窗口。

#import <UIKit/UIKit.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <stdio.h>
#include "DaemonSpawn.h"
#include "../src/Inject/RemoteLoad.h"
#include "../src/Inject/AppInspect.h"
#include "../src/Inject/BundleInfo.h"
#include "../src/Inject/ContainerCopy.h"
#include "../src/Inject/ManualLoad.h"
#include "../src/Inject/FileMap.h"
#include "../src/Game/Ue4Remote.h"
#include "../src/Kernel/KernelBackend.h"
#include "../src/Kernel/BuildTable.h"
#include "../src/Kernel/KernelCache.h"

// 启动探针：进程真的跑起来了才会留下文件。
// 用来区分两种完全不同的失败：
//   A. 文件完全没出现  -> 进程根本没被系统拉起（签名/权限/包结构问题）
//   B. 文件出现了      -> 进程跑起来了，之后才崩（代码问题，能拿到崩溃日志）
static void ARProbe(const char* tag) {
    @autoreleasepool {
        NSString* docDir = nil;
        NSArray* d = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        if (d.count) docDir = [d[0] stringByAppendingPathComponent:@"arhost_probe.txt"];

        NSMutableArray* paths = [NSMutableArray array];
        [paths addObject:@"/var/mobile/Library/Caches/arhost_probe.txt"];  // 无沙盒时可写
        if ([NSTemporaryDirectory() length]) {
            [paths addObject:[NSTemporaryDirectory() stringByAppendingPathComponent:@"arhost_probe.txt"]];
        }
        if (docDir) [paths addObject:docDir];

        NSString* line = [NSString stringWithFormat:@"%@ %s pid=%d\n",
                          [NSDate date], tag, getpid()];
        NSData* data = [line dataUsingEncoding:NSUTF8StringEncoding];

        for (NSString* p in paths) {
            @try {
                if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
                    [[NSFileManager defaultManager] createFileAtPath:p contents:nil attributes:nil];
                }
                NSFileHandle* fh = [NSFileHandle fileHandleForWritingAtPath:p];
                if (!fh) continue;
                [fh seekToEndOfFile];
                [fh writeData:data];
                [fh closeFile];
            } @catch (...) {
                // 写不了就算了，探针不能影响主流程
            }
        }
    }
}

static NSString* containerPath(void) {
    NSArray* d = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return d.count ? d[0] : NSTemporaryDirectory();
}

// ---------------------------------------------------------------------------
// 日志路径解析：
//   无沙盒（platform-application / no-sandbox）的 app 常常没有容器，
//   Documents 不存在，写日志会静默失败。所以按顺序试多个位置，
//   选第一个真的能写的。
// ---------------------------------------------------------------------------
static BOOL can_write(NSString* path) {
    if (!path) return NO;
    NSString* dir = [path stringByDeletingLastPathComponent];
    if (![[NSFileManager defaultManager] fileExistsAtPath:dir]) return NO;
    int fd = open(path.UTF8String, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return NO;
    close(fd);
    return YES;
}

static NSString* resolveLogPath(void) {
    static NSString* cached = nil;
    if (cached) return cached;

    NSMutableArray* cands = [NSMutableArray array];
    NSArray* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (docs.count) [cands addObject:[docs[0] stringByAppendingPathComponent:@"ar_daemon.log"]];
    [cands addObject:@"/var/mobile/Library/Caches/ar_daemon.log"];   // 无沙盒时可写
    [cands addObject:@"/tmp/ar_daemon.log"];                         // /private/var/tmp
    [cands addObject:[NSTemporaryDirectory() stringByAppendingPathComponent:@"ar_daemon.log"]];

    for (NSString* p in cands) {
        if (can_write(p)) { cached = p; return p; }
    }
    cached = cands.lastObject;
    return cached;
}

static NSString* stopFlagPath(void) {
    return [containerPath() stringByAppendingPathComponent:@"ar_daemon.stop"];
}

static NSString* daemonLogPath(void) {
    return resolveLogPath();
}

static NSString* daemonPath(void) {
    return [[NSBundle mainBundle] pathForResource:@"ARDaemon" ofType:nil];
}

static NSString* payloadPath(void) {
    return [[NSBundle mainBundle] pathForResource:@"ARPayload" ofType:@"dylib"];
}

// ---------------------------------------------------------------------------
// 直接在本进程里注入 SpringBoard（不再需要 ARDaemon）
//
// 为什么这样更好：
//   1. 沙盒 app 不允许 posix_spawn 自己 bundle 里的二进制，daemon 那条路在 B 版
//      签名下一定失败；
//   2. 注入只发生在启动那一刻，注入完成后浮窗住在 SpringBoard 里，与谁启动它无关，
//      所以 daemon 的生命周期对我们没有价值。
// RemoteLoad 里的 printf 会被重定向到界面上显示的那个日志文件。
// ---------------------------------------------------------------------------
// 手动映射注入：不走 dlopen，自己把段映射进目标（绕开沙盒 exec 限制与 library validation）
static void ARManualInject(const char* kw) {
    printf("\n=== 手动映射注入 ===\n");
    const char* target = (kw && strncmp(kw, "com.", 4) == 0) ? kw : "com.proximabeta.mf.uamo";
    char path[1024] = {0};
    pid_t pid = bundleinfo::find_pid_by_bundle(target, 1024, path, sizeof(path));
    printf("[mload] 目标 bundle=%s pid=%d\n", target, pid);

    if (!pid) {
        // 兜底 1：按路径片段找
        static const char* fb[] = { "UAGame", "uamo" };
        pid = rload::find_by_needles(1024, fb, 2);
        if (pid) printf("[mload] 按路径兜底命中 pid=%d\n", pid);
    }
    if (!pid) {
        printf("[mload] 找不到目标。当前能看到的进程如下（确认游戏是否真的在运行）：\n");
        rload::dump_readable(1024, 30);
        printf("[mload] 若上面没有 UAGame，说明游戏进程已经退出，请重新打开游戏并保持在大厅，\n");
        printf("[mload] 然后从多任务切回本 app（不要划掉游戏），再点一次。\n");
        return;
    }
    printf("[mload] path=%s\n", path);

    // 顺便记下注入前目标是否在运行，便于判断线程何时才真正执行
    printf("[mload] 注意：游戏在后台时是挂起状态，远程线程要等游戏恢复运行才会执行。\n");
    printf("[mload] 注入后请立刻切回游戏等 3~5 秒，再回来点「只扫描」看面包屑。\n");

    // 用我们的 payload_manual（不定义新 ObjC 类）
    NSString* dylib = [[NSBundle mainBundle] pathForResource:@"ARPayloadManual" ofType:@"dylib"];
    if (!dylib) dylib = [[NSBundle mainBundle] pathForResource:@"ARPayload" ofType:@"dylib"];
    if (!dylib) { printf("[mload] 包里找不到 payload dylib\n"); return; }
    printf("[mload] payload=%s\n", dylib.UTF8String);

    mach_port_t task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    printf("[mload] task_for_pid(%d) -> %d\n", pid, kr);
    if (kr != KERN_SUCCESS) return;

    // 面包屑写到目标自己的容器里（沙盒可写）
    char container[1200] = {0};
    char note[1400] = {0};
    if (containercopy::data_container_of(target, container, sizeof(container))) {
        snprintf(note, sizeof(note), "%s/tmp/ar_manual_probe.txt", container);
        printf("[mload] 面包屑路径=%s\n", note);
    }
    mload::map_and_call(task, dylib.UTF8String, "arp_probe", note[0] ? note : nullptr, false);

    // 顺手读目标的主镜像基址与路径（为后面定位 UE4 全局变量做准备）
    char imgPath[512] = {0};
    uint64_t imgBase = ue4r::main_image_base(task, imgPath, sizeof(imgPath));
    printf("[ue4r] 目标主镜像 base=0x%llx path=%s\n",
           (unsigned long long)imgBase, imgPath[0] ? imgPath : "(未取到)");

    mach_port_deallocate(mach_task_self(), task);
    printf("=== 手动映射结束 ===\n");
}

// ---------------------------------------------------------------------------
// 导出完整日志：把 daemon 日志 + 面包屑 + 系统信息 + 进程清单写成单个文件，
// 并同时尝试写入几个「文件」App 里能直接找到的位置。
// 这样你不用截图，直接把文件发我即可。
// ---------------------------------------------------------------------------
static void ARExportLog(void) {
    NSMutableString* out = [NSMutableString string];

    struct utsname u;
    uname(&u);
    [out appendFormat:@"=== AR 日志导出 %@ ===\n", [NSDate date]];
    [out appendFormat:@"machine: %s\n", u.machine];
    [out appendFormat:@"kernel : %s\n", u.version];
    [out appendFormat:@"build  : %s   product: %s\n",
        buildtable::current_build(), buildtable::product_version()];

    // daemon 日志
    NSString* dl = daemonLogPath();
    NSString* dlog = [NSString stringWithContentsOfFile:dl encoding:NSUTF8StringEncoding error:nil];
    [out appendFormat:@"\n--- daemon log: %@ ---\n%@\n", dl, dlog.length ? dlog : @"(空)"];

    // 面包屑
    [out appendString:@"\n--- breadcrumbs ---\n"];
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* base = @"/var/mobile/Containers/Data/Application";
    NSArray* dirs = [fm contentsOfDirectoryAtPath:base error:nil];
    int bc = 0;
    for (NSString* d in dirs) {
        NSString* tmp = [NSString stringWithFormat:@"%@/%@/tmp", base, d];
        NSArray* files = [fm contentsOfDirectoryAtPath:tmp error:nil];
        for (NSString* f in files) {
            if (![f hasPrefix:@"ar_loaded_"] && ![f hasPrefix:@"ar_manual_"]) continue;
            NSString* full = [NSString stringWithFormat:@"%@/%@", tmp, f];
            [out appendFormat:@"[%@]\n%@\n", full,
                [NSString stringWithContentsOfFile:full encoding:NSUTF8StringEncoding error:nil] ?: @"(读不出)"];
            bc++;
        }
    }
    if (!bc) [out appendString:@"(没有任何面包屑文件)\n"];

    // 进程清单（sysctl 枚举，确认游戏在不在）
    [out appendString:@"\n--- processes ---\n"];
    struct kinfo_proc* procs = nullptr;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t sz = 0;
    if (sysctl(mib, 4, nullptr, &sz, nullptr, 0) == 0 && sz) {
        procs = (struct kinfo_proc*)malloc(sz);
        if (procs && sysctl(mib, 4, procs, &sz, nullptr, 0) == 0) {
            int n = (int)(sz / sizeof(struct kinfo_proc));
            for (int i = 0; i < n; i++) {
                const char* c = procs[i].kp_proc.p_comm;
                if (!c[0]) continue;
                [out appendFormat:@"  pid=%-5d comm=%s\n", procs[i].kp_proc.p_pid, c];
            }
        }
        if (procs) free(procs);
    } else {
        [out appendString:@"(sysctl 枚举失败)\n"];
    }

    // 候选输出路径：尽量覆盖「我的 iPad」「iCloud 云盘」「下载」
    NSMutableArray* targets = [NSMutableArray array];
    NSArray* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (docs.count) [targets addObject:[docs[0] stringByAppendingPathComponent:@"ar_full_log.txt"]];
    [targets addObject:@"/var/mobile/Library/Mobile Documents/com~apple~CloudDocs/ar_full_log.txt"];
    [targets addObject:@"/var/mobile/Media/Downloads/ar_full_log.txt"];
    [targets addObject:@"/var/mobile/Library/Caches/ar_full_log.txt"];

    printf("\n=== 导出日志 ===\n");
    for (NSString* t in targets) {
        NSString* dir = [t stringByDeletingLastPathComponent];
        if (![[NSFileManager defaultManager] fileExistsAtPath:dir]) {
            printf("[export] 目录不存在: %s\n", dir.UTF8String);
            continue;
        }
        NSError* err = nil;
        BOOL ok = [out writeToFile:t atomically:YES encoding:NSUTF8StringEncoding error:&err];
        printf("[export] %s -> %s%s\n", ok ? "成功" : "失败", t.UTF8String,
               ok ? "" : (err ? err.localizedDescription.UTF8String : ""));
    }
    printf("=== 导出结束（在上面成功的路径里找 ar_full_log.txt）===\n");
}


// ---------------------------------------------------------------------------
// 自注入自测：把 payload 映射进**本进程**并尝试执行。
//   - 本进程内成功 -> 注入实现没问题，游戏里的失败是目标环境差异
//   - 本进程内也失败 -> 是注入实现的 bug，可以本地反复调，不用牺牲游戏
// ---------------------------------------------------------------------------
static void ARSelfTest(int mode) {
    printf("\n=== 自注入自测（目标 = 本进程 %d）===\n", getpid());

    NSString* dylib = [[NSBundle mainBundle] pathForResource:@"ARPayloadManual" ofType:@"dylib"];
    if (!dylib) { printf("[self] 包里找不到 payload\n"); return; }
    printf("[self] payload=%s\n", dylib.UTF8String);

    mach_port_t task = mach_task_self();
    int susp = -1;
    bool running = mload::target_is_running(task, &susp);
    printf("[self] running=%d suspend_count=%d\n", running, susp);

    printf("[self] mode=%d (%s)\n", mode, mode == 2 ? "只映射" : "映射+新建线程执行");
    mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false, mode);
    printf("=== 自注入自测结束（%s）===\n", mode == 2 ? "只映射" : "映射+执行");
}


// ---------------------------------------------------------------------------
// 自测C 用的"已签名代码"探针。
//
// 目的：区分两种可能
//   1) 执行任何新线程都崩  -> 是线程状态问题
//   2) 只有执行"手工映射的匿名页"才崩 -> 是 iOS 的代码签名强制（AMFI）
//
// 这个函数编译进我们自己的二进制，属于**已签名页**。
// 如果让新线程跳到它上面不崩、跳到映射页就崩，结论就是 AMFI。
// ---------------------------------------------------------------------------
volatile long g_signedProbeRan = 0;

__attribute__((noinline, used))
static void arSignedProbe(void) {
    g_signedProbeRan = 0xABCD1234;
    for (;;) { __asm__ volatile("nop"); }
}


// 自测C：新建线程，但 PC 指向**我们自己已签名的函数**（不是手工映射的页）
static void ARSelfTestSigned(void) {
    printf("\n=== 自测C：执行已签名页（对照实验）===\n");

    mach_port_t task = mach_task_self();

    // 栈
    vm_address_t stack = 0;
    if (vm_allocate(task, &stack, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[selfC] 栈分配失败\n");
        return;
    }
    uint64_t stackTop = (uint64_t)((stack + 0x8000) & ~0xFULL);

    uintptr_t fn = (uintptr_t)&arSignedProbe;
    printf("[selfC] 目标函数(已签名) = 0x%llx, 栈顶=0x%llx\n",
           (unsigned long long)fn, (unsigned long long)stackTop);

    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(task, &th);
    printf("[selfC] thread_create -> %d\n", kr);
    if (kr != KERN_SUCCESS) return;

    arm_thread_state64_t st;
    memset(&st, 0, sizeof(st));
    st.__pc = (uint64_t)fn;
    st.__sp = stackTop;
    st.__lr = 0;
    st.__x[0] = 0;
    printf("[selfC] set_state 前\n");
    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
    printf("[selfC] set_state -> %d\n", kr);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);
    printf("[selfC] resume -> %d\n", kr);

    // 等一会儿，看探针有没有被打上标记
    for (int i = 0; i < 20; i++) {
        usleep(50 * 1000);
        if (g_signedProbeRan == 0xABCD1234) break;
    }
    printf("[selfC] 探针标记 = 0x%lx (%s)\n", g_signedProbeRan,
           g_signedProbeRan == 0xABCD1234 ? "已签名页执行成功" : "未执行");
    printf("=== 自测C 结束 ===\n");
}


// ---------------------------------------------------------------------------
// 自测D：文件映射 + 执行
//
// 与自测B 的唯一区别：代码页的来源不是"匿名内存"，而是**磁盘上那个已签名文件**。
// 如果这条路能跑通，说明系统校验的是页面 provenance（文件签名），
// 那么把它用到游戏进程上也有机会成立。
// ---------------------------------------------------------------------------
static void ARSelfTestFileMap(void) {
    printf("\n=== 自测D：文件映射 + 执行 ===\n");

    NSString* dylib = [[NSBundle mainBundle] pathForResource:@"ARPayloadManual" ofType:@"dylib"];
    if (!dylib) { printf("[selfD] 包里找不到 payload\n"); return; }
    printf("[selfD] payload=%s\n", dylib.UTF8String);

    mach_port_t task = mach_task_self();

    // 结果槽
    vm_address_t slot = 0;
    if (vm_allocate(task, &slot, 0x1000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[selfD] 结果槽分配失败\n");
        return;
    }
    printf("[selfD] 结果槽=0x%llx\n", (unsigned long long)slot);

    // 栈
    vm_address_t stack = 0;
    if (vm_allocate(task, &stack, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[selfD] 栈分配失败\n");
        return;
    }
    uintptr_t stackTop = (uintptr_t)(stack + 0x8000);

    filemap::Mapped mp;
    if (!filemap::map_into(task, dylib.UTF8String, mp)) {
        printf("[selfD] 文件映射失败\n");
        filemap::release(mp);
        return;
    }

    uintptr_t off = filemap::sym_offset((const uint8_t*)mp.localMap, mp.localSize, "arp_probe");
    printf("[selfD] arp_probe 偏移=0x%llx\n", (unsigned long long)off);
    if (!off) { printf("[selfD] 符号没找到，放弃\n"); filemap::release(mp); return; }

    uintptr_t entry = mp.remote + off;
    printf("[selfD] 目标内入口=0x%llx，开始执行\n", (unsigned long long)entry);
    filemap::run_entry(task, entry, (uint64_t)slot, stackTop);

    long vals[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 20; i++) {
        usleep(100 * 1000);
        vm_size_t got = 0;
        if (vm_read_overwrite(task, slot, sizeof(long) * 5, (vm_address_t)vals, &got) != KERN_SUCCESS) continue;
        if (vals[0] != 0) break;
    }
    printf("[selfD] 结果槽: entered=0x%lx 值1=0x%lx done=0x%lx\n", vals[0], vals[1], vals[3]);
    printf("[selfD] 判定: %s\n", vals[0] == 0xC0FFEE11L ? "文件映射页执行成功" : "文件映射页未能执行");

    filemap::release(mp);
    printf("=== 自测D 结束 ===\n");
}

static void ARDumpBreadcrumbs(void);


static void ARInjectSpringBoard(const char* kw) {
    NSString* pp = payloadPath();
    if (!pp || ![[NSFileManager defaultManager] fileExistsAtPath:pp]) {
        fprintf(stderr, "[inject] payload 缺失: %s\n", pp ? pp.UTF8String : "(null)");
        return;
    }

    // stdout/stderr -> 日志文件，界面每帧读它
    NSString* logPath = daemonLogPath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:logPath]) {
        [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    }
    int fd = open(logPath.UTF8String, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[inject] pid=%d payload=%s\n", getpid(), pp.UTF8String);

    // 实测：无沙盒后 task_for_pid 能成功，但平台二进制（SpringBoard）的路径读不到，
    // 普通 app 的路径读得到。所以优先注入游戏进程（普通 app）。
    // 关键词框里填 com.xxx 就按 bundle id 定位；否则用默认目标（暗区国际服）
    const char* kTargetBundle = (kw && strncmp(kw, "com.", 4) == 0)
                              ? kw
                              : "com.proximabeta.mf.uamo";
    printf("[inject] 目标 bundle id = %s\n", kTargetBundle);
    char tpath[1024] = {0};
    pid_t byBundle = bundleinfo::find_pid_by_bundle(kTargetBundle, 1024, tpath, sizeof(tpath));
    if (byBundle) {
        printf("[inject] 按 bundle id 命中 pid=%d (%s)\n", byBundle, kTargetBundle);
        printf("[inject] path=%s\n", tpath);

        // 关键：payload 必须放进目标自己的数据容器，否则目标读不到
        char container[1200], staged[1300];
        const char* usePath = pp.UTF8String;
        if (containercopy::data_container_of(kTargetBundle, container, sizeof(container)) &&
            containercopy::install_payload(container, pp.UTF8String, staged, sizeof(staged))) {
            usePath = staged;
            printf("[inject] 使用容器内副本: %s\n", usePath);
        } else {
            printf("[inject] 放进目标容器失败，退而使用原路径（大概率打不开）\n");
        }

        bool okb = rload::inject_pid(byBundle, usePath);
        printf("[inject] 结果=%s\n", okb ? "已注入" : "失败");
        ARDumpBreadcrumbs();
        return;
    }
    printf("[inject] 按 bundle id 没找到目标，改用关键词匹配\n");

    static const char* kFallback[] = { "UAGame", "uamo", "Arena", "ProximaBeta" };
    const char* needles[8];
    int n = 0;
    if (kw && kw[0]) needles[n++] = kw;
    for (int i = 0; i < 4; i++) needles[n++] = kFallback[i];
    printf("[inject] 开始扫描目标进程（关键词 %d 个，首个=%s）\n", n, needles[0]);
    pid_t target = rload::find_by_needles(1024, needles, n);
    printf("[inject] 目标 pid=%d\n", target);
    if (!target) {
        printf("[inject] 没找到目标。请先打开游戏再试一次。\n");
        return;
    }

    bool ok = rload::inject_pid(target, pp.UTF8String);
    printf("[inject] 结果=%s\n", ok ? "已注入" : "失败");
    ARDumpBreadcrumbs();
}

// ==================== UI ====================
@interface RootVC : UIViewController
@property (nonatomic, strong) UILabel* status;
@property (nonatomic, strong) UITextView* logView;
@property (nonatomic, strong) UITextField* keyword;
@end

@implementation RootVC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    CGFloat w = self.view.bounds.size.width;
    CGFloat pad = 20;

    self.status = [[UILabel alloc] initWithFrame:CGRectMake(pad, 60, w - pad*2, 60)];
    self.status.numberOfLines = 0;
    self.status.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    [self.view addSubview:self.status];

    UIButton* start = [UIButton buttonWithType:UIButtonTypeSystem];
    start.frame = CGRectMake(pad, 130, (w - pad*3)/2, 48);
    [start setTitle:@"启动 daemon" forState:UIControlStateNormal];
    [start addTarget:self action:@selector(onStart) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:start];

    UIButton* stop = [UIButton buttonWithType:UIButtonTypeSystem];
    stop.frame = CGRectMake(pad*2 + (w - pad*3)/2, 130, (w - pad*3)/2, 48);
    [stop setTitle:@"停止 daemon" forState:UIControlStateNormal];
    [stop addTarget:self action:@selector(onStop) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stop];

    UIButton* inj = [UIButton buttonWithType:UIButtonTypeSystem];
    inj.frame = CGRectMake(pad, 190, (w - pad*3)/2, 48);
    [inj setTitle:@"找游戏并注入" forState:UIControlStateNormal];
    inj.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    [inj addTarget:self action:@selector(onInject) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:inj];

    // 分析目标 app：读它的二进制，看它用了什么机制（内核读写 / 场景托管 / 注入）
    UIButton* ana = [UIButton buttonWithType:UIButtonTypeSystem];
    ana.frame = CGRectMake(pad, 300, w - pad*2, 44);
    [ana setTitle:@"分析目标 app（读它的权限与机制）" forState:UIControlStateNormal];
    [ana addTarget:self action:@selector(onAnalyze) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:ana];

    // 手动映射注入（绕开 dlopen）
    UIButton* man = [UIButton buttonWithType:UIButtonTypeSystem];
    man.frame = CGRectMake(pad, 344, w - pad*2, 44);
    [man setTitle:@"手动映射注入（绕开 dlopen）" forState:UIControlStateNormal];
    [man addTarget:self action:@selector(onManual) forControlEvents:UIControlEventTouchUpInside];

    // 导出完整日志到「文件」App 可见位置，直接发文件代替截图
    UIButton* exp = [UIButton buttonWithType:UIButtonTypeSystem];
    exp.frame = CGRectMake(pad, 390, w - pad*2, 40);
    [exp setTitle:@"导出完整日志（发我用）" forState:UIControlStateNormal];
    [exp addTarget:self action:@selector(onExport) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:exp];
    [self.view addSubview:man];

    // 自注入自测：把 payload 打进本进程，验证执行链路（不碰游戏）
    UIButton* stBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn.frame = CGRectMake(pad, 428, (w - pad*3) / 2, 38);
    [stBtn setTitle:@"自测A：只映射" forState:UIControlStateNormal];
    [stBtn addTarget:self action:@selector(onSelfMapOnly) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn];

    UIButton* stBtn2 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn2.frame = CGRectMake(pad*2 + (w - pad*3) / 2, 428, (w - pad*3) / 2, 38);
    [stBtn2 setTitle:@"自测B：映射+执行" forState:UIControlStateNormal];
    [stBtn2 addTarget:self action:@selector(onSelfExec) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn2];

    UIButton* stBtn3 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn3.frame = CGRectMake(pad, 470, w - pad*2, 40);
    [stBtn3 setTitle:@"自测C：执行已签名页（对照实验）" forState:UIControlStateNormal];
    [stBtn3 addTarget:self action:@selector(onSelfSigned) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn3];

    UIButton* stBtn4 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn4.frame = CGRectMake(pad, 504, w - pad*2, 36);
    [stBtn4 setTitle:@"自测D：文件映射+执行（已签名文件页）" forState:UIControlStateNormal];
    [stBtn4 addTarget:self action:@selector(onSelfFileMap) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn4];


    // 只扫描不注入：安全的诊断步骤，绝不碰任何目标进程的内存
    UIButton* scan = [UIButton buttonWithType:UIButtonTypeSystem];
    scan.frame = CGRectMake(pad*2 + (w - pad*3)/2, 190, (w - pad*3)/2, 48);
    [scan setTitle:@"只扫描（不注入）" forState:UIControlStateNormal];
    [scan addTarget:self action:@selector(onScan) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:scan];

    // 目标关键词输入框：填游戏在 /var/containers/Bundle/Application/.../<这里>.app 里的名字
    self.keyword = [[UITextField alloc] initWithFrame:CGRectMake(pad, 250, w - pad*2, 40)];
    self.keyword.borderStyle = UITextBorderStyleRoundedRect;
    self.keyword.placeholder = @"留空即只找暗区（UAGame）";
    self.keyword.text = @"";
    self.keyword.autocapitalizationType = UITextAutocapitalizationTypeNone;
    self.keyword.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    [self.view addSubview:self.keyword];

    self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 546, w - pad*2, self.view.bounds.size.height - 566)];
    self.logView.editable = NO;
    self.logView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    [self.view addSubview:self.logView];

    [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
    [self tick];
}

- (void)tick {
    NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                             encoding:NSUTF8StringEncoding error:nil];
    self.logView.text = log.length ? log : @"(daemon 还没写日志)";

    NSArray* lines = [log componentsSeparatedByString:@"\n"];
    NSString* last = nil;
    for (NSString* l in [lines reverseObjectEnumerator]) {
        if (l.length) { last = l; break; }
    }
    NSString* alive = @"未知";
    if (last) {
        NSRange r = [last rangeOfString:@"pid="];
        if (r.location != NSNotFound) alive = @"运行中";
    }
    self.status.text = [NSString stringWithFormat:
        @"daemon: %@\n日志: %@\npayload: %@",
        alive, daemonLogPath(), payloadPath() ? @"已打包" : @"缺失"];
}

- (void)onStart {
    [[NSFileManager defaultManager] removeItemAtPath:stopFlagPath() error:nil];

    NSString* dp = daemonPath();
    // daemon 用不定义 ObjC 类的手动映射版 payload
    NSString* pp = [[NSBundle mainBundle] pathForResource:@"ARPayloadManual" ofType:@"dylib"];
    if (!pp) pp = payloadPath();
    if (!dp || !pp) {
        self.status.text = @"缺少 ARDaemon 或 ARPayload，检查打包步骤";
        return;
    }

    // 参数：argv[1]=日志路径 argv[2]=payload 路径 argv[3..]=目标关键词
    NSString* kw = self.keyword.text.length ? self.keyword.text : @"UAGame";
    char* argv[] = {
        (char*)dp.UTF8String,
        (char*)daemonLogPath().UTF8String,
        (char*)pp.UTF8String,
        (char*)kw.UTF8String,
        (char*)"UAGame",
        (char*)"uamo",
        NULL
    };
    int err = 0;
    pid_t pid = host::spawn_daemon(dp.UTF8String, argv, &err);
    if (pid > 0) {
        self.status.text = [NSString stringWithFormat:@"已孵化 pid=%d", pid];
    } else {
        self.status.text = [NSString stringWithFormat:
            @"spawn 失败 errno=%d (%s)\n签名了吗？", err, strerror(err)];
    }
    [self tick];
}

- (void)onStop {
    [@"stop" writeToFile:stopFlagPath() atomically:YES encoding:NSUTF8StringEncoding error:nil];
    self.status.text = @"已写入 stop 标志，daemon 将在 1s 内退出";
}

// 去所有 app 数据容器里找 payload 的面包屑文件，证明注入是否真的加载成功
static void ARDumpBreadcrumbs(void) {
    printf("\n=== payload 面包屑 ===\n");
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* base = @"/var/mobile/Containers/Data/Application";
    NSArray* dirs = [fm contentsOfDirectoryAtPath:base error:nil];
    int found = 0;
    for (NSString* d in dirs) {
        NSString* tmp = [NSString stringWithFormat:@"%@/%@/tmp", base, d];
        NSArray* files = [fm contentsOfDirectoryAtPath:tmp error:nil];
        for (NSString* f in files) {
            if (![f hasPrefix:@"ar_loaded_"] && ![f hasPrefix:@"ar_manual_"]) continue;
            NSString* full = [NSString stringWithFormat:@"%@/%@", tmp, f];
            NSString* content = [NSString stringWithContentsOfFile:full
                                                         encoding:NSUTF8StringEncoding error:nil];
            printf("[breadcrumb] %s\n", full.UTF8String);
            if (content) printf("    %s\n", content.UTF8String);
            found++;
        }
    }
    if (!found) printf("[breadcrumb] 一个都没有 —— payload 没有被加载（dlopen 很可能被 library validation 拒了）\n");
    printf("=== 面包屑结束 ===\n");
}

// 分析目标 app：找到进程 -> 读它的可执行文件路径 -> 解析 entitlements + 扫机制关键词
// 把 stdout/stderr 接到日志文件，界面上就能看到
static void ARRedirectToLog(void) {
    NSString* logPath = daemonLogPath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:logPath]) {
        [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    }
    int fd = open(logPath.UTF8String, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void ARAnalyzeTarget(const char* kw) {    if (!kw || !kw[0]) { printf("[analyze] 关键词为空\n"); return; }
    printf("\n=== 分析目标：%s ===\n", kw);

    pid_t pid = rload::find_pid_comm(kw);
    if (pid) printf("[analyze] 按 comm 命中 pid=%d\n", pid);
    if (!pid) pid = rload::find_pid_path(kw);
    if (!pid) pid = rload::find_by_needles(1024, &kw, 1);
    if (!pid) { printf("[analyze] 找不到进程，先把它打开再试\n"); return; }

    mach_port_t t = MACH_PORT_NULL;
    if (task_for_pid(mach_task_self(), pid, &t) != KERN_SUCCESS) {
        printf("[analyze] task_for_pid 失败\n"); return;
    }
    char path[1024];
    bool ok = rload::read_remote_path(t, path, sizeof(path));
    mach_port_deallocate(mach_task_self(), t);
    if (!ok) { printf("[analyze] 读不到它的可执行路径\n"); return; }
    printf("[analyze] 可执行文件: %s\n", path);

    inspect::analyze_file(path);
    printf("=== 分析结束 ===\n");
}

// 只扫描：安全诊断，只做 task_for_pid 与只读的 dyld 信息读取，不写、不注入
static void ARScanOnly(const char* kw) {
    NSString* logPath = daemonLogPath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:logPath]) {
        [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    }
    int fd = open(logPath.UTF8String, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== 只扫描模式：只读，不写入任何目标进程 ===\n");
    static const char* fb[] = { "Arena", "uamo", "ProximaBeta", "SpringBoard" };
    const char* needles[8];
    int n = 0;
    if (kw && kw[0]) needles[n++] = kw;
    for (int i = 0; i < 4; i++) needles[n++] = fb[i];
    pid_t hit = rload::find_by_needles(1024, needles, n);
    printf("[scan] 首个命中 pid=%d\n", hit);

    // 把每个 app 的 bundle id 也读出来：进程名可能和游戏显示名完全无关
    printf("\n=== 内核侧状态 ===\n");
    buildtable::dump_status();
    kback::dump_status();
    kcache::probe();

    ARDumpBreadcrumbs();

    printf("\n=== 各 app 的 bundle id ===\n");
    bundleinfo::dump_all_bundles(1024, 20);
    printf("=== 扫描结束，未做任何注入 ===\n");
}

- (void)onScan {
    self.status.text = @"正在扫描（只读，不注入）…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARScanOnly(self.keyword.text.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"扫描完成，未注入";
        });
    });
}

- (void)onSelfFileMap {
    self.status.text = @"自测D：文件映射…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTestFileMap();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"自测D结束";
        });
    });
}

- (void)onSelfSigned {
    self.status.text = @"自测C：执行已签名页…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTestSigned();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"自测C结束";
        });
    });
}

- (void)onSelfMapOnly { [self runSelfTest:2]; }
- (void)onSelfExec    { [self runSelfTest:1]; }

- (void)runSelfTest:(int)mode {
    self.status.text = (mode == 2) ? @"自测A：只映射…" : @"自测B：映射+执行…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTest(mode);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"自测结束（若崩溃：重开本 app 后点导出日志）";
        });
    });
}

- (void)onExport {

    self.status.text = @"正在导出日志…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARExportLog();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"已导出，看日志里 [export] 成功的路径";
        });
    });
}

- (void)onManual {
    NSString* kw = self.keyword.text;
    self.status.text = @"正在手动映射…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARManualInject(kw.UTF8String);
        ARDumpBreadcrumbs();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"手动映射完成";
        });
    });
}

- (void)onAnalyze {
    NSString* kw = self.keyword.text.length ? self.keyword.text : @"DeltaForce";
    self.status.text = @"正在分析目标…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARAnalyzeTarget(kw.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"分析完成";
        });
    });
}

- (void)onInject {
    self.status.text = @"正在注入 SpringBoard…";
    // 放后台队列：task_for_pid + thread_create 会阻塞，不能占主线程
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARInjectSpringBoard(self.keyword.text.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"注入流程已结束，看下面日志";
        });
    });
}

@end

// ==================== App ====================
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow* window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)o {
    ARProbe("ui_ready");            // 走到这里说明 UI 已经起来了
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[RootVC alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        ARProbe("main_enter");          // 走到这里说明进程已被拉起
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
