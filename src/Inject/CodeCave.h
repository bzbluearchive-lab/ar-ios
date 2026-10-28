// CodeCave.h —— 不新建可执行内存，改成往目标进程自己已签名的 __TEXT 页里写跳板
//
// ── 为什么走这条路 ──────────────────────────────────────────────
// 真机上已经确认的两条事实（不是推测）：
//   1) 在目标进程里 vm_allocate 一块匿名内存、再 vm_protect 成 RX、把 PC 打过去
//      → 目标进程被系统杀掉。iOS 上"既能写又能执行"的匿名页需要 CS_DEBUGGED /
//        MAP_JIT 这类 entitlement，App Store 签名的进程一个都没有。
//   2) 执行目标进程自身已经签过名的代码页 → 正常。
// 于是本文件的策略是：把跳板写进目标 __TEXT 段里**本来就存在的空洞**（code cave），
// 再用一条分支指令把某个高频函数的入口改跳到那段空洞。
// 全程没有新增任何可执行内存，跑的一直是同一个被签名的 __TEXT 段所在的那些页。
//
// ── 成功率的真正变量：AMFI 会不会重新校验"已驻留"的代码页 ──────────
// 代码签名不是加载时的一次性动作，下面两个时刻系统可能再看一眼页内容：
//   1) 提升页权限时。我们把页 vm_protect 成 RW（带 VM_PROT_COPY 做私有 COW 拷贝），
//      内核会判断该页是否属于某个已签名映射；不许改写的映射直接返回
//      KERN_PROTECTION_FAILURE。这一步失败时我们**一个字节都还没写**，
//      目标进程是干净的，只是这条路走不通。
//   2) 页内容被改之后。某些配置下 code signing 会在页被 fault / 被取指时校验内容
//      （cs_invalid_page 那条路径），校验不过的处理是**直接 SIGKILL**。
// 所以本文件真正在做实验的问题是：目标 App 的 __TEXT 空洞是否落在 code directory
// 哈希覆盖范围之外，以及写完以后会不会触发上面第 2 条。
//
// ── 失败时的现象（用来判断卡在哪一步）───────────────────────────
//   A. install_branch 立刻返回 false，日志是 vm_protect -> 4
//      → 卡在第 1 条，页不许写。进程完全没事，可以换区间/换函数再试。
//   B. vm_protect 成功、vm_write 成功、回读也一致，但目标在几百毫秒内消失
//      → 命中第 2 条。现象：没有崩溃报告，只有 CODESIGNING / AMFI 日志，进程被 SIGKILL，
//        下一次 attach 会直接报 task port 失效。
//   C. 全部返回成功、进程也活着，但跳板从不执行（入口那条 B 像没生效）
//      → 编码/对齐问题，或者那个函数根本没被再调用，或者 I-cache 没失效。
//        先用 selftest_branch_encoding() 排掉编码，再确认函数是不是真的高频。
//
// ── arm64e 与 PAC（先按 arm64 做，差别全在这里）────────────────────
//   * B / BR 都是直接跳转或寄存器间接跳转，本身不经过 PAC 认证，
//     所以跳板这条路径不会被 PAC 拦下来。
//   * PAC 的风险不在跳转，在**指针**：目标里可能有别处存着"函数入口的 PAC 签名指针"，
//     我们改入口指令不影响那个指针；但跳板若要"跳回原函数 +N"，必须用真实
//     （未签名）地址去跳，不能拿带 PAC 的指针直接 BR/BLR（会认不过而崩）。
//   * arm64e 函数入口常见 PACIBSP 前导。入口前 4/16 字节被改成跳板后，原来的前导
//     就被绕过了。但注意"被搬走的指令"里可能就有 PACIBSP，重放它要和函数尾的
//     AUTIBSP 配对，很容易配错 —— 所以**优先选 4 字节 B 方案**（只搬走 1 条指令，
//     并且那条指令一般不需要回来执行）。
//
// ── 标准用法 ───────────────────────────────────────────────────
//   1) list_segments() 读本机上那份 Mach-O，拿到 __TEXT 的 vmaddr / vmsize；
//      目标地址 = imageBase + (seg.vmaddr - text.vmaddr)
//   2) find_cave() 在 __TEXT 区间里找 >= 16 字节的连续空洞 → caveVa
//   3) write_bytes() 把跳板写进 caveVa
//   4) install_branch(task, from, caveVa, &patch) 把函数入口改跳
//   5) restore(task, &patch) 回滚
//
// 只依赖 iOS SDK 一定提供的头。注意：不能用 mach_vm.h（iOS SDK 里会 #error），
// 一律走 vm_* 系列：vm_read_overwrite / vm_write / vm_protect / vm_allocate。

#pragma once
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// pid_for_task 在 iOS SDK 的公开头里没暴露，但 libSystem 里有。
// 这里与 TaskPorts.h 用同一套做法：自己声明，签名与内核侧完全一致。
extern "C" kern_return_t pid_for_task(mach_port_t task, int* pid);

