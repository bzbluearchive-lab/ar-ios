import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ---------- 1) ManualLoad：新增 execMode==2 = 只映射不执行 ----------
mp = root + r"\src\Inject\ManualLoad.h"
m = open(mp, encoding="utf-8").read()

m = m.replace('''            if (execMode == 1) {''',
'''            if (execMode == 2) {
                // 只映射、不执行：验证镜像搬运与权限设置本身是否安全
                printf("[mload] 执行模式=只映射（不做任何线程操作）\\n");
                printf("[mload] 映射完成，入口就绪 @ 0x%llx，未执行\\n", (unsigned long long)entry);
                ranOk = true;
                entryRan = true;   // 映射成功即视为本次任务完成
            } else if (execMode == 1) {''')

# 每个关键步骤前打一条标记（无缓冲已开，直接落盘）
m = m.replace('    // 2) 目标里分配\n', '''    // 2) 目标里分配
    printf("[step] before_vm_allocate\\n");
''')
m = m.replace('    // 4) 修重定位\n', '''    // 4) 修重定位
    printf("[step] before_fixups\\n");
''')
m = m.replace('    // 5) 设段权限\n', '''    // 5) 设段权限
    printf("[step] before_protect\\n");
''')
m = m.replace('    vm_address_t slot = 0;   // payload 结果槽（作用域要覆盖整个入口流程）',
              '    printf("[step] before_entry_block\\n");\n    vm_address_t slot = 0;   // payload 结果槽（作用域要覆盖整个入口流程）')

open(mp, "w", encoding="utf-8", newline="\n").write(m)
print("execMode==2 added:", "执行模式=只映射" in m)
print("step markers:", m.count("[step]"))

# ---------- 2) call_addr 内部也加标记 ----------
m2 = open(mp, encoding="utf-8").read()
m2 = m2.replace('''static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop,
                                      uint64_t x0 = 0, uint64_t x18 = 0, uint64_t x1 = 0) {
    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(task, &th);''',
'''static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop,
                                      uint64_t x0 = 0, uint64_t x18 = 0, uint64_t x1 = 0) {
    mach_port_t th = MACH_PORT_NULL;
    printf("[step] thread_create 前\\n");
    kern_return_t kr = thread_create(task, &th);
    printf("[step] thread_create -> %d\\n", kr);''')
m2 = m2.replace('''    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st,
                          ARM_THREAD_STATE64_COUNT);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);''',
'''    printf("[step] set_state 前 (pc=0x%llx sp=0x%llx)\\n",
           (unsigned long long)st.__pc, (unsigned long long)st.__sp);
    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st,
                          ARM_THREAD_STATE64_COUNT);
    printf("[step] set_state -> %d\\n", kr);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);
    printf("[step] resume -> %d\\n", kr);''')
open(mp, "w", encoding="utf-8", newline="\n").write(m2)
print("call_addr markers:", m2.count("[step]"))

# ---------- 3) host：两个自测按钮 ----------
hp = root + r"\host\main.mm"
h = open(hp, encoding="utf-8").read()

h = h.replace('static void ARSelfTest(void) {', 'static void ARSelfTest(int mode) {')
h = h.replace('''    mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false, 1);
    printf("=== 自注入自测结束 ===\\n");''',
'''    printf("[self] mode=%d (%s)\\n", mode, mode == 2 ? "只映射" : "映射+新建线程执行");
    mload::map_and_call(task, dylib.UTF8String, "arp_probe", nullptr, false, mode);
    printf("=== 自注入自测结束（%s）===\\n", mode == 2 ? "只映射" : "映射+执行");''')

# 按钮：只映射 + 映射并执行
old_btn = '''    UIButton* stBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn.frame = CGRectMake(pad, 432, w - pad*2, 40);
    [stBtn setTitle:@"自注入自测（打本进程）" forState:UIControlStateNormal];
    [stBtn addTarget:self action:@selector(onSelfTest) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn];'''
new_btn = '''    UIButton* stBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn.frame = CGRectMake(pad, 428, (w - pad*3) / 2, 38);
    [stBtn setTitle:@"自测A：只映射" forState:UIControlStateNormal];
    [stBtn addTarget:self action:@selector(onSelfMapOnly) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn];

    UIButton* stBtn2 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn2.frame = CGRectMake(pad*2 + (w - pad*3) / 2, 428, (w - pad*3) / 2, 38);
    [stBtn2 setTitle:@"自测B：映射+执行" forState:UIControlStateNormal];
    [stBtn2 addTarget:self action:@selector(onSelfExec) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn2];'''
if old_btn in h:
    h = h.replace(old_btn, new_btn)
    print("two self-test buttons")
else:
    print("!! self button anchor missing")

# 方法：两个
h = h.replace('''- (void)onSelfTest {
    self.status.text = @"自注入自测中…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTest();''',
'''- (void)onSelfMapOnly { [self runSelfTest:2]; }
- (void)onSelfExec    { [self runSelfTest:1]; }

- (void)runSelfTest:(int)mode {
    self.status.text = mode == 2 ? @"自测A：只映射…" : @"自测B：映射+执行…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARSelfTest(mode);''')

h = h.replace('''            self.status.text = @"自测完成，看 [mload] 结果槽";''',
'''            self.status.text = @"自测结束（若崩溃，重开本 app 后点导出日志）";''')

open(hp, "w", encoding="utf-8", newline="\n").write(h)
print("runSelfTest:", "runSelfTest:(int)mode" in h)

# ---------- 4) 括号自检 + 审查 ----------
for f in (mp, hp):
    src = open(f, encoding="utf-8").read()
    depth = 0
    for ln in src.split("\n"):
        tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
        for ch in tt:
            if ch == '{': depth += 1
            elif ch == '}': depth -= 1
    print(f.split("\\")[-1], "depth =", depth)
