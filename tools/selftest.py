import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
p = root + r"\host\main.mm"
s = open(p, encoding="utf-8").read()

# ---- 1) 自注入自测函数 ----
fn = r'''
// ---------------------------------------------------------------------------
// 自注入自测：把 payload 映射进**本进程**并尝试执行。
// 目的：在不碰游戏的前提下，用最快速度验证"新建线程能否执行映射进来的代码"。
//   - 本进程内成功 -> 代码没问题，游戏里的失败是目标环境差异（沙盒/AMCE）
//   - 本进程内也失败 -> 是我们的注入实现有问题，可以本地反复调，不用牺牲游戏
// ---------------------------------------------------------------------------
static void ARSelfTest(void) {
    printf("\n=== 自注入自测（目标 = 本进程 %d）===\n", getpid());

    NSString* dylib = [[NSBundle mainBundle] pathForResource:@"ARPayloadManual" ofType:@"dylib"];
    if (!dylib) { printf("[self] 包里找不到 payload\n"); return; }
    printf("[self] payload=%s\n", dylib.UTF8String);

    mach_port_t task = mach_task_self();
    printf("[self] 挂起状态检查: ");
    int susp = -1;
    bool running = mload::target_is_running(task, &susp);
    printf("running=%d suspend_count=%d\n", running, susp);

    mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false);
    printf("=== 自注入自测结束 ===\n");
}

'''
if "ARSelfTest" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);   // 前向声明", fn + "static void ARDumpBreadcrumbs(void);   // 前向声明")

# ---- 2) 按钮 ----
btn = r'''
    // 自注入自测：把 payload 打进本进程，验证执行链路（不碰游戏）
    UIButton* self = [UIButton buttonWithType:UIButtonTypeSystem];
    self.frame = CGRectMake(pad, 432, w - pad*2, 40);
    [self setTitle:@"自注入自测（打本进程）" forState:UIControlStateNormal];
    [self addTarget:self action:@selector(onSelfTest) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self];

'''
if "自注入自测（打本进程）" not in s:
    s = s.replace("    // 只扫描不注入：安全的诊断步骤，绝不碰任何目标进程的内存", btn + "    // 只扫描不注入：安全的诊断步骤，绝不碰任何目标进程的内存")

# ---- 3) 日志视图下移 & 方法 ----
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 434, w - pad*2, self.view.bounds.size.height - 454)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 478, w - pad*2, self.view.bounds.size.height - 498)];")

meth = r'''- (void)onSelfTest {
    self.status.text = @"自注入自测中…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTest();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"自测完成，看 [mload] 结果槽";
        });
    });
}

- (void)onExport {
'''
if "onSelfTest" not in s:
    s = s.replace("- (void)onExport {", meth)

open(p, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("has self test:", "ARSelfTest" in s, "onSelfTest" in s)
