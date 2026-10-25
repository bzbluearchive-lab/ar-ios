// KernelCache.h —— 只读分析设备上的 kernelcache 文件
//
// 为什么重要：
//   "找到 task_for_pid 的检查点" 这件事本质上是对**内核二进制**做静态分析。
//   kernelcache 是磁盘上的文件，只要读得到，就不需要任何内核权限。
//   也就是说：偏移推导、特征码扫描、补丁字节计算 —— 全部可以零风险完成。
//   kfd 只用于最后"真正写入"那一步。
//
// iOS 16 起 kernelcache 可能被包在 IMG4/IM4P 容器里，所以要在文件里定位
// Mach-O magic (0xfeedfacf)，而不是假设文件开头就是 Mach-O。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

namespace kcache {

static const char* kCandidates[] = {
    "/System/Library/Caches/com.apple.kernelcaches/kernelcache",
    "/System/Library/Caches/com.apple.kernelcaches/kernelcache.release",
    "/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.kernelcaches/kernelcache",
    "/System/Volumes/Preboot/Cryptexes/OS/System/Library/Caches/com.apple.kernelcaches/kernelcache",
    "/private/var/db/com.apple.kernelcaches/kernelcache",
    nullptr
};

static const char* kDirs[] = {
    "/System/Library/Caches/com.apple.kernelcaches",
    "/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.kernelcaches",
    "/System/Volumes/Preboot/Cryptexes/OS/System/Library/Caches/com.apple.kernelcaches",
    nullptr
};

// 列出几个候选目录，看看设备上到底有什么
inline void list_dirs(void) {
    for (int i = 0; kDirs[i]; i++) {
        DIR* d = opendir(kDirs[i]);
        if (!d) { printf("[kcache] 打不开目录 %s\n", kDirs[i]); continue; }
        printf("[kcache] 目录 %s:\n", kDirs[i]);
        struct dirent* e;
        int n = 0;
        while ((e = readdir(d)) && n < 20) {
            if (e->d_name[0] == '.') continue;
            printf("    %s\n", e->d_name);
            n++;
        }
        if (n == 0) printf("    (空)\n");
        closedir(d);
    }
}

// 在文件里找 Mach-O magic，返回偏移；找不到返回 -1
inline long find_macho(const uint8_t* buf, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 0xcf && buf[i+1] == 0xfa && buf[i+2] == 0xed && buf[i+3] == 0xfe)
            return (long)i;   // 0xfeedfacf 小端存放
    }
    return -1;
}

// 解析一个候选路径：打印容器类型、Mach-O 位置、架构、段表
inline bool inspect_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[kcache] 打不开 %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); printf("[kcache] 空文件 %s\n", path); return false; }

    printf("[kcache] 可读: %s  (%.2f MB)\n", path, sz / 1048576.0);

    uint8_t head[16] = {0};
    fread(head, 1, 16, f);
    printf("[kcache]   头部: %02x %02x %02x %02x  (%.4s)\n",
           head[0], head[1], head[2], head[3], (const char*)head);
    if (memcmp(head, "IM4P", 4) == 0 || memcmp(head, "IMG4", 4) == 0)
        printf("[kcache]   容器: IMG4/IM4P（需要在内部定位 Mach-O）\n");

    // 前 8MB 里找 Mach-O
    size_t probe = (size_t)(sz < (long long)(8 * 1024 * 1024) ? sz : (long long)(8 * 1024 * 1024));
    uint8_t* buf = (uint8_t*)malloc(probe);
    if (!buf) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    size_t got = fread(buf, 1, probe, f);
    long mo = find_macho(buf, got);
    if (mo < 0) {
        printf("[kcache]   前 %.1f MB 里没找到 Mach-O magic\n", probe / 1048576.0);
        free(buf); fclose(f); return false;
    }
    printf("[kcache]   Mach-O 位于文件偏移 0x%lx\n", mo);

    if ((size_t)mo + 32 <= got) {
        const uint8_t* mh = buf + mo;
        uint32_t magic, cputype, cpusub, filetype, ncmds;
        memcpy(&magic, mh, 4);
        memcpy(&cputype, mh + 4, 4);
        memcpy(&cpusub, mh + 8, 4);
        memcpy(&filetype, mh + 12, 4);
        memcpy(&ncmds, mh + 16, 4);
        printf("[kcache]   cputype=0x%x cpusubtype=0x%x filetype=%u ncmds=%u\n",
               cputype, cpusub, filetype, ncmds);
        printf("[kcache]   (filetype 2=MH_EXECUTE, cpusubtype 2=arm64e)\n");
    }
    free(buf);
    fclose(f);
    return true;
}

// 入口：先列目录，再逐个试候选文件
inline void probe(void) {
    printf("\n=== kernelcache 只读探测 ===\n");
    list_dirs();
    bool any = false;
    for (int i = 0; kCandidates[i]; i++) {
        if (inspect_file(kCandidates[i])) any = true;
    }
    if (!any) {
        printf("[kcache] 所有候选路径都不可读。\n");
        printf("[kcache] 这说明该设备上 kernelcache 受保护，离线分析这条路走不通。\n");
    } else {
        printf("[kcache] 至少有一个 kernelcache 可读 —— 离线特征扫描可行，不需要 kfd。\n");
    }
    printf("=== 探测结束 ===\n");
}

} // namespace kcache
