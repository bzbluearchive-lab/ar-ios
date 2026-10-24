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
#include <fcntl.h>
#include <stdio.h>
#include "DaemonSpawn.h"
#include "../src/Inject/RemoteLoad.h"

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
static void ARInjectSpringBoard(void) {
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

    // 枚举接口在沙盒里被拒（实测 sysctl=-1 / proc_listallpids=0），
    // 所以直接走暴力定位：逐个 pid 试 task_for_pid，再读路径认身份
    printf("[inject] 开始暴力定位 SpringBoard\n");
    pid_t sb = rload::find_by_bruteforce("SpringBoard", 1024, true);
    printf("[inject] SpringBoard pid=%d\n", sb);
    if (!sb) { printf("[inject] 暴力定位失败，注入中止\n"); return; }

    bool ok = rload::inject_pid(sb, pp.UTF8String);
    printf("[inject] 结果=%s\n", ok ? "已注入" : "失败");
}

// ==================== UI ====================
@interface RootVC : UIViewController
@property (nonatomic, strong) UILabel* status;
@property (nonatomic, strong) UITextView* logView;
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
    [inj setTitle:@"注入 SpringBoard" forState:UIControlStateNormal];
    inj.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    [inj addTarget:self action:@selector(onInject) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:inj];

    // 只扫描不注入：安全的诊断步骤，绝不碰任何目标进程的内存
    UIButton* scan = [UIButton buttonWithType:UIButtonTypeSystem];
    scan.frame = CGRectMake(pad*2 + (w - pad*3)/2, 190, (w - pad*3)/2, 48);
    [scan setTitle:@"只扫描（不注入）" forState:UIControlStateNormal];
    [scan addTarget:self action:@selector(onScan) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:scan];

    self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 260, w - pad*2, self.view.bounds.size.height - 280)];
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
    NSString* pp = payloadPath();
    if (!dp || !pp) {
        self.status.text = @"缺少 ARDaemon 或 ARPayload，检查打包步骤";
        return;
    }

    // 参数：argv[1]=日志路径 argv[2]=payload 路径
    char* argv[] = {
        (char*)dp.UTF8String,
        (char*)daemonLogPath().UTF8String,
        (char*)pp.UTF8String,
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

// 只扫描：安全诊断，只做 task_for_pid 与只读的 dyld 信息读取，不写、不注入
static void ARScanOnly(void) {
    NSString* logPath = daemonLogPath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:logPath]) {
        [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    }
    int fd = open(logPath.UTF8String, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== 只扫描模式：只读，不写入任何目标进程 ===\n");
    pid_t sb = rload::find_by_bruteforce("SpringBoard", 1024, true);
    printf("[scan] SpringBoard=%d\n", sb);
    printf("=== 扫描结束，未做任何注入 ===\n");
}

- (void)onScan {
    self.status.text = @"正在扫描（只读，不注入）…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARScanOnly();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"扫描完成，未注入";
        });
    });
}

- (void)onInject {
    self.status.text = @"正在注入 SpringBoard…";
    // 放后台队列：task_for_pid + thread_create 会阻塞，不能占主线程
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARInjectSpringBoard();
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
