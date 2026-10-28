import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

if "KfdBackend.h" not in s:
    s = s.replace('#include "../src/Inject/CodeCave.h"',
                  '#include "../src/Inject/CodeCave.h"\n#include "../src/Kernel/KfdBackend.h"', 1)

fn = r'''
// ---------------------------------------------------------------------------
// kfd 自检：验证内核读写是否真的拿到
//   method: 0=physpuppet 1=landa 2=smith
// 判定标准（全过才算成功）：命中版本条目 → kopen 成功 → 内核基址 magic 校验
//   → current_proc 的 p_pid == 本进程 → kernel_proc 的 p_pid == 0
// ---------------------------------------------------------------------------
static void ARKfdSelfTest(int method) {
    const char* name = (method == 0) ? "physpuppet" : (method == 1 ? "landa" : "smith");
    printf("\n=== kfd 自检 (%s) ===\n", name);
    printf("[kfd-test] 设备: %s / iOS %s / build %s\n",
           buildtable::current_machine(), buildtable::product_version(), buildtable::current_build());
    fflush(stdout);

    bool ok = kfdb::selftest(method);
    printf("[kfd-test] 结论: %s\n", ok ? "内核读写可用" : "未拿到内核读写（看上面 [kfd] 日志的失败步）");
    printf("=== kfd 自检结束 ===\n");
    fflush(stdout);
}

'''
if "ARKfdSelfTest" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn + "static void ARDumpBreadcrumbs(void);", 1)

# 两个按钮
if "@selector(onKfdPhys)" not in s:
    anchor = "    [self.view addSubview:cvBtn];"
    add = '''    [self.view addSubview:cvBtn];

    UIButton* kfd1 = [UIButton buttonWithType:UIButtonTypeSystem];
    kfd1.frame = CGRectMake(pad, 646, (w - pad*3) / 2, 38);
    [kfd1 setTitle:@"kfd 自检：physpuppet" forState:UIControlStateNormal];
    [kfd1 addTarget:self action:@selector(onKfdPhys) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:kfd1];

    UIButton* kfd2 = [UIButton buttonWithType:UIButtonTypeSystem];
    kfd2.frame = CGRectMake(pad*2 + (w - pad*3) / 2, 646, (w - pad*3) / 2, 38);
    [kfd2 setTitle:@"kfd 自检：landa" forState:UIControlStateNormal];
    [kfd2 addTarget:self action:@selector(onKfdLanda) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:kfd2];'''
    s = s.replace(anchor, add, 1)

if "- (void)onKfdPhys {" not in s:
    s = s.replace("- (void)onCaveProbe {", '''- (void)onKfdPhys {
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

- (void)onCaveProbe {''', 1)

# 日志区下移
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 646, w - pad*2, self.view.bounds.size.height - 666)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 690, w - pad*2, self.view.bounds.size.height - 710)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("fn:", "ARKfdSelfTest" in s, "| buttons:", "onKfdPhys" in s, "onKfdLanda" in s)
