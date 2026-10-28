import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

# 1) include
if "TaskPorts.h" not in s:
    s = s.replace('#include "../src/Inject/FileMap.h"',
                  '#include "../src/Inject/FileMap.h"\n#include "../src/Inject/TaskPorts.h"', 1)

# 2) 测试函数：用 processor_set_tasks 拿游戏 task port 并读主镜像
fn = r'''
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

'''
if "ARTestTaskPorts" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn + "static void ARDumpBreadcrumbs(void);", 1)

# 3) 按钮
if "@selector(onTaskPorts)" not in s:
    anchor = "    [self.view addSubview:stBtn5];"
    add = '''    [self.view addSubview:stBtn5];

    UIButton* tpBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    tpBtn.frame = CGRectMake(pad, 570, w - pad*2, 34);
    [tpBtn setTitle:@"取任务端口（processor_set_tasks）" forState:UIControlStateNormal];
    [tpBtn addTarget:self action:@selector(onTaskPorts) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:tpBtn];'''
    s = s.replace(anchor, add, 1)

# 4) 方法
if "- (void)onTaskPorts {" not in s:
    s = s.replace("- (void)onSelfJIT {", '''- (void)onTaskPorts {
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

- (void)onSelfJIT {''', 1)

# 5) 日志区下移
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 582, w - pad*2, self.view.bounds.size.height - 602)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 612, w - pad*2, self.view.bounds.size.height - 632)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("fn:", "ARTestTaskPorts" in s, "| btn:", "onTaskPorts" in s)
