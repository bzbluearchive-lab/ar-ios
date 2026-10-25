// AppInspect.h —— 在设备上分析另一个 app 的二进制
//
// 针对大二进制（游戏本体常 300MB+）：
//   - entitlements 只读文件头 + load commands + 签名段，不碰其余部分
//   - 字符串扫描按 4MB 分块读，块间留 64 字节重叠，避免关键词跨块漏检
//
// 代码签名 blob 是**大端序**（Apple 的规定），不是小端。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

namespace inspect {

static inline uint32_t be32(const void* p) {
    uint32_t v = 0;
    memcpy(&v, p, 4);
    return ntohl(v);
}

static const uint32_t kLCCodeSignature    = 0x1d;
static const uint32_t kCSSLOTEntitlements = 5;
static const uint32_t kCSMagicEmbedded    = 0xfade0cc0;
static const uint32_t kCSMagicEntitlement = 0xfade7171;

// ---------------- 1. entitlements：只读头部与签名段 ----------------
inline void print_entitlements(FILE* f, long long fileSize) {
    uint8_t hdr[32];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 32, f) != 32) { printf("[ent] 读头失败\n"); return; }
    uint32_t magic;
    memcpy(&magic, hdr, 4);
    if (magic != 0xfeedfacf) { printf("[ent] 不是 arm64 Mach-O (magic=0x%x)\n", magic); return; }
    uint32_t ncmds;
    memcpy(&ncmds, hdr + 16, 4);
    printf("[ent] Mach-O 头 OK, ncmds=%u, 文件 %.1f MB\n", ncmds, fileSize / 1048576.0);

    long long off = 32;
    uint32_t sigOff = 0, sigSize = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint8_t lc[16];
        if (fseek(f, (long)off, SEEK_SET) != 0) break;
        if (fread(lc, 1, 16, f) != 16) break;
        uint32_t cmd, cmdsize;
        memcpy(&cmd, lc, 4);
        memcpy(&cmdsize, lc + 4, 4);
        if (cmdsize < 8) break;
        if (cmd == kLCCodeSignature) {
            memcpy(&sigOff, lc + 8, 4);
            memcpy(&sigSize, lc + 12, 4);
            break;
        }
        off += cmdsize;
    }
    if (!sigOff || !sigSize) { printf("[ent] 没有 LC_CODE_SIGNATURE\n"); return; }
    if (sigSize > 1024 * 1024) { printf("[ent] 签名段过大 %u\n", sigSize); return; }

    uint8_t* sig = (uint8_t*)malloc(sigSize);
    if (!sig) { printf("[ent] 内存不足\n"); return; }
    if (fseek(f, (long)sigOff, SEEK_SET) != 0 || fread(sig, 1, sigSize, f) != sigSize) {
        printf("[ent] 读签名段失败\n"); free(sig); return;
    }

    const uint32_t sbMagic = be32(sig);
    const uint32_t sbCount = be32(sig + 8);
    if (sbMagic != kCSMagicEmbedded) {
        printf("[ent] 签名 magic=0x%08x 不是嵌入签名\n", sbMagic); free(sig); return;
    }
    printf("[ent] 嵌入签名，%u 个子 blob\n", sbCount);

    bool printed = false;
    for (uint32_t i = 0; i < sbCount && !printed; i++) {
        const uint32_t type   = be32(sig + 12 + i * 8);
        const uint32_t offset = be32(sig + 16 + i * 8);
        printf("  slot[%u] type=0x%08x offset=%u\n", i, type, offset);
        if (type != kCSSLOTEntitlements) continue;
        if (offset + 8 > sigSize) continue;
        const uint32_t bMagic  = be32(sig + offset);
        const uint32_t bLength = be32(sig + offset + 4);
        if (bMagic != kCSMagicEntitlement) { printf("[ent] 子 blob magic 异常\n"); continue; }
        if (offset + bLength > sigSize) continue;
        printf("[ent] ==== entitlements ====\n");
        fwrite(sig + offset + 8, 1, bLength - 8, stdout);
        printf("\n[ent] =======================\n");

        static const char* keys[] = {
            "platform-application", "com.apple.private.cs.debugger", "task_for_pid-allow",
            "com.apple.system-task-ports", "no-sandbox", "no-container",
            "frontboard", "springboard", "backboard", "dynamic-codesigning", "run-unsigned-code",
            "com.apple.private.amfi",
        };
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (memmem(sig + offset, bLength, keys[k], strlen(keys[k])))
                printf("[ent] * 命中权限: %s\n", keys[k]);
        }
        printed = true;
    }
    if (!printed) printf("[ent] 没找到 entitlements 子 blob\n");
    free(sig);
}

