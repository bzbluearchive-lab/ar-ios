// ContainerCopy.h —— 把 payload 复制进目标 app 自己的数据容器
//
// 为什么必须这么做：
//   目标的沙盒只允许它读自己的容器。我们 app 的 bundle 路径
//   (/private/var/containers/Bundle/Application/<我们的UUID>/Gxx.app/ARPayload.dylib)
//   对它来说是不可读的 —— dlopen 会直接失败，而且这个失败和 library validation
//   的失败现象一样（都没有面包屑），很容易误判。
//
// 做法：
//   1. 遍历 /var/mobile/Containers/Data/Application/*
//   2. 读每个容器里的 .com.apple.mobile_container_manager.metadata.plist
//   3. 里面出现目标 bundle id 的那个目录就是目标的数据容器
//   4. 把 payload 拷进 <容器>/tmp/ ，chmod 0755
//
// 这样目标进程读自己容器里的文件，沙盒不拦。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

namespace containercopy {

static const char* kDataBase = "/var/mobile/Containers/Data/Application";
static const char* kMetaName = ".com.apple.mobile_container_manager.metadata.plist";

static inline bool file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    bool hit = memmem(buf, (size_t)sz, needle, strlen(needle)) != nullptr;
    free(buf);
    return hit;
}

// 找目标 bundle 的数据容器目录；找不到返回 false
static inline bool data_container_of(const char* bid, char* out, size_t cap) {
    if (!bid || !bid[0]) return false;
    DIR* d = opendir(kDataBase);
    if (!d) { printf("[container] 打不开 %s\n", kDataBase); return false; }
    struct dirent* e;
    int scanned = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char meta[1200];
        snprintf(meta, sizeof(meta), "%s/%s/%s", kDataBase, e->d_name, kMetaName);
        scanned++;
        if (!file_contains(meta, bid)) continue;
        snprintf(out, cap, "%s/%s", kDataBase, e->d_name);
        closedir(d);
        printf("[container] 扫描 %d 个容器，命中 %s\n", scanned, out);
        return true;
    }
    closedir(d);
    printf("[container] 扫描 %d 个容器，没有匹配 %s 的\n", scanned, bid);
    return false;
}

// 把 srcPath 复制到 <container>/tmp/ARPayload.dylib
static inline bool install_payload(const char* container, const char* srcPath,
                                   char* outPath, size_t outCap) {
    char tmpDir[1200];
    snprintf(tmpDir, sizeof(tmpDir), "%s/tmp", container);
    struct stat st;
    if (stat(tmpDir, &st) != 0) {
        if (mkdir(tmpDir, 0755) != 0) {
            printf("[container] 建不了 %s\n", tmpDir);
            return false;
        }
    }
    snprintf(outPath, outCap, "%s/ARPayload.dylib", tmpDir);

    FILE* in = fopen(srcPath, "rb");
    if (!in) { printf("[container] 打不开源文件 %s\n", srcPath); return false; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(in); return false; }

    FILE* out = fopen(outPath, "wb");
    if (!out) {
        printf("[container] 写不了 %s（沙盒？权限？）\n", outPath);
        fclose(in);
        return false;
    }
    uint8_t buf[64 * 1024];
    long left = sz;
    while (left > 0) {
        size_t want = (size_t)(left > (long)sizeof(buf) ? (long)sizeof(buf) : left);
        size_t got = fread(buf, 1, want, in);
        if (got == 0) break;
        if (fwrite(buf, 1, got, out) != got) break;
        left -= (long)got;
    }
    fclose(in);
    fclose(out);

    chmod(outPath, 0755);
    struct stat chk;
    if (stat(outPath, &chk) != 0 || chk.st_size != sz) {
        printf("[container] 复制后大小不符（%lld vs %ld）\n", (long long)chk.st_size, sz);
        return false;
    }
    printf("[container] 已复制 payload -> %s  (%ld 字节)\n", outPath, sz);
    return true;
}

} // namespace containercopy
