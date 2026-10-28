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
#include <errno.h>
#include <sys/mman.h>
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
#include "../src/Inject/TaskPorts.h"
#include "../src/Inject/CodeCave.h"
#include "../src/Kernel/KfdBackend.h"
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


// ---------------------------------------------------------------------------
// 自测E：JIT 能力探测（最短路径）
//
// 直接回答：本进程能否创建可执行内存并跳进去执行？
//   1. 分配一页 RW
//   2. 写入 8 字节 ARM64：movz w0,#0x1234 ; ret
//   3. mprotect 成 RX
//   4. 当函数指针调用
//
// 返回 0x1234 = JIT 可用；进程被杀 = 系统禁止（与自测B 同因）；
// mprotect 报错 = 内核直接拒绝。
// ---------------------------------------------------------------------------
static void ARSelfTestJIT(void) {
    printf("\n=== 自测E：JIT 能力探测 ===\n");

    size_t page = (size_t)getpagesize();
    void* mem = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        printf("[selfE] 分配失败: %s\n", strerror(errno));
        return;
    }
    printf("[selfE] 已分配 RW 页 @ %p\n", mem);

    uint32_t code[2] = { 0x52824680u, 0xd65f03c0u };   // movz w0,#0x1234 ; ret
    memcpy(mem, code, sizeof(code));
    printf("[selfE] 已写入 8 字节 ARM64 代码\n");

    int rc = mprotect(mem, page, PROT_READ | PROT_EXEC);
    printf("[selfE] mprotect(RX) -> %d %s\n", rc, rc != 0 ? strerror(errno) : "");
    if (rc != 0) {
        munmap(mem, page);
        printf("[selfE] 结论: 内核拒绝创建可执行内存\n");
        printf("=== 自测E 结束 ===\n");
        return;
    }

    int (*fn)(void) = (int (*)(void))mem;
    printf("[selfE] 即将跳入该页执行...\n");
    fflush(stdout);
    int r = fn();
    printf("[selfE] 返回 0x%x (%s)\n", r, (r == 0x1234) ? "JIT 可用" : "返回值异常");
    munmap(mem, page);
    printf("=== 自测E 结束 ===\n");
}


// ---------------------------------------------------------------------------
// 取任务端口测试：用 processor_set_tasks 而非 task_for_pid
//   商用产品（播客.RS.R）的导入表里就有 processor_set_tasks，
//   这条路绕开 task_for_pid 的平台二进制限制。
// ---------------------------------------------------------------------------
static void ARTestTaskPorts(void) {
    printf("\n=== processor_set_tasks 取任务端口 ===\n");

    // 先枚举一下当前所有能拿到的 task，确认这条路本身通不通
    int total = 0;
    taskports::Found probe = taskports::find_by_pid(getpid(), &total);
    printf("[tports] 本进程 pid=%d -> port=%u（枚举到 %d 个 task）\n",
           getpid(), probe.port, total);

    if (!probe.port) {
        printf("[tports] 连自己的 task 都拿不到 -> 这条路权限不足\n");
        return;
    }
    taskports::verify(probe.port, getpid());

    // 再找目标游戏
    pid_t game = rload::find_pid_comm("UAGame");
    printf("[tports] 目标游戏 pid=%d\n", game);
    if (!game) {
        printf("[tports] 游戏没在运行，先打开游戏\n");
        return;
    }

    taskports::Found g = taskports::find_by_pid(game);
    if (!g.port) {
        printf("[tports] 没拿到游戏 task port\n");
        return;
    }
    bool ok = taskports::verify(g.port, g.pid);
    printf("[tports] 结论: %s\n", ok ? "拿到游戏 task port 且可读内存" : "拿到端口但读不到内存");
    printf("=== 测试结束 ===\n");
}


