import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

# ---- 1) include ----
if "FileMap.h" not in s:
    s = s.replace('#include "../src/Inject/ManualLoad.h"',
                  '#include "../src/Inject/ManualLoad.h"\n#include "../src/Inject/FileMap.h"')

# ---- 2) 自测D：文件映射 + 执行（先在本进程验证机制）----
fn = r'''
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

'''
if "ARSelfTestFileMap" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn + "static void ARDumpBreadcrumbs(void);")

# ---- 3) 按钮（放在自测C 下面）----
if "自测D：文件映射" not in s:
    anchor = "    [self.view addSubview:stBtn3];"
    add = '''    [self.view addSubview:stBtn3];

    UIButton* stBtn4 = [UIButton buttonWithType:UIButtonTypeSystem];
    stBtn4.frame = CGRectMake(pad, 504, w - pad*2, 36);
    [stBtn4 setTitle:@"自测D：文件映射+执行（已签名文件页）" forState:UIControlStateNormal];
    [stBtn4 addTarget:self action:@selector(onSelfFileMap) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:stBtn4];'''
    s = s.replace(anchor, add, 1)

# ---- 4) 方法 ----
if "onSelfFileMap {" not in s:
    s = s.replace("- (void)onSelfSigned {", '''- (void)onSelfFileMap {
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

- (void)onSelfSigned {''', 1)

# ---- 5) 日志区下移 ----
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 516, w - pad*2, self.view.bounds.size.height - 536)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 546, w - pad*2, self.view.bounds.size.height - 566)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("has D:", "ARSelfTestFileMap" in s, "| button:", "自测D：文件映射" in s, "| method:", "onSelfFileMap {" in s)