namespace cave {

// ============================ 常量 ============================
// ARM64 指令定长 4 字节，所以一切对齐都以 4 为单位。
static const uint64_t kInsnAlign = 4;

// 页对齐粒度。iOS arm64 用户态页是 16KB；这里统一按 16KB 做下取整/上取整，
// 对 4KB 页的系统同样正确 —— 16KB 是 4KB 的整数倍，按 16KB 对齐出来的区间
// 必然也是 4KB 对齐的，vm_protect 不会因为"地址未页对齐"而拒绝。
static const uint64_t kPageAlign = 0x4000;

// find_cave 每次读 64KB：一次读太大容易整块失败（__TEXT 里偶有不可读的间隙），
// 而且 64KB 远在 vm_read_overwrite 单次可用范围之内。
static const size_t kScanChunk = 64 * 1024;

// load command 缓冲区上限：大 App 的 sizeofcmds 一般 10KB 到 200KB。
static const size_t kLcMax = 256 * 1024;

// 超范围跳板固定 4 条指令 = 16 字节，正好等于 Patch.saved 的容量。
static const int kStubWords = 4;
static const int kStubBytes = kStubWords * 4;

// ====================== 远程内存小工具 ======================
// 只做"原样搬运"，权限相关的处理统一放在 write_bytes() 里，职责不混。
inline bool read_mem(mach_port_t task, uint64_t addr, void* dst, size_t len) {
    if (!task || !dst || len == 0) {
        printf("[cave] read_mem 参数非法 (task=%u dst=%p len=%llu)\n", task, dst,
               (unsigned long long)len);
        return false;
    }
    vm_size_t got = 0;
    kern_return_t kr = vm_read_overwrite(task, (vm_address_t)addr, (vm_size_t)len,
                                         (vm_address_t)dst, &got);
    if (kr != KERN_SUCCESS) {
        printf("[cave] vm_read_overwrite(0x%llx, %llu) -> %d\n",
               (unsigned long long)addr, (unsigned long long)len, kr);
        return false;
    }
    if ((size_t)got != len) {
        printf("[cave] vm_read_overwrite 只读到 %llu / %llu 字节 @0x%llx\n",
               (unsigned long long)got, (unsigned long long)len, (unsigned long long)addr);
        return false;
    }
    return true;
}

// 不带任何权限处理的写；调用方必须已经保证该地址可写（一般走 write_bytes）。
inline bool write_mem_raw(mach_port_t task, uint64_t addr, const void* src, size_t len) {
    if (!task || !src || len == 0) {
        printf("[cave] write_mem_raw 参数非法\n");
        return false;
    }
    kern_return_t kr = vm_write(task, (vm_address_t)addr, (vm_offset_t)src,
                                (mach_msg_type_number_t)len);
    if (kr != KERN_SUCCESS) {
        printf("[cave] vm_write(0x%llx, %llu) -> %d\n",
               (unsigned long long)addr, (unsigned long long)len, kr);
        return false;
    }
    return true;
}

// ======================= a. 段表解析 =======================
// 只是把 Mach-O 文件里的 LC_SEGMENT_64 抄出来，不做任何换算。
// 目标进程里的实际地址由调用方算：
//   目标地址 = imageBase + (seg.vmaddr - textVmaddr)
// 其中 textVmaddr = __TEXT 段的 vmaddr，imageBase 是目标进程里 __TEXT 的实际起始地址
// （PIE 镜像带 ASLR 滑动，所以 imageBase 不等于 vmaddr）。
struct Seg {
    char     name[16];   // 段名，如 "__TEXT"；强制以 0 结尾，可直接 printf
    uint64_t vmaddr;     // 文件里的链接地址
    uint64_t vmsize;     // 段占用的虚拟大小
    uint64_t fileoff;    // 段在文件里的起始偏移
    uint64_t filesize;   // 段在文件里的长度（vmsize 可能更大）
    uint32_t initprot;   // 初始保护位：VM_PROT_READ / WRITE / EXECUTE 的组合
};

// 从本机可读的那份 Mach-O 里解析段表（App Store 的二进制即使在设备上，
// 只要路径给对、进程有读权限，就能读到；拿不到文件时只能走别的路子）。
// task 只用于打日志确认我们面对的是哪个 pid，不参与解析。
// 返回 true 表示至少解析出一个段。
inline bool list_segments(mach_port_t task, const char* binPath, Seg* out, int maxOut, int* n) {
    if (n) *n = 0;
    if (!out || maxOut <= 0) {
        printf("[cave] list_segments: 输出数组非法 (out=%p maxOut=%d)\n", out, maxOut);
        return false;
    }
    if (!binPath || !binPath[0]) {
        printf("[cave] list_segments: binPath 为空\n");
        return false;
    }

    // task 在这里的用途：确认我们分析的段表和实际操作的是同一个进程。
    // pid 对不上说明调用方把 task port 和路径配错了，后面所有写入都会打偏。
    if (task != MACH_PORT_NULL) {
        int pid = -1;
        kern_return_t kr = pid_for_task(task, &pid);
        printf("[cave] list_segments: 目标 pid=%d (pid_for_task -> %d)\n", pid, kr);
    } else {
        printf("[cave] list_segments: task 为空，只解析文件\n");
    }

    FILE* f = fopen(binPath, "rb");
    if (!f) {
        printf("[cave] 打不开 %s（路径要写全，且当前进程要能读）\n", binPath);
        return false;
    }

    struct mach_header_64 mh;
    memset(&mh, 0, sizeof(mh));
    if (fread(&mh, 1, sizeof(mh), f) != sizeof(mh)) {
        printf("[cave] %s 读 Mach-O 头失败\n", binPath);
        fclose(f);
        return false;
    }
    if (mh.magic != MH_MAGIC_64) {
        printf("[cave] %s 不是 64 位 Mach-O（magic=0x%08x）\n", binPath, mh.magic);
        fclose(f);
        return false;
    }
    // cputype 低 24 位 = 12 表示 ARM，高 24 位是 ABI64 之类的标志位。
    if ((mh.cputype & 0x00FFFFFFu) != 12u) {
        printf("[cave] %s 不是 ARM（cputype=0x%x）\n", binPath, (unsigned)mh.cputype);
        fclose(f);
        return false;
    }
    if (mh.sizeofcmds == 0 || mh.sizeofcmds > kLcMax) {
        printf("[cave] sizeofcmds=%u 超出缓冲上限（上限 %llu）\n", (unsigned)mh.sizeofcmds,
               (unsigned long long)kLcMax);
        fclose(f);
        return false;
    }

    // load commands 必须连续读出来才能按 cmdsize 走链，所以整块读进静态缓冲。
    // 用 static 而不是栈：这个缓冲 256KB，放在栈上会顶掉线程栈。
    // 注意：这也意味着本函数不可重入，不要并发调用。
    static uint8_t lcbuf[kLcMax];
    memset(lcbuf, 0, (size_t)mh.sizeofcmds);
    if (fseek(f, (long)sizeof(mh), SEEK_SET) != 0) {
        printf("[cave] %s 定位 load commands 失败\n", binPath);
        fclose(f);
        return false;
    }
    if (fread(lcbuf, 1, (size_t)mh.sizeofcmds, f) != (size_t)mh.sizeofcmds) {
        printf("[cave] %s 读 load commands 不足 %u 字节\n", binPath, (unsigned)mh.sizeofcmds);
        fclose(f);
        return false;
    }
    fclose(f);

    int cnt = 0;
    uint64_t textVmaddr = 0;
    bool haveText = false;
    uint32_t off = 0;
    while (off + sizeof(struct load_command) <= mh.sizeofcmds) {
        const struct load_command* lc = (const struct load_command*)(lcbuf + off);
        // cmdsize 至少 8 字节，且不能越过 sizeofcmds —— 坏值会让下面死循环。
        if (lc->cmdsize < sizeof(struct load_command) ||
            off + lc->cmdsize > mh.sizeofcmds) {
            printf("[cave] load command 链在偏移 %u 处损坏（cmd=0x%x cmdsize=%u）\n", off,
                   (unsigned)lc->cmd, (unsigned)lc->cmdsize);
            break;
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (cnt < maxOut) {
                Seg& g = out[cnt];
                memset(&g, 0, sizeof(g));
                // segname 是 char[16] 定长字段；强制末字节为 0 保证 printf 安全。
                memcpy(g.name, s->segname, 16);
                g.name[15] = 0;
                g.vmaddr  = s->vmaddr;
                g.vmsize  = s->vmsize;
                g.fileoff = s->fileoff;
                g.filesize = s->filesize;
                g.initprot = s->initprot;
                if (strcmp(g.name, "__TEXT") == 0) {
                    textVmaddr = g.vmaddr;
                    haveText = true;
                }
                cnt++;
            } else {
                printf("[cave] 段表超过 maxOut=%d，多余的段被丢弃\n", maxOut);
            }
        }
        off += lc->cmdsize;
    }

    if (n) *n = cnt;
    printf("[cave] 共解析出 %d 个段（ncmds=%u sizeofcmds=%u）\n", cnt, (unsigned)mh.ncmds,
           (unsigned)mh.sizeofcmds);
    for (int i = 0; i < cnt; i++) {
        printf("[cave]   %-16s vmaddr=0x%llx vmsize=0x%llx fileoff=0x%llx filesize=0x%llx prot=0x%x\n",
               out[i].name, (unsigned long long)out[i].vmaddr,
               (unsigned long long)out[i].vmsize, (unsigned long long)out[i].fileoff,
               (unsigned long long)out[i].filesize, out[i].initprot);
    }

    if (!haveText) {
        printf("[cave] 没找到 __TEXT 段，无法换算目标地址\n");
        return false;
    }
    // 把换算关系写死在这里，省得调用方自己再推一遍：
    //   目标地址 = imageBase + (seg.vmaddr - textVmaddr)
    //   反过来，目标地址 A 对应的镜像内偏移 = A - imageBase
    printf("[cave] 地址换算: 目标地址 = imageBase + (seg.vmaddr - 0x%llx)\n",
           (unsigned long long)textVmaddr);
    for (int i = 0; i < cnt; i++) {
        printf("[cave]   段偏移: %-16s 相对 __TEXT 的偏移 = 0x%llx，目标区间 = [imageBase+0x%llx, imageBase+0x%llx)\n",
               out[i].name, (unsigned long long)(out[i].vmaddr - textVmaddr),
               (unsigned long long)(out[i].vmaddr - textVmaddr),
               (unsigned long long)(out[i].vmaddr - textVmaddr + out[i].vmsize));
    }
    return cnt > 0;
}

// ======================= b. 找空洞 =======================
// 在目标进程的 [startVa, limitVa) 里找一段"连续 0x00 或连续 0xFF"的空洞，
// 按 4 字节对齐推进（ARM64 指令定长 4，非 4 对齐的洞写不了指令）。
//
// 参数：
//   base      调用方算出的镜像基址（__TEXT 段在目标进程里的实际起始地址）。
//             只用于把结果换算成"镜像内偏移"打日志，不参与扫描判断 —— 这样即使
//             调用方给的 base 有偏差，也不会影响找洞的正确性。
//   startVa   扫描起点（目标进程里的绝对地址）。建议从 __TEXT 里第一个可执行区段
//             之后开始，避开 Mach-O 头和 load commands。
//   sizeNeeded 需要的连续字节数（内部会向上取整到 4 的倍数）。跳板一般是 16。
//   limitVa   扫描终点（不含）。不要超过 __TEXT 的末尾，否则会扫进别的映射。
// 返回可用地址；找不到返回 0（这不是错误：__TEXT 大多数时候真的没有洞）。
//
// 每次读 64KB，跨块的空洞用 runLen / runStart 在块与块之间续上。
// 不可读的块跳过而不是整段放弃：__TEXT 里偶有未映射的间隙。
// 用了 static 缓冲，所以不可重入，不要并发调用。
inline uint64_t find_cave(mach_port_t task, uint64_t base, uint64_t startVa,
                          uint64_t sizeNeeded, uint64_t limitVa) {
    if (!task) {
        printf("[cave] find_cave: task 无效\n");
        return 0;
    }
    if (sizeNeeded == 0) {
        printf("[cave] find_cave: sizeNeeded=0\n");
        return 0;
    }
    // 需求长度也按 4 字节上取整：4 字节对齐的洞才放得下整条指令。
    sizeNeeded = (sizeNeeded + (kInsnAlign - 1)) & ~(kInsnAlign - 1);
    if (limitVa <= startVa) {
        printf("[cave] find_cave: 区间非法 startVa=0x%llx limitVa=0x%llx\n",
               (unsigned long long)startVa, (unsigned long long)limitVa);
        return 0;
    }
    uint64_t pos = (startVa + (kInsnAlign - 1)) & ~(kInsnAlign - 1);
    if (pos >= limitVa) {
        printf("[cave] find_cave: 对齐后起点 0x%llx 已越过 limitVa\n",
               (unsigned long long)pos);
        return 0;
    }
    // 最常见的调用错误：把 Mach-O 里的 vmaddr 直接当目标地址传进来（忘了加 ASLR 滑动）。
    // 这里只警告不中止 —— 万一是调用方故意取一段低地址，扫描本身仍然是安全的。
    if (base != 0 && pos < base) {
        printf("[cave] find_cave 警告: startVa=0x%llx 小于 base=0x%llx，"
               "很可能漏了滑动量（正确写法是 imageBase + (seg.vmaddr - __TEXT.vmaddr)）\n",
               (unsigned long long)pos, (unsigned long long)base);
    }
    if (sizeNeeded > (limitVa - pos)) {
        printf("[cave] find_cave: 区间只有 %llu 字节，装不下 %llu 字节的空洞\n",
               (unsigned long long)(limitVa - pos), (unsigned long long)sizeNeeded);
        return 0;
    }

    printf("[cave] 开始找洞: 区间 [0x%llx, 0x%llx) 需要 %llu 字节，每块读 %llu 字节\n",
           (unsigned long long)pos, (unsigned long long)limitVa,
           (unsigned long long)sizeNeeded, (unsigned long long)kScanChunk);

    static uint8_t buf[kScanChunk];
    uint64_t runStart = 0;   // 当前这段连续填充的起始地址
    uint64_t runLen   = 0;   // 当前这段连续填充的长度
    int      filler   = -1;  // -1 无；0 表示 0x00；1 表示 0xFF
    int      failChunks = 0;
    uint64_t scanned = 0;

    while (pos < limitVa) {
        size_t want = (size_t)(limitVa - pos);
        if (want > kScanChunk) want = kScanChunk;
        // 只处理完整的 4 字节单元，跨块残留不撕开一条指令。
        want &= ~(size_t)(kInsnAlign - 1);
        if (want == 0) break;

        if (!read_mem(task, pos, buf, want)) {
            // 这一块读不到：跳过，并且把当前 run 断掉（洞不能跨过不可读区）
            failChunks++;
            pos += want;
            scanned += want;
            runStart = 0;
            runLen = 0;
            filler = -1;
            continue;
        }

        for (size_t o = 0; o + 4 <= want; o += 4) {
            uint32_t w = 0;
            memcpy(&w, buf + o, 4);            // 用 memcpy 而不是强转，避开对齐与别名问题
            int cur = (w == 0x00000000u) ? 0 : ((w == 0xFFFFFFFFu) ? 1 : -1);
            if (cur >= 0 && cur == filler) {
                runLen += 4;                    // 同一填充值，接着数
            } else if (cur >= 0) {
                filler = cur;                   // 换了填充值或新开一段
                runStart = pos + o;
                runLen = 4;
            } else {
                filler = -1;                    // 撞到真实内容，run 中断
                runLen = 0;
                runStart = 0;
            }
            if (runLen >= sizeNeeded) {
                printf("[cave] 找到空洞: 0x%llx（镜像内偏移 0x%llx，填充 0x%02x，长度 >= %llu）\n",
                       (unsigned long long)runStart,
                       (unsigned long long)(runStart - base),
                       filler == 0 ? 0x00 : 0xFF, (unsigned long long)runLen);
                return runStart;
            }
        }
        pos += want;
        scanned += want;
    }

    printf("[cave] 扫了 %llu 字节（读失败 %d 块）仍未找到 >= %llu 字节的空洞\n",
           (unsigned long long)scanned, failChunks, (unsigned long long)sizeNeeded);
    return 0;
}

// ============== c. 打补丁：写字节 / 装分支 / 回滚 ==============
// saved 存原始字节，最多 16 字节 = 4 条 ARM64 指令（正好覆盖超范围跳板）。
struct Patch {
    uint64_t addr      = 0;   // 被改写的目标地址（一般是函数入口）
    uint8_t  saved[16] = {0}; // 原始字节，回滚用
    int      len       = 0;   // 实际改写长度：4（单条 B）或 16（超范围跳板）
    bool     installed = false; // 已安装；restore 成功后置回 false
};

// 复用一个 Patch 之前先清空，避免把上一次的原始字节又写回去（那会把代码写坏）。
inline void reset_patch(Patch* p) {
    if (!p) return;
    p->addr = 0;
    memset(p->saved, 0, sizeof(p->saved));
    p->len = 0;
    p->installed = false;
}

// 往目标进程写字节：临时加写权限 → 写 → 恢复权限 → 回读校验。
//
// 几个必须知道的点：
//   * 加权限时带 VM_PROT_COPY：目标页多半是 file-backed 的共享代码页，
//     不带 COPY 会直接改到共享映射上；带 COPY 会先做一次私有 COW 拷贝。
//   * 恢复权限这一步同时是 I-cache 的失效触发点：arm64 上把页重新设成可执行时，
//     内核会对该范围做指令缓存失效。若发现补丁"写了但没生效"，可以手工
//     vm_protect 成 RW 再 vm_protect 回 RX 一次，强制再刷一遍。
//   * restoreProt 默认 RX（这是改 __TEXT 代码页的场景）。若要往数据页写，
//     调用方要显式传该页原本的保护位，否则会把数据页改成可执行。
inline bool write_bytes(mach_port_t task, uint64_t addr, const void* buf, size_t len,
                        vm_prot_t restoreProt = VM_PROT_READ | VM_PROT_EXECUTE) {
    if (!task || !buf || len == 0) {
        printf("[cave] write_bytes 参数非法 (task=%u buf=%p len=%llu)\n", task, buf,
               (unsigned long long)len);
        return false;
    }
    if (len > kScanChunk) {
        printf("[cave] write_bytes: 单次写 %llu 字节过大（上限 %llu）\n",
               (unsigned long long)len, (unsigned long long)kScanChunk);
        return false;
    }

    // 页对齐：下取整到起点页，上取整到终点页。16KB 粒度对 4KB 页系统也安全。
    uint64_t pageLo = addr & ~(kPageAlign - 1);
    uint64_t pageHi = (addr + (uint64_t)len + (kPageAlign - 1)) & ~(kPageAlign - 1);
    size_t   pageLen = (size_t)(pageHi - pageLo);
    if (pageLen == 0 || pageHi <= pageLo) {
        printf("[cave] write_bytes: 页范围计算异常 lo=0x%llx hi=0x%llx\n",
               (unsigned long long)pageLo, (unsigned long long)pageHi);
        return false;
    }

    kern_return_t kr = vm_protect(task, (vm_address_t)pageLo, (vm_size_t)pageLen, false,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        printf("[cave] vm_protect(RW+COPY) @0x%llx len=0x%llx -> %d  (%s)\n",
               (unsigned long long)pageLo, (unsigned long long)pageLen, kr,
               kr == KERN_PROTECTION_FAILURE
                   ? "页不许写：已签名映射拒绝可写，一个字节都没写，进程是干净的"
                   : "页范围可能跨了未映射区，检查 addr 与 len");
        return false;
    }

    if (!write_mem_raw(task, addr, buf, len)) {
        printf("[cave] 写失败，正在把权限恢复成 0x%x\n", (unsigned)restoreProt);
        kern_return_t bk = vm_protect(task, (vm_address_t)pageLo, (vm_size_t)pageLen, false,
                                      restoreProt);
        printf("[cave] 权限恢复 -> %d\n", bk);
        return false;
    }

    kern_return_t kr2 = vm_protect(task, (vm_address_t)pageLo, (vm_size_t)pageLen, false,
                                   restoreProt);
    if (kr2 != KERN_SUCCESS) {
        printf("[cave] 权限恢复失败 @0x%llx -> %d（页现在仍是可写状态，务必记录）\n",
               (unsigned long long)pageLo, kr2);
    }

    // 回读校验：写"成功"不等于内容对，必须读回来比。
    if (len <= 64) {
        uint8_t back[64];
        memset(back, 0, sizeof(back));
        if (!read_mem(task, addr, back, len)) {
            printf("[cave] 写入后回读失败 @0x%llx\n", (unsigned long long)addr);
            return false;
        }
        if (memcmp(back, buf, len) != 0) {
            printf("[cave] 写入后回读不一致 @0x%llx（期望 %02x%02x%02x%02x 实得 %02x%02x%02x%02x）\n",
                   (unsigned long long)addr,
                   ((const uint8_t*)buf)[0], len > 1 ? ((const uint8_t*)buf)[1] : 0,
                   len > 2 ? ((const uint8_t*)buf)[2] : 0, len > 3 ? ((const uint8_t*)buf)[3] : 0,
                   back[0], len > 1 ? back[1] : 0, len > 2 ? back[2] : 0, len > 3 ? back[3] : 0);
            return false;
        }
    } else {
        printf("[cave] 写入 %llu 字节，超过 64 字节不做回读比对（需调用方自行验证）\n",
               (unsigned long long)len);
    }

    printf("[cave] 写入成功: 0x%llx 共 %llu 字节，权限已恢复 0x%x\n",
           (unsigned long long)addr, (unsigned long long)len, (unsigned)restoreProt);
    return true;
}

// ---------------------- ARM64 指令编码 ----------------------
// 全部按 ARM ARM 的位域手写，并给推导过程，不用来历不明的常量。
//
// B（无条件直接跳转，imm26）：
//   bits[31:26] = 0b000101          操作码
//   bits[25:0]  = imm26             目标地址 - 当前指令地址，单位是**指令**（字节偏移右移 2）
//   符号扩展 26 位 → 字节范围 [-2^27, 2^27-4] = [-128MB, +128MB-4]
static const int64_t kBranchFwdMax = ((int64_t)1 << 27) - 4;    // 0x07FFFFFC
static const int64_t kBranchBackMax = -((int64_t)1 << 27);      // -0x08000000

inline bool branch_in_range(int64_t disp) {
    if ((disp & 3) != 0) return false;                          // 偏移必须是 4 的倍数
    return disp >= kBranchBackMax && disp <= kBranchFwdMax;
}

inline uint32_t enc_b(int64_t disp) {
    // 右移 2 位后取低 26 位；负数先转无符号再右移，避免算术右移带进来的高位干扰。
    uint32_t imm26 = (uint32_t)(((uint64_t)disp >> 2) & 0x03FFFFFFULL);
    return ((uint32_t)0x05u << 26) | imm26;                     // 0x05<<26 = 0x14000000
}

// 反向解码（只给 selftest 用）：验证编码确实能解回原来的字节偏移。
inline int64_t b_disp_from(uint32_t insn) {
    int32_t v = (int32_t)(insn << 6);   // 左移 6 丢掉 opcode 高 6 位，imm26 落到最高位
    v >>= 6;                            // 算术右移 → 符号扩展出 26 位有符号数
    return (int64_t)v * 4;              // 单位从"指令"还原成"字节"
}

// ADRP（把目标页地址装进寄存器）：
//   bit31        = 1                              op
//   bits[30:29]  = immlo                         页偏移的低 2 位
//   bits[28:24]  = 0b10000                       操作码
//   bits[23:5]   = immhi                         页偏移的高 19 位
//   bits[4:0]    = Rd
//   共 21 位页偏移，符号扩展后范围 ±2^20 页 = ±4GB
inline int64_t adrp_page_delta(uint64_t pc, uint64_t target) {
    // 页基址 = 地址清掉低 12 位
    int64_t pcPage  = (int64_t)(pc & ~(uint64_t)0xFFF);
    int64_t tgtPage = (int64_t)(target & ~(uint64_t)0xFFF);
    int64_t deltaBytes = tgtPage - pcPage;
    return deltaBytes >> 12;                                    // 单位换成"页"
}

inline bool adrp_in_range(uint64_t pc, uint64_t target) {
    int64_t pages = adrp_page_delta(pc, target);
    return pages >= -((int64_t)1 << 20) && pages <= (((int64_t)1 << 20) - 1);
}

inline uint32_t enc_adrp(uint64_t pc, uint64_t target, uint32_t rd) {
    int64_t pages = adrp_page_delta(pc, target);
    uint32_t imm21 = (uint32_t)((uint64_t)pages & 0x1FFFFFULL);  // 取低 21 位（补码形式）
    uint32_t immlo = imm21 & 0x3u;                               // 低 2 位 → bits[30:29]
    uint32_t immhi = (imm21 >> 2) & 0x7FFFFu;                    // 高 19 位 → bits[23:5]
    return ((uint32_t)1u << 31)                 // bit31 = 1
         | (immlo << 29)                        // bits[30:29] = immlo
         | ((uint32_t)0x10u << 24)              // bits[28:24] = 0b10000
         | (immhi << 5)                         // bits[23:5]  = immhi
         | (rd & 0x1Fu);                        // bits[4:0]   = Rd
}

inline int64_t adrp_pages_from(uint32_t insn) {
    uint32_t immlo = (insn >> 29) & 0x3u;
    uint32_t immhi = (insn >> 5) & 0x7FFFFu;
    uint32_t imm21 = (immhi << 2) | immlo;      // 拼回 21 位（低 2 位来自 immlo）
    int32_t s = (int32_t)(imm21 << 11);         // 21 位顶到最高位
    s >>= 11;                                   // 算术右移 → 符号扩展
    return (int64_t)s;
}

// ADD（立即数，64 位，不移位）：
//   bit31 = 1 (sf=1 → 64 位), bit30 = 0 (op=ADD), bit29 = 0 (S=0，不设标志)
//   bits[28:24] = 0b10001
//   bits[23:22] = sh = 0b00（不左移）
//   bits[21:10] = imm12
//   bits[9:5] = Rn, bits[4:0] = Rd
inline uint32_t enc_add_imm(uint32_t rn, uint32_t rd, uint32_t imm12) {
    return ((uint32_t)1u << 31)
         | ((uint32_t)0x11u << 24)               // 0b10001 落在 bits[28:24]
         | (((uint32_t)0u) << 22)                // sh = 0
         | ((imm12 & 0xFFFu) << 10)
         | ((rn & 0x1Fu) << 5)
         | (rd & 0x1Fu);
}

// BR Xn（寄存器间接跳转，不认证 PAC）：
//   bits[31:25] = 0b1101011
//   bits[24:21] = 0b0000
//   bits[20:16] = 0b11111（固定）
//   bits[15:10] = 0b000000
//   bits[9:5]   = Rn
//   bits[4:0]   = 0b00000
inline uint32_t enc_br(uint32_t rn) {
    return ((uint32_t)0x6Bu << 25)               // 0b1101011
         | ((uint32_t)0x00u << 21)
         | ((uint32_t)0x1Fu << 16)
         | ((uint32_t)0x00u << 10)
         | ((rn & 0x1Fu) << 5)
         | ((uint32_t)0x00u);
}

// NOP = HINT #0：
//   bits[31:24] = 0b11010101, bits[23:20] = 0b0000, bits[19:16] = 0b0011,
//   bits[15:12] = 0b0010, bits[11:8] = CRm = 0, bits[7:5] = op2 = 0, bits[4:0] = 0b11111
inline uint32_t enc_nop() {
    return ((uint32_t)0xD5u << 24)
         | ((uint32_t)0x03u << 16)
         | ((uint32_t)0x02u << 12)
         | ((uint32_t)0x00u << 8)
         | ((uint32_t)0x00u << 5)
         | ((uint32_t)0x1Fu);
}

// 超范围时用的 16 字节序列：ADRP x16, page(to) ; ADD x16, x16, #off ; BR x16 ; NOP
//
// 为什么是 ADRP + ADD 而不是 LDR literal：
//   * LDR (literal) 只能寻址 ±1MB，对我们没用。
//   * ADRP + ADD 覆盖 ±4GB，一个进程内的任何目标都够，而且是位置无关的纯计算，
//     不依赖任何数据槽（也就不会踩坏别人的数据）。
//   * ARM64 的 PLT 本身就用 ADRP+ADD+BR 这套，行为最可预期。
//
// 为什么用 x16：
//   x18 是 iOS 保留的 TLS 指针，绝对不能碰；x16/x17 是 IP0/IP1，
//   AAPCS64 里就是给链接器做跳板用的临时寄存器。我们改的是**函数入口**，
//   此刻 x16 属于调用者保存寄存器范围，被覆盖是合法的（函数入口不承诺入口处
//   x16 有任何值）。这一点和"跳板要跳回原函数 +N"无关：我们根本不回来。
inline void enc_far_stub(uint64_t from, uint64_t to, uint32_t outWords[kStubWords]) {
    // ADRP 的 PC 基址是第一条指令的地址，也就是 from 本身。
    outWords[0] = enc_adrp(from, to, 16);
    // ADD 补上页内偏移：目标地址低 12 位
    outWords[1] = enc_add_imm(16, 16, (uint32_t)(to & 0xFFFu));
    outWords[2] = enc_br(16);
    outWords[3] = enc_nop();   // 占位，凑满 16 字节，让 4 条指令的边界干净
}

// 在 from 处装一条跳到 to 的分支。
//
// 两种形态：
//   1) 位移在 ±128MB 内 → 4 字节单条 B，只改写 1 条指令，最省事也最安全。
//   2) 超出范围 → 16 字节 ADRP/ADD/BR/NOP。
//      **代价**：from 处的 4 条原有指令被覆盖，而且跳板不回到 from+16，
//      所以这 4 条指令的效果全部丢失（它们既不会执行，也不会被重放）。
//      这适合"函数入口的几句前导无关紧要"的高频函数；如果前导里有
//      保存参数寄存器的栈操作，就必须改成"跳板里先重放被搬走的指令再跳回 from+16"，
//      那时候跳板本身放在 cave 里，入口只留一条 B（同样受 ±128MB 限制）。
//
// 原子性：
//   4 字节对齐的 4 字节存储在 arm64 上是原子的，单个 B 补丁不会让目标取到半条指令；
//   但 16 字节跳板**不是**原子的，4 条指令之间被取指就会执行到半成品。
//   走 16 字节方案时，调用方必须先 suspend 目标线程（或确保那个函数此刻没有线程在跑）。
inline bool install_branch(mach_port_t task, uint64_t from, uint64_t to, Patch* out) {
    if (!task) {
        printf("[cave] install_branch: task 无效\n");
        return false;
    }
    if (!out) {
        printf("[cave] install_branch: Patch 输出为空\n");
        return false;
    }
    if (out->installed) {
        printf("[cave] install_branch: 这个 Patch 已经是已安装状态 @0x%llx，"
               "继续装会丢掉原始字节、无法回滚\n", (unsigned long long)out->addr);
        return false;
    }
    if (from == to) {
        printf("[cave] install_branch: from == to == 0x%llx，跳到自己没有意义\n",
               (unsigned long long)from);
        return false;
    }
    if ((from & (kInsnAlign - 1)) != 0 || (to & (kInsnAlign - 1)) != 0) {
        printf("[cave] install_branch: 地址未 4 字节对齐 from=0x%llx to=0x%llx\n",
               (unsigned long long)from, (unsigned long long)to);
        return false;
    }

    int64_t disp = (int64_t)(to - from);   // 无符号相减再转有符号，得到真实的带符号距离

    uint32_t words[kStubWords];
    memset(words, 0, sizeof(words));
    int nWords = 0;
    bool far = false;

    if (branch_in_range(disp)) {
        words[0] = enc_b(disp);
        nWords = 1;
        printf("[cave] 用 4 字节 B 指令: from=0x%llx -> to=0x%llx disp=%lld(0x%llx) "
               "imm26=0x%x 编码=0x%08x\n",
               (unsigned long long)from, (unsigned long long)to, (long long)disp,
               (unsigned long long)disp, (unsigned)(words[0] & 0x03FFFFFFu), words[0]);
    } else {
        if (!adrp_in_range(from, to)) {
            printf("[cave] 目标超出 ADRP 的 ±4GB 窗口: from=0x%llx to=0x%llx，"
                   "需要先换一个更近的落点（例如把跳板放在同一段里）\n",
                   (unsigned long long)from, (unsigned long long)to);
            return false;
        }
        far = true;
        enc_far_stub(from, to, words);
        nWords = kStubWords;
        printf("[cave] 超出 ±128MB，改用 16 字节跳板: disp=%lld\n", (long long)disp);
        printf("[cave]   ADRP x16 = 0x%08x ; ADD x16,x16,#0x%x = 0x%08x\n",
               words[0], (unsigned)(to & 0xFFFu), words[1]);
        printf("[cave]   BR   x16 = 0x%08x ; NOP = 0x%08x\n", words[2], words[3]);
        // 跨页提醒：16 字节落在页尾时，第 2 页可能属于别的段或根本未映射，
        // 那么"整块写成功"就只覆盖了一半。
        // 判据用 16KB（write_bytes 的对齐粒度，也就是 vm_protect 实际作用的页），
        // 只跨 4KB 边界没关系：16KB 对齐后仍然落在同一页里。
        if ((from & (kPageAlign - 1)) > (kPageAlign - (uint64_t)kStubBytes)) {
            printf("[cave] 注意: 16 字节跳板跨过了 16KB 页边界（from 页内偏移 0x%llx），"
                   "确认相邻页也在 __TEXT 里且可写\n", (unsigned long long)(from & (kPageAlign - 1)));
        }
    }

    size_t patchLen = (size_t)nWords * 4;
    if (patchLen > sizeof(out->saved)) {
        printf("[cave] 补丁长度 %llu 超过 Patch.saved 容量 %llu\n",
               (unsigned long long)patchLen, (unsigned long long)sizeof(out->saved));
        return false;
    }

    // 先备份原始字节，再写。备份失败就绝不进入写入阶段。
    memset(out->saved, 0, sizeof(out->saved));
    if (!read_mem(task, from, out->saved, patchLen)) {
        printf("[cave] 备份原始字节失败 @0x%llx，放弃安装（没有改动任何字节）\n",
               (unsigned long long)from);
        return false;
    }
    out->addr = from;
    out->len = (int)patchLen;
    out->installed = false;

    printf("[cave] 原始字节: ");
    for (size_t i = 0; i < patchLen; i++) printf("%02x", out->saved[i]);
    printf("\n");

    if (!write_bytes(task, from, words, patchLen)) {
        // write_bytes 的失败分两种：vm_protect 被拒（一个字节都没写）、
        // 以及写成功但回读不一致（内容可能已经变了）。后者如果不回滚，
        // Patch.installed 是 false，restore() 会拒绝执行，只能重启 App 才能丢掉改动。
        // 所以这里做一次尽力回滚：页本来就不可写时这次回滚也会失败，但那时内容没变。
        printf("[cave] 写补丁失败 @0x%llx，做一次尽力回滚\n", (unsigned long long)from);
        if (write_bytes(task, from, out->saved, patchLen)) {
            printf("[cave] 已用备份把入口字节还原\n");
        } else {
            printf("[cave] 回滚尝试也失败；要靠重启 App 丢掉这个进程里的改动\n");
        }
        return false;
    }

    // 回读校验：必须看到我们写的那几条指令，否则不算装好。
    uint32_t back[kStubWords];
    memset(back, 0, sizeof(back));
    if (!read_mem(task, from, back, patchLen)) {
        printf("[cave] 装完回读失败 @0x%llx，尝试用备份回滚\n", (unsigned long long)from);
        write_bytes(task, from, out->saved, patchLen);
        return false;
    }
    if (memcmp(back, words, patchLen) != 0) {
        printf("[cave] 装完回读不一致 @0x%llx（期望 0x%08x 实得 0x%08x），回滚\n",
               (unsigned long long)from, words[0], back[0]);
        if (write_bytes(task, from, out->saved, patchLen)) {
            printf("[cave] 回滚完成\n");
        } else {
            printf("[cave] 回滚也失败了，目标入口现在是坏的，需要重新启动 App\n");
        }
        return false;
    }

    out->installed = true;
    printf("[cave] 补丁已安装: 0x%llx 处 %d 字节%s\n", (unsigned long long)out->addr,
           out->len, far ? "（16 字节跳板，4 条原有指令已失效）" : "（单条 B）");
    return true;
}

// 回滚：把原始字节写回去，并读回比对。失败必须如实报出来 ——
// 目标入口停在一个坏状态比"没装上"危险得多。
inline bool restore(mach_port_t task, Patch* p) {
    if (!task) {
        printf("[cave] restore: task 无效\n");
        return false;
    }
    if (!p) {
        printf("[cave] restore: Patch 为空\n");
        return false;
    }
    if (!p->installed) {
        printf("[cave] restore: 这个 Patch 没被安装过（addr=0x%llx installed=0）\n",
               (unsigned long long)p->addr);
        return false;
    }
    if (p->len <= 0 || (size_t)p->len > sizeof(p->saved)) {
        printf("[cave] restore: Patch.len=%d 非法（应在 1..%llu）\n", p->len,
               (unsigned long long)sizeof(p->saved));
        return false;
    }

    if (!write_bytes(task, p->addr, p->saved, (size_t)p->len)) {
        printf("[cave] restore: 写回原始字节失败 @0x%llx\n", (unsigned long long)p->addr);
        return false;
    }

    uint8_t back[16];
    memset(back, 0, sizeof(back));
    if (!read_mem(task, p->addr, back, (size_t)p->len)) {
        printf("[cave] restore: 回滚后回读失败 @0x%llx\n", (unsigned long long)p->addr);
        return false;
    }
    if (memcmp(back, p->saved, (size_t)p->len) != 0) {
        printf("[cave] restore: 回滚后内容与备份不一致 @0x%llx\n",
               (unsigned long long)p->addr);
        return false;
    }

    p->installed = false;
    printf("[cave] 已回滚 0x%llx 的 %d 字节，读回校验一致\n",
           (unsigned long long)p->addr, p->len);
    return true;
}

// ============== d. 纯计算自检（没有设备也能人工验算）==============
// 不碰任何进程，只算编码并反解回来。全部用例通过返回 true。
inline bool selftest_branch_encoding() {
    bool ok = true;
    printf("[cave] ==== selftest_branch_encoding（纯计算，不碰任何进程）====\n");

    // ---- 用例 1：同段内、落在 ±128MB 之内 ----
    {
        uint64_t from = 0x100004000ULL;
        uint64_t to   = 0x100010000ULL;
        int64_t  disp = (int64_t)(to - from);
        bool     in   = branch_in_range(disp);
        uint32_t insn = in ? enc_b(disp) : 0u;
        int64_t  back = in ? b_disp_from(insn) : 0;
        printf("[cave] 用例1 from=0x%llx to=0x%llx disp=0x%llx(%lld) in_range=%d\n",
               (unsigned long long)from, (unsigned long long)to,
               (unsigned long long)disp, (long long)disp, in);
        printf("[cave] 用例1 imm26=0x%x 编码=0x%08x（期望 0x14003000）反解 disp=0x%llx\n",
               (unsigned)((uint32_t)(((uint64_t)disp >> 2) & 0x03FFFFFFULL)), insn,
               (unsigned long long)back);
        if (!in) ok = false;
        if (insn != 0x14003000u) { printf("[cave] 用例1 编码不符预期\n"); ok = false; }
        if (back != disp) { printf("[cave] 用例1 反解不回到原偏移\n"); ok = false; }
    }

    // ---- 用例 2：超出 ±128MB → 必须走 16 字节跳板 ----
    {
        uint64_t from = 0x100004000ULL;
        uint64_t to   = 0x200000000ULL;
        int64_t  disp = (int64_t)(to - from);
        bool     in   = branch_in_range(disp);
        int64_t  pages = adrp_page_delta(from, to);
        bool     aok   = adrp_in_range(from, to);
        uint32_t w[kStubWords];
        enc_far_stub(from, to, w);
        printf("[cave] 用例2 from=0x%llx to=0x%llx disp=0x%llx in_range=%d\n",
               (unsigned long long)from, (unsigned long long)to,
               (unsigned long long)disp, in);
        printf("[cave] 用例2 页差=%lld(0x%llx) adrp_in_range=%d\n", (long long)pages,
               (unsigned long long)pages, aok);
        printf("[cave] 用例2 ADRP=0x%08x（期望 0x907ffff0）ADD=0x%08x（期望 0x91000210）"
               "BR=0x%08x（期望 0xd61f0200）NOP=0x%08x（期望 0xd503201f）\n",
               w[0], w[1], w[2], w[3]);
        printf("[cave] 用例2 反解 ADRP 页差=%lld（应等于上面页差）\n",
               (long long)adrp_pages_from(w[0]));
        if (in) { printf("[cave] 用例2 误判为在范围内\n"); ok = false; }
        if (!aok) { printf("[cave] 用例2 误判为超出 ADRP 窗口\n"); ok = false; }
        if (w[0] != 0x907FFFF0u) { printf("[cave] 用例2 ADRP 编码不符预期\n"); ok = false; }
        if (w[1] != 0x91000210u) { printf("[cave] 用例2 ADD 编码不符预期\n"); ok = false; }
        if (w[2] != 0xD61F0200u) { printf("[cave] 用例2 BR 编码不符预期\n"); ok = false; }
        if (w[3] != 0xD503201Fu) { printf("[cave] 用例2 NOP 编码不符预期\n"); ok = false; }
        if (adrp_pages_from(w[0]) != pages) { printf("[cave] 用例2 ADRP 反解不回去\n"); ok = false; }
    }

    // ---- 用例 3：±128MB 的四个边界 ----
    {
        struct Case { int64_t disp; bool expectIn; uint32_t expectInsn; } cases[4] = {
            // +128MB-4 是能表示的最大正偏移：imm26 = 0x01FFFFFF → B = 0x14000000|0x01FFFFFF
            { 0x07FFFFFCLL, true,  0x15FFFFFFu },
            // +128MB 正好超界（imm26 需要 0x2000000，放不进 26 位有符号正数）
            { 0x08000000LL, false, 0u },
            // -128MB 是能表示的最大负偏移：imm26 = 0x2000000
            { -0x08000000LL, true, 0x16000000u },
            // -128MB-4 超界
            { -0x08000004LL, false, 0u },
        };
        for (int i = 0; i < 4; i++) {
            bool in = branch_in_range(cases[i].disp);
            uint32_t insn = in ? enc_b(cases[i].disp) : 0u;
            int64_t back = in ? b_disp_from(insn) : 0;
            printf("[cave] 用例3.%d disp=0x%llx in_range=%d 编码=0x%08x 反解=%lld\n", i,
                   (unsigned long long)cases[i].disp, in, insn, (long long)back);
            if (in != cases[i].expectIn) {
                printf("[cave] 用例3.%d 范围判断不符预期\n", i);
                ok = false;
            }
            if (in && insn != cases[i].expectInsn) {
                printf("[cave] 用例3.%d 编码不符预期 0x%08x\n", i, cases[i].expectInsn);
                ok = false;
            }
            if (in && back != cases[i].disp) {
                printf("[cave] 用例3.%d 反解不回到原偏移\n", i);
                ok = false;
            }
        }
    }

    // ---- 用例 4：非 4 字节对齐的偏移必须被拒绝 ----
    {
        bool in = branch_in_range(0x1002);   // 0x1002 不是 4 的倍数
        printf("[cave] 用例4 disp=0x1002(非 4 对齐) in_range=%d（期望 0）\n", in);
        if (in) { printf("[cave] 用例4 未拒绝未对齐偏移\n"); ok = false; }
    }

    printf("[cave] ==== selftest 结果: %s ====\n", ok ? "全部通过" : "有不符项，见上");
    return ok;
}

} // namespace cave