// ---------------------------------------------------------------------------
// code cave 探测（无害验证）
//
// 目的：在**不执行任何东西**的前提下量出三件事
//   1) 能不能用 processor_set_tasks 拿到游戏 task port
//   2) 能不能在游戏 __TEXT 里找到可用空洞（code cave）
//   3) 能不能对 App Store 签名的 __TEXT 页做 vm_protect(R|W|COPY) + vm_write
//      —— 做法是把洞里"原本就有的那个字节"原样写回去，
//         语义零改变，但权限链路完整走了一遍。
//
// 第 3 点是巨魔方案的命门：能写才有后面的 hook；不能写（返回 4 = 权限不足）
// 就直接说明这条路要放弃，转 kfd。
// ---------------------------------------------------------------------------
static void ARProbeCodeCave(void) {
    printf("\n=== code cave 探测 ===\n");

    pid_t game = rload::find_pid_comm("UAGame");
    printf("[cave-test] 游戏 pid=%d\n", game);
    if (!game) { printf("[cave-test] 游戏没在运行\n"); return; }

    taskports::Found tp = taskports::find_by_pid(game);
    if (!tp.port) { printf("[cave-test] 拿不到 task port，后面无从谈起\n"); return; }

    // 目标主镜像基址（TASK_DYLD_INFO）
    task_dyld_info_data_t di;
    mach_msg_type_number_t dc = TASK_DYLD_INFO_COUNT;
    if (task_info(tp.port, TASK_DYLD_INFO, (task_info_t)&di, &dc) != KERN_SUCCESS) {
        printf("[cave-test] TASK_DYLD_INFO 失败\n");
        return;
    }
    uint64_t arr = 0, imageBase = 0;
    vm_size_t got = 0;
    vm_read_overwrite(tp.port, (vm_address_t)(di.all_image_info_addr + 8), 8, (vm_address_t)&arr, &got);
    vm_read_overwrite(tp.port, (vm_address_t)arr, 8, (vm_address_t)&imageBase, &got);
    printf("[cave-test] 游戏主镜像基址=0x%llx\n", (unsigned long long)imageBase);
    if (!imageBase) return;

    // 段表：从本机可读的二进制文件解析
    char binPath[1024] = {0};
    if (!rload::read_remote_path(tp.port, binPath, sizeof(binPath)) || !binPath[0]) {
        printf("[cave-test] 读不到主二进制路径\n");
        return;
    }
    printf("[cave-test] 二进制=%s\n", binPath);

    cave::Seg segs[32];
    int nseg = 0;
    if (!cave::list_segments(tp.port, binPath, segs, 32, &nseg)) {
        printf("[cave-test] 段表解析失败\n");
        return;
    }

    // 找 __TEXT 范围
    uint64_t textVa = 0, textSize = 0, textVmaddr = 0, textVmsize = 0;
    for (int i = 0; i < nseg; i++) {
        if (strcmp(segs[i].name, "__TEXT") == 0) {
            textVmaddr = segs[i].vmaddr;
            textVmsize = segs[i].vmsize;
        }
    }
    if (!textVmsize) { printf("[cave-test] 没找到 __TEXT\n"); return; }
    textVa = imageBase + (textVmaddr - textVmaddr);   // __TEXT 本身就是基准
    textSize = textVmsize;

    // 在 __TEXT 后半段找洞（前半段是代码，尾部通常是 padding）
    uint64_t scanFrom = textVa + textSize / 2;
    uint64_t scanTo = textVa + textSize;
    uint64_t c = cave::find_cave(tp.port, imageBase, scanFrom, 16, scanTo);
    printf("[cave-test] 扫描区间 [0x%llx, 0x%llx) 空洞=0x%llx\n",
           (unsigned long long)scanFrom, (unsigned long long)scanTo, (unsigned long long)c);
    if (!c) { printf("[cave-test] 没找到空洞（可能全被代码占满）\n"); return; }

    // 读洞里原本的 4 字节
    uint32_t orig = 0;
    if (!cave::read_mem(tp.port, c, &orig, 4)) {
        printf("[cave-test] 读洞里数据失败\n");
        return;
    }
    printf("[cave-test] 洞里原值 = 0x%08x，将原样写回（语义零改变，只测权限）\n", orig);

    bool w = cave::write_bytes(tp.port, c, &orig, 4);
    printf("[cave-test] 写入结果 = %d\n", w);
    if (w) {
        uint32_t back = 0;
        cave::read_mem(tp.port, c, &back, 4);
        printf("[cave-test] 回读 = 0x%08x  %s\n", back, back == orig ? "一致" : "不一致!");
        printf("[cave-test] 结论: **可以写游戏 __TEXT** -> code cave hook 路线可行\n");
    } else {
        printf("[cave-test] 结论: 写不进去（看上面 vm_protect 的返回码；4=权限不足）\n");
    }
    printf("=== 探测结束 ===\n");
}


