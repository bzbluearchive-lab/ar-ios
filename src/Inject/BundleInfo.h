// BundleInfo.h —— 从运行中的进程反推出它属于哪个 app，并读出 bundle id
//
// 用途：进程名可能和 app 显示名完全无关（比如二进制叫 DeltaForceClient，
// 但显示名是别的）。唯一可靠的定性依据是 app bundle 里的 Info.plist 中的
// CFBundleIdentifier。
//
// 注意：Info.plist 常是 **二进制 plist**，普通文本解析不可靠。
// 但二进制 plist 里的字符串是以 UTF-8 原样存放的，所以扫描可打印串、
// 挑出形如 com.xxx.yyy 的候选就够了。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RemoteLoad.h"

namespace bundleinfo {

static inline bool idchar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

// 定位 "com." 后向两侧扩展到非 id 字符为止 —— XML 与二进制 plist 都适用
static inline int extract_ids(const uint8_t* data, size_t n, char out[][96], int maxOut) {
    int found = 0;
    for (size_t i = 0; i + 4 <= n && found < maxOut; i++) {
        if (!(data[i] == 'c' && data[i+1] == 'o' && data[i+2] == 'm' && data[i+3] == '.')) continue;
        // 向左扩展
        size_t start = i;
        while (start > 0 && idchar(data[start - 1]) && i - start < 64) start--;
        // 向右扩展
        size_t end = i + 4;
        while (end < n && idchar(data[end]) && end - start < 90) end++;
        size_t len = end - start;
        if (len < 8 || len > 90) continue;

        char tmp[96];
        snprintf(tmp, sizeof(tmp), "%.*s", (int)len, (const char*)(data + start));
        // 至少要有一个点在 com 之后
        if (!strchr(tmp + 4, '.')) continue;

        bool dup = false;
        for (int k = 0; k < found; k++) if (strcmp(out[k], tmp) == 0) { dup = true; break; }
        if (!dup) { snprintf(out[found], 96, "%s", tmp); found++; }
        i = end;
    }
    return found;
}

// UTF-16（字符间夹 0x00）版本
static inline int extract_ids_utf16(const uint8_t* data, size_t n, char out[][96], int maxOut) {
    int found = 0;
    for (size_t i = 0; i + 9 < n && found < maxOut; i++) {
        if (!(data[i] == 'c' && data[i+1] == 0 && data[i+2] == 'o' && data[i+3] == 0 &&
              data[i+4] == 'm' && data[i+5] == 0 && data[i+6] == '.' && data[i+7] == 0)) continue;
        char tmp[96]; int k = 0;
        size_t j = i;
        while (j + 1 < n && k < 90 && data[j+1] == 0 && idchar(data[j])) { tmp[k++] = (char)data[j]; j += 2; }
        tmp[k] = 0;
        if (k < 8) continue;
        bool dup = false;
        for (int m = 0; m < found; m++) if (strcmp(out[m], tmp) == 0) { dup = true; break; }
        if (!dup) { snprintf(out[found], 96, "%s", tmp); found++; }
        i = j;
    }
    return found;
}
// 从可执行文件路径推导出 .app 根目录
static inline bool app_root_from_exec(const char* execPath, char* out, size_t cap) {
    if (!execPath || !execPath[0]) return false;
    snprintf(out, cap, "%s", execPath);
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    *slash = 0;                                  // 去掉 /<binary>
    if (!strstr(out, ".app") && !strstr(out, ".appex")) return false;
    // 若是 .appex，继续往上找到外层 .app
    char* appex = strstr(out, ".appex");
    if (appex) {
        char* p = strrchr(out, '/');
        if (p) *p = 0;
    }
    return true;
}

// 读某个 app 的 Info.plist，打印 bundle id 候选
static inline void print_bundle_ids(const char* execPath) {
    char root[1024];
    if (!app_root_from_exec(execPath, root, sizeof(root))) {
        printf("    (无法推导 app 根目录)\n");
        return;
    }
    char plist[1200];
    snprintf(plist, sizeof(plist), "%s/Info.plist", root);

    FILE* f = fopen(plist, "rb");
    if (!f) { printf("    Info.plist 打不开: %s\n", plist); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); printf("    Info.plist 大小异常\n"); return; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    char ids[8][96];
    int n = extract_ids(buf, (size_t)sz, ids, 8);
    if (n == 0) n = extract_ids_utf16(buf, (size_t)sz, ids, 8);
    printf("    Info.plist: %s\n", plist);
    if (n == 0) printf("    (没找到 bundle id 候选)\n");
    for (int i = 0; i < n; i++) printf("    bundle候选: %s\n", ids[i]);
    free(buf);
}

// 列出所有路径可读的进程 + 它们各自的 bundle id
static inline void dump_all_bundles(int maxPid, int maxPrint) {
    int shown = 0;
    for (pid_t p = 1; p <= maxPid && shown < maxPrint; p++) {
        mach_port_t t = MACH_PORT_NULL;
        if (task_for_pid(mach_task_self(), p, &t) != KERN_SUCCESS) continue;
        char path[1024];
        bool ok = rload::read_remote_path(t, path, sizeof(path));
        mach_port_deallocate(mach_task_self(), t);
        if (!ok || !path[0]) continue;
        if (!getenv("AR_ALL") && !strstr(path, "containers/Bundle/Application")) continue;
        printf("[app] pid=%-5d %s\n", p, path);
        print_bundle_ids(path);
        shown++;
    }
    printf("[app] 共列出 %d 个\n", shown);
}

} // namespace bundleinfo
