// AppInspect.h —— 在设备上直接分析另一个 app 的二进制
//
// 前提：调用方无沙盒（fs 可读任意路径）
//
// 回答的问题：那个商业外挂是靠什么机制浮在系统窗口之上的？
//   - 有 kfd / physpuppet / landa 之类字符串  → 内核读写
//   - 有 FBScene / FrontBoard / CAContext     → 场景托管
//   - 有 SpringBoard + Dobby/MSHookFunction   → 往 SpringBoard 注入
//   - entitlements 里有 frontboard / system-task-ports / cs.debugger → 权限清单
//
// entitlements 是直接从二进制内嵌的代码签名 blob 里解析的（CS_SuperBlob ->
// CSSLOT_ENTITLEMENTS），不依赖任何私有 API。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

namespace inspect {

// 代码签名 blob 全部是**大端序**，直接按小端读会得到 0xc00cdefa 这类错值
static inline uint32_t be32(const void* p) {
    uint32_t v = 0;
    memcpy(&v, p, 4);
    return ntohl(v);
}

// ---------------- Mach-O 结构（只取需要的字段） ----------------
struct mach_header_64_t {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command_t { uint32_t cmd, cmdsize; };
struct linkedit_data_command_t { uint32_t cmd, cmdsize; uint32_t dataoff, datasize; };
struct blob_index_t { uint32_t type, offset; };
struct superblob_t { uint32_t magic, length, count; };
struct blob_t { uint32_t magic, length; };

static const uint32_t kLCCodeSignature   = 0x1d;
static const uint32_t kCSSLOTEntitlements = 5;
static const uint32_t kCSMagicEmbedded    = 0xfade0cc0;
static const uint32_t kCSMagicEntitlement = 0xfade7171;

inline void print_entitlements(const uint8_t* base, size_t size) {
    if (size < sizeof(mach_header_64_t)) { printf("[ent] 文件太小\n"); return; }
    const mach_header_64_t* mh = (const mach_header_64_t*)base;
    if (mh->magic != 0xfeedfacf) { printf("[ent] 不是 arm64 Mach-O\n"); return; }

    const linkedit_data_command_t* sig = nullptr;
    const uint8_t* p = base + sizeof(mach_header_64_t);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command_t* lc = (const load_command_t*)p;
        if (lc->cmdsize == 0) break;
        if (lc->cmd == kLCCodeSignature) { sig = (const linkedit_data_command_t*)lc; break; }
        p += lc->cmdsize;
    }
    if (!sig) { printf("[ent] 没有 LC_CODE_SIGNATURE（可能是未签名二进制）\n"); return; }
    if ((size_t)sig->dataoff + sig->datasize > size) { printf("[ent] 签名段越界\n"); return; }

    const uint8_t* sigbase = base + sig->dataoff;
    const superblob_t* sb = (const superblob_t*)sigbase;
    if (be32(&sb->magic) != kCSMagicEmbedded) { printf("[ent] 签名 magic=0x%x 不是嵌入签名\n", be32(&sb->magic)); return; }
    printf("[ent] 代码签名 blob: %u 个子 blob\n", be32(&sb->count));

    const blob_index_t* idx = (const blob_index_t*)(sigbase + sizeof(superblob_t));
    for (uint32_t i = 0; i < be32(&sb->count); i++) {
        if (be32(&idx[i].type) != kCSSLOTEntitlements) continue;
        const blob_t* b = (const blob_t*)(sigbase + be32(&idx[i].offset));
        if (be32(&b->magic) != kCSMagicEntitlement) { printf("[ent] entitlements blob magic 异常\n"); return; }
        const char* xml = (const char*)b + sizeof(blob_t);
        size_t xlen = (size_t)be32(&b->length) - sizeof(blob_t);
        printf("[ent] ==== entitlements ====\n");
        fwrite(xml, 1, xlen, stdout);
        printf("\n[ent] =======================\n");

        // 顺手标出关键权限
        static const char* keys[] = {
            "platform-application", "com.apple.private.cs.debugger",
            "task_for_pid-allow", "com.apple.system-task-ports",
            "com.apple.private.security.no-sandbox",
            "com.apple.private.security.no-container",
            "com.apple.frontboard", "com.apple.springboard",
            "com.apple.backboard", "com.apple.QuartzCore",
            "com.apple.private.amfi", "dynamic-codesigning", "run-unsigned-code",
        };
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (memmem(xml, xlen, keys[k], strlen(keys[k])))
                printf("[ent] ★ 命中权限: %s\n", keys[k]);
        }
        return;
    }
    printf("[ent] 签名里没有 entitlements 项，列出所有子 blob 类型：\n");
    for (uint32_t i = 0; i < be32(&sb->count); i++) {
        printf("  slot type=0x%08x offset=%u\n", be32(&idx[i].type), be32(&idx[i].offset));
    }
}

// ---------------- 字符串扫描：判断它用的是什么机制 ----------------
inline void scan_mechanism_strings(const uint8_t* base, size_t size) {
    static const char* probes[] = {
        // 内核读写
        "kfd", "physpuppet", "landa", "smith", "kernrw", "libkernrw", "tfp0", "kread", "kwrite",
        // 场景托管
        "FBScene", "FrontBoard", "FBSOpen", "SBScene", "CAContext", "CARemoteLayer", "BackBoard",
        // 注入
        "SpringBoard", "Dobby", "MSHookFunction", "hook", "thread_create", "vm_write",
        "task_for_pid", "ptrace", "mach_vm",
        // 越狱生态
        "ellekit", "Substrate", "libhooker", "libjailbreak", "jailbreak",
        // 浮窗
        "UIWindow", "windowLevel", "SBS", "overlay",
    };
    printf("[scan] 机制关键词命中：\n");
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const char* k = probes[i];
        size_t kl = strlen(k);
        int hits = 0;
        for (size_t off = 0; off + kl <= size; off++) {
            if (base[off] == (uint8_t)k[0] && memcmp(base + off, k, kl) == 0) {
                if (++hits >= 3) break;   // 每词最多数 3 次，够判断了
            }
        }
        if (hits) printf("  %-24s x%d\n", k, hits);
    }
}

// ---------------- 读文件并分析 ----------------
inline bool analyze_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[inspect] 打不开 %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 200 * 1024 * 1024) { fclose(f); printf("[inspect] 大小异常 %ld\n", sz); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    printf("[inspect] 读入 %zu 字节: %s\n", got, path);
    print_entitlements(buf, got);
    scan_mechanism_strings(buf, got);
    free(buf);
    return true;
}

} // namespace inspect