// ---------------------------------------------------------------------------
// kfd 自检：验证内核读写是否真的拿到
//   method: 0=physpuppet 1=landa 2=smith
// 判定标准（全过才算成功）：命中版本条目 → kopen 成功 → 内核基址 magic 校验
//   → current_proc 的 p_pid == 本进程 → kernel_proc 的 p_pid == 0
// ---------------------------------------------------------------------------
static void ARKfdSelfTest(int method) {
    const char* name = (method == 0) ? "physpuppet" : (method == 1 ? "landa" : "smith");
    printf("\n=== kfd 自检 (%s) ===\n", name);
    printf("[kfd-test] 设备: arch=%s / iOS %s / build %s\n",
           buildtable::current_arch(), buildtable::product_version(), buildtable::current_build());
    fflush(stdout);

    bool ok = kfdb::selftest(method);
    printf("[kfd-test] 结论: %s\n", ok ? "内核读写可用" : "未拿到内核读写（看上面 [kfd] 日志的失败步）");
    printf("=== kfd 自检结束 ===\n");
    fflush(stdout);
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

    UIButton* stBtn5 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn5.frame = CGRectMake(pad, 540, w - pad*2, 36);
    [stBtn5 setTitle:@"自测E：JIT 能力探测（可否执行新代码）" forState:UIControlStateNormal];
    [stBtn5 addTarget:self action:@selector(onSelfJIT) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn5];

    UIButton* tpBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    tpBtn.frame = CGRectMake(pad, 570, w - pad*2, 34);
    [tpBtn setTitle:@"取任务端口（processor_set_tasks）" forState:UIControlStateNormal];
    [tpBtn addTarget:self action:@selector(onTaskPorts) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:tpBtn];

    UIButton* cvBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    cvBtn.frame = CGRectMake(pad, 604, w - pad*2, 34);
    [cvBtn setTitle:@"code cave 探测（试写同值）" forState:UIControlStateNormal];
    [cvBtn addTarget:self action:@selector(onCaveProbe) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:cvBtn];

    UIButton* kfd1 = [UIButton buttonWithType:UIButtonTypeSystem];
    kfd1.frame = CGRectMake(pad, 646, (w - pad*3) / 2, 38);
    [kfd1 setTitle:@"kfd 自检：physpuppet" forState:UIControlStateNormal];
    [kfd1 addTarget:self action:@selector(onKfdPhys) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:kfd1];

    UIButton* kfd2 = [UIButton buttonWithType:UIButtonTypeSystem];
    kfd2.frame = CGRectMake(pad*2 + (w - pad*3) / 2, 646, (w - pad*3) / 2, 38);
    [kfd2 setTitle:@"kfd 自检：landa" forState:UIControlStateNormal];
    [kfd2 addTarget:self action:@selector(onKfdLanda) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:kfd2];


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

    self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 690, w - pad*2, self.view.bounds.size.height - 710)];
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

- (void)onKfdPhys {
    self.status.text = @"kfd 自检 physpuppet…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARKfdSelfTest(0);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"kfd 自检结束";
        });
    });
}

- (void)onKfdLanda {
    self.status.text = @"kfd 自检 landa…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARKfdSelfTest(1);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"kfd 自检结束";
        });
    });
}

- (void)onCaveProbe {
    self.status.text = @"探测 code cave…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARProbeCodeCave();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"探测结束";
        });
    });
}

- (void)onTaskPorts {
    self.status.text = @"取任务端口中…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARTestTaskPorts();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"端口测试结束";
        });
    });
}

- (void)onSelfJIT {
    self.status.text = @"自测E：JIT 探测…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTestJIT();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"自测E结束";
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
