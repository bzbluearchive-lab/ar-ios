import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"

# ============ 1) package.sh：新增 P2c（P2b + JIT 特权）============
pk = root + r"\tools\package.sh"
s = open(pk, encoding="utf-8").read()

if "P2c.plist" not in s:
    old_line = 'ent build/ents/P2b.plist platform-application com.apple.private.security.no-sandbox task_for_pid-allow com.apple.system-task-ports get-task-allow'
    new_block = old_line + '\n' + \
        '# P2c：在「能启动、能 task_for_pid」的 P2b 基础上，追加允许创建可执行内存的特权。\n' + \
        '# 目的：验证 iOS 的 JIT 限制能否被 entitlement 打开（TrollStore 可授予任意 entitlement）。\n' + \
        'ent build/ents/P2c.plist platform-application com.apple.private.security.no-sandbox task_for_pid-allow com.apple.system-task-ports get-task-allow \\\n' + \
        '                        dynamic-codesigning com.apple.private.cs.dynamic-codesigning run-unsigned-code com.apple.private.amfi.can-execute-cdhash'
    if old_line in s:
        s = s.replace(old_line, new_block)
        print("P2c block added")
    else:
        print("!! P2b line missing")

# 变体切到 P2c
s = re.sub(r'build_variant G\d+ com\.local\.arg\d+ "AR G\d+" build/ents/P2b\.plist ARHost-G\d+\.ipa',
           'build_variant G55 com.local.arg55 "AR G55" build/ents/P2c.plist ARHost-G55.ipa', s)
open(pk, "w", encoding="utf-8", newline="\n").write(s)
print("variant:", "ARHost-G55.ipa" in s, "| P2c:", "P2c.plist" in s)

# ============ 2) host：自测E（JIT 探测）============
hp = root + r"\host\main.mm"
h = open(hp, encoding="utf-8").read()

jit = r'''
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

'''
if "ARSelfTestJIT" not in h:
    h = h.replace("static void ARDumpBreadcrumbs(void);", jit + "static void ARDumpBreadcrumbs(void);", 1)

if "@selector(onSelfJIT)" not in h:
    anchor = "    [self.view addSubview:stBtn4];"
    add = '''    [self.view addSubview:stBtn4];

    UIButton* stBtn5 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn5.frame = CGRectMake(pad, 540, w - pad*2, 36);
    [stBtn5 setTitle:@"自测E：JIT 能力探测（可否执行新代码）" forState:UIControlStateNormal];
    [stBtn5 addTarget:self action:@selector(onSelfJIT) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn5];'''
    h = h.replace(anchor, add, 1)
    print("E button added")

if "- (void)onSelfJIT {" not in h:
    h = h.replace("- (void)onSelfFileMap {", '''- (void)onSelfJIT {
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

- (void)onSelfFileMap {''', 1)
    print("E method added")

for inc in ("#include <sys/mman.h>", "#include <errno.h>"):
    if inc not in h:
        h = h.replace("#include <unistd.h>", "#include <unistd.h>\n" + inc, 1)

h = h.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 546, w - pad*2, self.view.bounds.size.height - 566)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 582, w - pad*2, self.view.bounds.size.height - 602)];")

open(hp, "w", encoding="utf-8", newline="\n").write(h)

print("has JIT fn:", "ARSelfTestJIT" in h, "| has method:", "onSelfJIT {" in h)

depth = 0
for ln in h.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
