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
            needles[nNeedles++] = "DeltaForce";
            needles[nNeedles++] = "Arena";
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

            // 优先按 bundle id 精确定位
            pid_t target = bundleinfo::find_pid_by_bundle("com.proximabeta.mf.uamo", 1024, nullptr, 0);
            const char* how = "bundle";
            if (!target) {
                target = rload::find_by_needles(1024, needles, nNeedles);
                how = "path";
            }

            // 没命中时，每 5 次循环列一次所有可读路径，方便直接看出游戏叫什么
            if (!target && (tick % 5 == 1)) {
                L("未命中，列出当前可读路径：");
                rload::dump_readable(1024, 20);
            }

            if (target && target != lastInjected) {
                L("目标 pid=%d（%s），开始手动映射注入", target, how);
                mach_port_t task = MACH_PORT_NULL;
                kern_return_t kr = task_for_pid(mach_task_self(), target, &task);
                L("task_for_pid(%d) -> %d", target, kr);
                if (kr == KERN_SUCCESS) {
                    uintptr_t rb = mload::map_and_call(task, payload, "arp_entry");
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
