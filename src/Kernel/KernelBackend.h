// KernelBackend.h —— 内核读写后端的抽象层
//
// 目的：把"谁来提供内核读写"这件事和我们的业务逻辑彻底解耦。
//   实现 A：空后端（现在默认）——所有能力返回不可用，绝不误伤
//   实现 B：开源 kfd（自己编）
//   实现 C：买回来的方案（源码或二进制）
//
// 任何后端接入后，上层（Patch / Inject）只按接口调用，
// 这样换供应商、换版本、退回安全模式都只改一处。
//
// 铁律：宁可不可用，也不猜。
//   init 失败 → available() == false → 上层立刻停止，不做任何内核操作。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace kback {

struct Ops {
    const char* name = "none";

    // 返回 true 表示后端可用
    bool (*init)(void) = nullptr;

    // 内核读：返回 true 表示读成功且长度匹配
    bool (*kread)(uint64_t addr, void* out, size_t len) = nullptr;

    // 内核写：必须自己保证"写前备份、写后回读"由上层做，这里只负责写
    bool (*kwrite)(uint64_t addr, const void* in, size_t len) = nullptr;

    // 取得内核基址（kbase）；0 表示不可用
    uint64_t (*kbase)(void) = nullptr;

    // 给 pid 拿到可读写的 task port（部分后端会绕过 task_for_pid 的限制）
    // 返回 0 表示拿不到
    mach_port_t (*task_for_pid_bypass)(pid_t pid) = nullptr;
};

// ---------------- 空后端：默认，永远不可用 ----------------
inline bool null_init(void) { return false; }
inline bool null_kread(uint64_t, void*, size_t) { return false; }
inline bool null_kwrite(uint64_t, const void*, size_t) { return false; }
inline uint64_t null_kbase(void) { return 0; }
inline mach_port_t null_tfp(pid_t) { return MACH_PORT_NULL; }

inline Ops& null_ops(void) {
    static Ops o = { "none", null_init, null_kread, null_kwrite, null_kbase, null_tfp };
    return o;
}

// ---------------- 注册与查询 ----------------
inline Ops& current(void) {
    static Ops* g = nullptr;
    if (!g) g = &null_ops();
    return *g;
}

inline void set_backend(const Ops& ops) { current() = ops; }

inline bool available(void) {
    Ops& o = current();
    return o.init && o.kread && o.kwrite && o.kbase;
}

// ---------------- 带校验的读 ----------------
// 上层统一走这两个包装：读失败永远返回 false，绝不返回垃圾数据
inline bool read(uint64_t addr, void* out, size_t len) {
    if (!available() || !addr || !out || !len) return false;
    return current().kread(addr, out, len);
}

inline uint32_t read32(uint64_t addr, bool* ok = nullptr) {
    uint32_t v = 0;
    bool r = read(addr, &v, sizeof(v));
    if (ok) *ok = r;
    return r ? v : 0;
}

inline uint64_t read64(uint64_t addr, bool* ok = nullptr) {
    uint64_t v = 0;
    bool r = read(addr, &v, sizeof(v));
    if (ok) *ok = r;
    return r ? v : 0;
}

// ---------------- 带"读回校验"的写 ----------------
// 唯一允许调用的写入口：先读原值 → 写 → 再读回比对
// 比对失败立即把原值写回去，并返回 false
inline bool write_verified(uint64_t addr, const void* in, size_t len) {
    if (!available() || !addr || !in || !len || len > 64) return false;
    uint8_t orig[64];
    if (!current().kread(addr, orig, len)) return false;

    printf("[kwrite] addr=0x%llx len=%zu\n", (unsigned long long)addr, len);
    printf("[kwrite]   orig: ");
    for (size_t i = 0; i < len; i++) printf("%02x ", orig[i]);
    printf("\n");
    printf("[kwrite]   new : ");
    for (size_t i = 0; i < len; i++) printf("%02x ", ((const uint8_t*)in)[i]);
    printf("\n");

    if (!current().kwrite(addr, in, len)) {
        printf("[kwrite] 写入调用失败\n");
        return false;
    }

    uint8_t back[64];
    if (!current().kread(addr, back, len) || memcmp(back, in, len) != 0) {
        printf("[kwrite] 回读不符，立即回滚\n");
        current().kwrite(addr, orig, len);
        return false;
    }
    printf("[kwrite] 回读校验通过\n");
    return true;
}

// ---------------- 状态诊断 ----------------
inline void dump_status(void) {
    printf("[kback] 后端=%s 可用=%s\n", current().name, available() ? "是" : "否");
    if (!available()) {
        printf("[kback] 未接入内核后端：所有内核操作都会静默失败（这是安全的默认值）\n");
    }
}

} // namespace kback
