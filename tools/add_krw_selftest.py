import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

if "KrwProc.h" not in s:
    s = s.replace('#include "../src/Kernel/KfdBackend.h"',
                  '#include "../src/Kernel/KfdBackend.h"\n#include "../src/Kernel/KrwProc.h"', 1)

fn = r'''
// ---------------------------------------------------------------------------
// 跨进程读自检（页表遍历）
//
// 先只读**我们自己的内存**：拿自己的已知 canary 值，通过
//   kernel_proc → p_list → 本进程 proc → proc_ro → task → map → pmap → 页表
// 读回来比对。这一步不碰游戏，能一次性验证：
//   ① proc→proc_ro(0x18)→pr_task(0x8) 的偏移对不对
//   ② 16KB 粒度页表遍历 + PTE 解析对不对
//   ③ kread 在真机上对"用户页的影子地址"是否可读
// 这三条都过，才谈得上读游戏内存。
// ---------------------------------------------------------------------------
static void ARKrwSelfTest(void) {
    printf("\n=== 跨进程读自检（页表遍历，目标=本进程）===\n");
    fflush(stdout);

    kfdb::Client c;
    if (!c.init(0)) {                 // 0 = physpuppet（真机已验证可用）
        printf("[krw-test] kfd 初始化失败，先看上一节 [kfd] 日志\n");
        return;
    }
    printf("[krw-test] kfd 就绪 handle=0x%llx\n", (unsigned long long)c.handle);

    krwproc::Ctx kc;
    if (!krwproc::init(&kc, c.handle)) {
        printf("[krw-test] krwproc::init 失败\n");
        c.shutdown();
        return;
    }
    printf("[krw-test] kernel_proc=0x%llx\n", (unsigned long long)kc.kernel_proc);

    bool ok = krwproc::selftest_self(kc);
    printf("[krw-test] 结论: %s\n", ok ? "页表遍历可用（可继续读游戏内存）"
                                      : "页表自检失败（看上面 [krw] 哪一步断）");
    c.shutdown();
    printf("=== 自检结束 ===\n");
    fflush(stdout);
}

'''
if "ARKrwSelfTest" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn + "static void ARDumpBreadcrumbs(void);", 1)

if "@selector(onKrwTest)" not in s:
    anchor = "    [self.view addSubview:kfd2];"
    add = '''    [self.view addSubview:kfd2];

    UIButton* krwBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    krwBtn.frame = CGRectMake(pad, 690, w - pad*2, 38);
    [krwBtn setTitle:@"跨进程读自检（页表遍历，读自己）" forState:UIControlStateNormal];
    [krwBtn addTarget:self action:@selector(onKrwTest) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:krwBtn];'''
    s = s.replace(anchor, add, 1)

if "- (void)onKrwTest {" not in s:
    s = s.replace("- (void)onKfdPhys {", '''- (void)onKrwTest {
    self.status.text = @"页表自检中…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARKrwSelfTest();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"页表自检结束";
        });
    });
}

- (void)onKfdPhys {''', 1)

# 日志区下移，避免和按钮重叠
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 690, w - pad*2, self.view.bounds.size.height - 710)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 736, w - pad*2, self.view.bounds.size.height - 756)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("fn:", "ARKrwSelfTest" in s, "| button:", "onKrwTest" in s)

# 顺手把 KfdBackend.h 里"10 个函数"改成 11（文档准确性）
kb = root + r"\src\Kernel\KfdBackend.h"
t = open(kb, encoding="utf-8").read()
if "10 个 extern" in t:
    t = t.replace("10 个 extern", "11 个 extern")
    open(kb, "w", encoding="utf-8", newline="\n").write(t)
    print("KfdBackend.h 注释已更新为 11 个函数")
