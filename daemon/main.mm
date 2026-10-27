// daemon/main.mm —— 常驻扫描器：等目标出现就注入
//
// 为什么需要它：
//   注入必须在目标进程存活时进行，而我们的 app 一进后台就挂起。
//   这个 daemon 是无沙盒进程，可以一直跑，每 2 秒扫一次进程表，
//   目标（游戏）一出现就注入；同一 pid 只注入一次。
//
// 参数：
//   argv[1] 日志路径
//   argv[2] payload dylib 路径
//   argv[3..] 目标路径关键词（可多个）
//
// 退出：同目录出现 ar_daemon.stop 文件。

#import <Foundation/Foundation.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include "../src/Inject/RemoteLoad.h"
#include "../src/Inject/BundleInfo.h"
#include "../src/Inject/ManualLoad.h"
#include "../src/Inject/ContainerCopy.h"

static FILE* g_log = NULL;

static void L(const char* fmt, ...) {
    if (!g_log) return;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fprintf(g_log, "[%02d:%02d:%02d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------- 轻量进程枚举：sysctl（无沙盒进程可直接用） ----------------
// 相比对 1024 个 pid 逐个 task_for_pid，这个只读一次内核进程表，
// 对目标几乎无扰动，不会把游戏搞挂。
static int list_procs(struct kinfo_proc** out) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t sz = 0;
    if (sysctl(mib, 4, nullptr, &sz, nullptr, 0) != 0 || sz == 0) { *out = nullptr; return -1; }
    struct kinfo_proc* p = (struct kinfo_proc*)malloc(sz);
    if (!p) { *out = nullptr; return -1; }
    if (sysctl(mib, 4, p, &sz, nullptr, 0) != 0) { free(p); *out = nullptr; return -1; }
    *out = p;
    return (int)(sz / sizeof(struct kinfo_proc));
}

static pid_t find_pid_comm_fast(const char* needle) {
    struct kinfo_proc* p = nullptr;
    int n = list_procs(&p);
    if (n <= 0 || !p) return 0;
    pid_t found = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(p[i].kp_proc.p_comm, needle)) { found = p[i].kp_proc.p_pid; break; }
    }
    free(p);
    return found;
}

static void dump_comms(int maxPrint) {
    struct kinfo_proc* p = nullptr;
    int n = list_procs(&p);
    if (n <= 0 || !p) { L("sysctl 枚举失败"); return; }
    L("当前进程 %d 个：", n);
    int shown = 0;
    for (int i = 0; i < n && shown < maxPrint; i++) {
        const char* c = p[i].kp_proc.p_comm;
        if (!c[0]) continue;
        L("  pid=%-5d comm=%s", p[i].kp_proc.p_pid, c);
        shown++;
    }
    free(p);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        setsid();
        signal(SIGPIPE, SIG_IGN);

        const char* logPath = (argc > 1) ? argv[1] : "/var/mobile/Library/Caches/ar_daemon.log";
        const char* payload = (argc > 2) ? argv[2] : NULL;

        g_log = fopen(logPath, "a");
        if (!g_log) return 1;

        // stdout/stderr 指向同一日志，RemoteLoad 里的 printf 就不会丢
        int fd = open(logPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);

        L("=== daemon 启动 pid=%d ppid=%d ===", getpid(), getppid());
        L("payload=%s", payload ? payload : "(none)");

        // 关键词支持 "Arena Breakout" 这种带空格的写法：拆成多个独立片段
        static const char* needles[32];
        int nNeedles = 0;
        for (int i = 3; i < argc && nNeedles < 30; i++) {
            char* buf = strdup(argv[i]);
            for (char* t = strtok(buf, " ,/"); t && nNeedles < 30; t = strtok(NULL, " ,/")) {
                if (strlen(t) >= 2) needles[nNeedles++] = t;
            }
        }
        if (nNeedles == 0) {
            // 只认暗区（UAGame / mf.uamo），不要误打三角洲（DeltaForceClient）
            needles[nNeedles++] = "UAGame";
            needles[nNeedles++] = "uamo";
        }
        for (int i = 0; i < nNeedles; i++) L("关键词[%d]=%s", i, needles[i]);

        if (!payload) { L("缺 payload 路径，退出"); return 1; }

        char stopPath[1024];
        snprintf(stopPath, sizeof(stopPath), "%s", logPath);
        char* slash2 = strrchr(stopPath, '/');
        if (slash2) snprintf(slash2 + 1, sizeof(stopPath) - (size_t)(slash2 + 1 - stopPath), "ar_daemon.stop");

        pid_t lastInjected = -1;
        int tick = 0;
        for (;;) {
            tick++;

            // 轻量定位：先用 sysctl 按进程名找（几乎零扰动）
            pid_t target = 0;
            const char* how = "comm";
            for (int i = 0; i < nNeedles && !target; i++) {
                target = find_pid_comm_fast(needles[i]);
                if (target) L("sysctl 命中 %s -> pid=%d", needles[i], target);
            }

            // 再按 bundle id（需要读 Info.plist，稍重，但仅在必要时用）
            if (!target) {
                target = bundleinfo::find_pid_by_bundle("com.proximabeta.mf.uamo", 1024, nullptr, 0);
                how = "bundle";
            }

            // 都没命中：每 10 次列一次完整进程名表，用来确认游戏到底在不在
            if (!target && (tick % 10 == 1)) {
                dump_comms(40);
            }

            if (target && target != lastInjected) {
                L("目标 pid=%d（%s），开始手动映射注入", target, how);
                mach_port_t task = MACH_PORT_NULL;
                kern_return_t kr = task_for_pid(mach_task_self(), target, &task);
                L("task_for_pid(%d) -> %d", target, kr);
                if (kr == KERN_SUCCESS) {
                    char container[1200] = {0};
                    char note[1400] = {0};
                    if (containercopy::data_container_of("com.proximabeta.mf.uamo", container, sizeof(container)))
                        snprintf(note, sizeof(note), "%s/tmp/ar_manual_probe.txt", container);
                    L("面包屑路径=%s", note[0] ? note : "(none)");
                    uintptr_t rb = mload::map_and_call(task, payload, "arp_note", note[0] ? note : nullptr, true);
                    mach_port_deallocate(mach_task_self(), task);
                    L("手动映射完成 base=0x%llx", (unsigned long long)rb);
                } else {
                    L("拿不到 task port，跳过");
                }
                lastInjected = target;
            } else if (!target) {
                L("tick %d：目标不在运行", tick);
            }

            if (access(stopPath, F_OK) == 0) {
                L("检测到 stop 标志，退出");
                unlink(stopPath);
                break;
            }
            sleep(2);
        }

        if (g_log) fclose(g_log);
        return 0;
    }
}
