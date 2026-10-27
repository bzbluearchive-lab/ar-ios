import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

# ---- 1) 已签名代码探针：位于我们自己的 __TEXT（已签名），与手工映射页形成对照 ----
probe = r'''
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

'''
if "arSignedProbe" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", probe + "static void ARDumpBreadcrumbs(void);")

# ---- 2) 自测C 函数 ----
fn_c = r'''
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

'''
if "ARSelfTestSigned" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn_c + "static void ARDumpBreadcrumbs(void);")

# ---- 3) 按钮 + 方法 ----
btn_new = '''    UIButton* stBtn2 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn2.frame = CGRectMake(pad*2 + (w - pad*3) / 2, 428, (w - pad*3) / 2, 38);
    [stBtn2 setTitle:@"自测B：映射+执行" forState:UIControlStateNormal];
    [stBtn2 addTarget:self action:@selector(onSelfExec) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn2];

    UIButton* stBtn3 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn3.frame = CGRectMake(pad, 470, w - pad*2, 40);
    [stBtn3 setTitle:@"自测C：执行已签名页（对照）" forState:UIControlStateNormal];
    [stBtn3 addTarget:self action:@selector(onSelfSigned) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn3];
'''
if "自测C：执行已签名页" not in s:
    s = s.replace('''    [stBtn2 addTarget:self action:@selector(onSelfExec) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn2];''', btn_new)

meth_new = '''- (void)onSelfSigned {
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

- (void)onSelfMapOnly {'''
if "onSelfSigned" not in s:
    s = s.replace("- (void)onSelfMapOnly {", meth_new)

# 日志视图下移
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 478, w - pad*2, self.view.bounds.size.height - 498)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 516, w - pad*2, self.view.bounds.size.height - 536)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("has sig probe:", "arSignedProbe" in s, "| has test C:", "ARSelfTestSigned" in s, "| method:", "onSelfSigned {" in s)