// ---------------- 2. 字符串扫描：分块 ----------------
inline void scan_strings(FILE* f, long long fileSize) {
    static const char* probes[] = {
        // 游戏身份
        "uamo", "ArenaBreakout", "Arena", "Breakout",
        "DeltaForce", "dfm", "tmgp", "proximabeta", "tencent",
        // 引擎
        "GWorld", "GNames", "FNamePool", "UWorld", "UnrealEngine", "il2cpp",
        // 反作弊（ACE / anogs）
        "anogs", "AnoSDK", "ACESDK", "libACE", "anti-cheat", "AntiCheat",
        // 内核读写
        "kfd", "physpuppet", "landa", "kernrw", "tfp0",
        // 注入 / 浮窗
        "task_for_pid", "thread_create", "mach_vm", "ptrace",
        "UIWindow", "windowLevel", "SBS", "overlay", "BackBoard", "SpringBoard",
        "Dobby", "MSHookFunction", "ellekit", "Substrate",
    };
    const int nProbes = (int)(sizeof(probes) / sizeof(probes[0]));
    int hits[64];
    for (int i = 0; i < 64; i++) hits[i] = 0;

    const size_t CHUNK = 4 * 1024 * 1024;
    const size_t OVERLAP = 64;
    uint8_t* buf = (uint8_t*)malloc(CHUNK + OVERLAP);
    if (!buf) { printf("[scan] 内存不足\n"); return; }
    memset(buf, 0, CHUNK + OVERLAP);

    long long done = 0;
    size_t carry = 0;
    while (done < fileSize) {
        if (fseek(f, (long)done, SEEK_SET) != 0) break;
        size_t want = CHUNK;
        if ((long long)want > fileSize - done) want = (size_t)(fileSize - done);
        size_t got = fread(buf + carry, 1, want, f);
        if (got == 0) break;
        size_t total = carry + got;

        for (int i = 0; i < nProbes && i < 64; i++) {
            const char* k = probes[i];
            size_t kl = strlen(k);
            if (kl == 0 || total < kl) continue;
            const uint8_t* p = buf;
            size_t remain = total;
            while (remain >= kl) {
                const uint8_t* found = (const uint8_t*)memmem(p, remain, k, kl);
                if (!found) break;
                if (hits[i] < 999) hits[i]++;
                size_t consumed = (size_t)(found - p) + kl;
                p += consumed;
                remain -= consumed;
            }
        }

        carry = (total >= OVERLAP) ? OVERLAP : total;
        memmove(buf, buf + total - carry, carry);

        done += got;
        printf("[scan] 进度 %.0f%%\n", 100.0 * done / (double)fileSize);
    }
    free(buf);

    printf("[scan] === 关键词命中 ===\n");
    for (int i = 0; i < nProbes && i < 64; i++)
        if (hits[i]) printf("  %-16s x%d\n", probes[i], hits[i]);
}

// ---------------- 3. 入口 ----------------
inline bool analyze_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[inspect] 打不开 %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    if (sz <= 0) { fclose(f); printf("[inspect] 空文件\n"); return false; }
    printf("[inspect] 目标 %.1f MB\n", sz / 1048576.0);

    print_entitlements(f, sz);
    scan_strings(f, sz);
    fclose(f);
    return true;
}

} // namespace inspect
