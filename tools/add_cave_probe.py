import re

root = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios"
hp = root + r"\host\main.mm"
s = open(hp, encoding="utf-8").read()

if "CodeCave.h" not in s:
    s = s.replace('#include "../src/Inject/TaskPorts.h"',
                  '#include "../src/Inject/TaskPorts.h"\n#include "../src/Inject/CodeCave.h"', 1)

fn = r'''
// ---------------------------------------------------------------------------
// code cave 探测（无害验证）
//
// 目的：在**不执行任何东西**的前提下量出三件事
//   1) 能不能用 processor_set_tasks 拿到游戏 task port
//   2) 能不能在游戏 __TEXT 里找到可用空洞（code cave）
//   3) 能不能对 App Store 签名的 __TEXT 页做 vm_protect(R|W|COPY) + vm_write
//      —— 做法是把洞里"原本就有的那个字节"原样写回去，
//         语义零改变，但权限链路完整走了一遍。
//
// 第 3 点是巨魔方案的命门：能写才有后面的 hook；不能写（返回 4 = 权限不足）
// 就直接说明这条路要放弃，转 kfd。
// ---------------------------------------------------------------------------
static void ARProbeCodeCave(void) {
    printf("\n=== code cave 探测 ===\n");

    pid_t game = rload::find_pid_comm("UAGame");
    printf("[cave-test] 游戏 pid=%d\n", game);
    if (!game) { printf("[cave-test] 游戏没在运行\n"); return; }

    taskports::Found tp = taskports::find_by_pid(game);
    if (!tp.port) { printf("[cave-test] 拿不到 task port，后面无从谈起\n"); return; }

    // 目标主镜像基址（TASK_DYLD_INFO）
    task_dyld_info_data_t di;
    mach_msg_type_number_t dc = TASK_DYLD_INFO_COUNT;
    if (task_info(tp.port, TASK_DYLD_INFO, (task_info_t)&di, &dc) != KERN_SUCCESS) {
        printf("[cave-test] TASK_DYLD_INFO 失败\n");
        return;
    }
    uint64_t arr = 0, imageBase = 0;
    vm_size_t got = 0;
    vm_read_overwrite(tp.port, (vm_address_t)(di.all_image_info_addr + 8), 8, (vm_address_t)&arr, &got);
    vm_read_overwrite(tp.port, (vm_address_t)arr, 8, (vm_address_t)&imageBase, &got);
    printf("[cave-test] 游戏主镜像基址=0x%llx\n", (unsigned long long)imageBase);
    if (!imageBase) return;

    // 段表：从本机可读的二进制文件解析
    char binPath[1024] = {0};
    if (!rload::read_remote_path(tp.port, binPath, sizeof(binPath)) || !binPath[0]) {
        printf("[cave-test] 读不到主二进制路径\n");
        return;
    }
    printf("[cave-test] 二进制=%s\n", binPath);

    cave::Seg segs[32];
    int nseg = 0;
    if (!cave::list_segments(tp.port, binPath, segs, 32, &nseg)) {
        printf("[cave-test] 段表解析失败\n");
        return;
    }

    // 找 __TEXT 范围
    uint64_t textVa = 0, textSize = 0, textVmaddr = 0, textVmsize = 0;
    for (int i = 0; i < nseg; i++) {
        if (strcmp(segs[i].name, "__TEXT") == 0) {
            textVmaddr = segs[i].vmaddr;
            textVmsize = segs[i].vmsize;
        }
    }
    if (!textVmsize) { printf("[cave-test] 没找到 __TEXT\n"); return; }
    textVa = imageBase + (textVmaddr - textVmaddr);   // __TEXT 本身就是基准
    textSize = textVmsize;

    // 在 __TEXT 后半段找洞（前半段是代码，尾部通常是 padding）
    uint64_t scanFrom = textVa + textSize / 2;
    uint64_t scanTo = textVa + textSize;
    uint64_t c = cave::find_cave(tp.port, imageBase, scanFrom, 16, scanTo);
    printf("[cave-test] 扫描区间 [0x%llx, 0x%llx) 空洞=0x%llx\n",
           (unsigned long long)scanFrom, (unsigned long long)scanTo, (unsigned long long)c);
    if (!c) { printf("[cave-test] 没找到空洞（可能全被代码占满）\n"); return; }

    // 读洞里原本的 4 字节
    uint32_t orig = 0;
    if (!cave::read_mem(tp.port, c, &orig, 4)) {
        printf("[cave-test] 读洞里数据失败\n");
        return;
    }
    printf("[cave-test] 洞里原值 = 0x%08x，将原样写回（语义零改变，只测权限）\n", orig);

    bool w = cave::write_bytes(tp.port, c, &orig, 4);
    printf("[cave-test] 写入结果 = %d\n", w);
    if (w) {
        uint32_t back = 0;
        cave::read_mem(tp.port, c, &back, 4);
        printf("[cave-test] 回读 = 0x%08x  %s\n", back, back == orig ? "一致" : "不一致!");
        printf("[cave-test] 结论: **可以写游戏 __TEXT** -> code cave hook 路线可行\n");
    } else {
        printf("[cave-test] 结论: 写不进去（看上面 vm_protect 的返回码；4=权限不足）\n");
    }
    printf("=== 探测结束 ===\n");
}

'''
if "ARProbeCodeCave" not in s:
    s = s.replace("static void ARDumpBreadcrumbs(void);", fn + "static void ARDumpBreadcrumbs(void);", 1)

if "@selector(onCaveProbe)" not in s:
    anchor = "    [self.view addSubview:tpBtn];"
    add = '''    [self.view addSubview:tpBtn];

    UIButton* cvBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    cvBtn.frame = CGRectMake(pad, 604, w - pad*2, 34);
    [cvBtn setTitle:@"code cave 探测（试写同值）" forState:UIControlStateNormal];
    [cvBtn addTarget:self action:@selector(onCaveProbe) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:cvBtn];'''
    s = s.replace(anchor, add, 1)

if "- (void)onCaveProbe {" not in s:
    s = s.replace("- (void)onTaskPorts {", '''- (void)onCaveProbe {
    self.status.text = @"探测 code cave…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ARRedirectToLog();
        ARProbeCodeCave();
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString* log = [NSString stringWithContentsOfFile:daemonLogPath()
                                                     encoding:NSUTF8StringEncoding error:nil];
            self.logView.text = log ?: @"(没有日志)";
            self.status.text = @"探测结束";
        });
    });
}

- (void)onTaskPorts {''', 1)

# 日志区再下移
s = s.replace("self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 612, w - pad*2, self.view.bounds.size.height - 632)];",
              "self.logView = [[UITextView alloc] initWithFrame:CGRectMake(pad, 646, w - pad*2, self.view.bounds.size.height - 666)];")

open(hp, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    tt = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in tt:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
print("host depth =", depth)
print("fn:", "ARProbeCodeCave" in s, "| btn:", "onCaveProbe" in s)
