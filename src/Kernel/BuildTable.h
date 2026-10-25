// BuildTable.h —— 按 iOS 构建号索引的偏移表
//
// 为什么按 build 而不是按 iOS 版本号：
//   内核结构体偏移、补丁点地址由 XNU 的具体构建决定。
//   16.1.1 在不同设备上是同一个 build（20B101），所以同 build 可以直接复用；
//   16.1 与 16.1.1 是不同 build，不能混用。
//
// 铁律：查不到就停。
//   表里没有当前 build → 一切内核操作都不执行。
//   绝不"猜一个相近版本试试"——猜错的代价是用户的设备。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>

namespace buildtable {

struct Entry {
    const char* build;          // 例如 "20B101"
    const char* productVersion; // 例如 "16.1.1"，仅用于日志
    const char* arch;           // "arm64e"

    // 内核侧参数，全部由实测得出；填 0 表示"未知，尚未测量"
    const char* kfdBranch;      // "physpuppet" / "landa" / "smith" / ...
    uint64_t    proc_off;       // proc 结构体相对偏移（预留）
    uint64_t    task_off;       // task 结构体相对偏移（预留）
    uint64_t    patch_site;     // task_for_pid 检查点的内核地址（相对 kbase）
    uint8_t     patch_orig[8];  // 原始字节（实测得到）
    uint8_t     patch_new[8];   // 补丁字节
    int         patch_len;      // 补丁长度（0 表示未知）
};

// 目前只有一条占位记录：全部为"未知"。
// 影子机实测后，把真实数值填进来即可，不需要改任何调用方。
inline const Entry kTable[] = {
    {
        "20B101", "16.1.1", "arm64e",
        /*kfdBranch*/ nullptr,
        /*proc_off*/ 0, /*task_off*/ 0,
        /*patch_site*/ 0,
        /*patch_orig*/ {0}, /*patch_new*/ {0},
        /*patch_len*/ 0
    },
    { nullptr, nullptr, nullptr, nullptr, 0, 0, 0, {0}, {0}, 0 }
};

// 读当前系统的 build 号（kern.osversion，例如 "20B101"）
inline const char* current_build(void) {
    static char buf[64];
    if (buf[0]) return buf;
    size_t sz = sizeof(buf) - 1;
    if (sysctlbyname("kern.osversion", buf, &sz, nullptr, 0) != 0) {
        buf[0] = 0;
    }
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

// 读产品版本号（例如 "16.1.1"）
inline const char* product_version(void) {
    static char buf[64];
    if (buf[0]) return buf;
    size_t sz = sizeof(buf) - 1;
    if (sysctlbyname("kern.osproductversion", buf, &sz, nullptr, 0) != 0) buf[0] = 0;
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

inline const char* current_arch(void) {
#if defined(__arm64e__)
    return "arm64e";
#elif defined(__arm64__)
    return "arm64";
#else
    return "other";
#endif
}

// 查表；查不到返回 nullptr（调用方必须据此停止）
inline const Entry* lookup(const char* build) {
    if (!build || !build[0]) return nullptr;
    for (int i = 0; kTable[i].build; i++) {
        if (strcmp(kTable[i].build, build) == 0) return &kTable[i];
    }
    return nullptr;
}

// 打一行状态：当前 build / 架构 / 表里有没有
inline void dump_status(void) {
    const char* b = current_build();
    const char* pv = product_version();
    const char* ar = current_arch();
    const Entry* e = lookup(b);
    printf("[build] 系统 build=%s 版本=%s 架构=%s\n", b[0] ? b : "?", pv, ar);
    if (!e) {
        printf("[build] 偏移表未收录此 build —— 所有内核操作必须停止（fail-closed）\n");
        return;
    }
    printf("[build] 表内命中: %s / %s / %s\n", e->build, e->productVersion, e->arch);
    if (!e->kfdBranch || e->patch_len == 0) {
        printf("[build] 该记录尚未实测（kfdBranch/patch 为空）—— 仍需在影子机上补数据\n");
    } else {
        printf("[build] kfd 分支=%s patch_site=0x%llx len=%d\n",
               e->kfdBranch, (unsigned long long)e->patch_site, e->patch_len);
    }
}

} // namespace buildtable
