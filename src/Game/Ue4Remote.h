// Ue4Remote.h —— 跨进程读取 UE4 世界所需的最小工具集
//
// 全部操作都通过 task port 完成（vm_read_overwrite），不解引用目标地址。
// iOS SDK 禁用了 <mach/mach_vm.h>，所以一律用 vm_* 系列。
//
// 这里不内置任何具体游戏的偏移 —— 偏移必须由调用方传入，
// 因为不同游戏、不同版本全都不一样，硬编码等于编造。

#pragma once
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld_images.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace ue4r {

// ---------------- 跨进程读 ----------------
inline bool read_mem(mach_port_t task, uint64_t addr, void* out, size_t len) {
    if (!addr || !out || !len) return false;
    vm_size_t got = 0;
    kern_return_t kr = vm_read_overwrite(task, (vm_address_t)addr,
                                         (vm_size_t)len, (vm_address_t)out, &got);
    return kr == KERN_SUCCESS && got == (vm_size_t)len;
}

inline uint64_t rd64(mach_port_t task, uint64_t addr) {
    uint64_t v = 0;
    return read_mem(task, addr, &v, 8) ? v : 0;
}

inline uint32_t rd32(mach_port_t task, uint64_t addr) {
    uint32_t v = 0;
    return read_mem(task, addr, &v, 4) ? v : 0;
}

// ---------------- 主镜像基址与路径 ----------------
// dyld_all_image_infos 布局：+0 version(u32) +4 infoArrayCount(u32) +8 infoArray(ptr)
// dyld_image_info：+0 imageLoadAddress +8 imageFilePath +16 imageFileModDate
inline uint64_t main_image_base(mach_port_t task, char* pathOut = nullptr, size_t cap = 0) {
    if (pathOut && cap) pathOut[0] = 0;

    task_dyld_info_data_t di;
    mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
    if (task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &cnt) != KERN_SUCCESS) {
        printf("[ue4r] task_info(TASK_DYLD_INFO) 失败\n");
        return 0;
    }
    uint64_t all = di.all_image_info_addr;
    if (!all) { printf("[ue4r] all_image_info_addr 为 0（dyld 还没初始化）\n"); return 0; }

    uint32_t n = 0;
    if (!read_mem(task, all + 4, &n, 4)) return 0;
    uint64_t arr = 0;
    if (!read_mem(task, all + 8, &arr, 8)) return 0;
    if (!n || !arr) { printf("[ue4r] 镜像表为空\n"); return 0; }

    uint64_t loadAddr = 0, pathPtr = 0;
    if (!read_mem(task, arr, &loadAddr, 8)) return 0;
    if (!read_mem(task, arr + 8, &pathPtr, 8)) return 0;

    if (pathOut && cap && pathPtr) {
        memset(pathOut, 0, cap);
        read_mem(task, pathPtr, pathOut, cap - 1);
    }
    printf("[ue4r] 镜像数=%u 主镜像基址=0x%llx 路径=%s\n",
           n, (unsigned long long)loadAddr, (pathOut && pathOut[0]) ? pathOut : "(未取到)");
    return loadAddr;
}

// ---------------- AOB 特征扫描 ----------------
// mask 里 'x' 必须匹配、'?' 通配。分块读取，块间重叠 pattern 长度-1，避免跨块漏检。
inline uint64_t scan_pattern(mach_port_t task, uint64_t start, size_t size,
                             const uint8_t* sig, const char* mask) {
    if (!task || !start || !sig || !mask) return 0;
    size_t plen = strlen(mask);
    if (plen == 0 || plen > 256) return 0;

    const size_t CHUNK = 1 << 20;              // 1MB
    const size_t OVERLAP = plen ? plen - 1 : 0;
    uint8_t* buf = (uint8_t*)malloc(CHUNK + OVERLAP);
    if (!buf) { printf("[ue4r] 扫描缓冲分配失败\n"); return 0; }
    memset(buf, 0, CHUNK + OVERLAP);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t found = 0;
    size_t done = 0, carry = 0;

    while (done < size) {
        size_t want = (size - done > CHUNK) ? CHUNK : (size - done);
        if (!read_mem(task, start + done, buf + carry, want)) {
            // 单块读失败不致命（可能跨了不可读区间），跳过这块
            done += want;
            carry = 0;
            continue;
        }
        size_t total = carry + want;

        if (total >= plen) {
            size_t limit = total - plen;
            for (size_t i = 0; i <= limit; i++) {
                bool hit = true;
                for (size_t j = 0; j < plen; j++) {
                    if (mask[j] == 'x' && buf[i + j] != sig[j]) { hit = false; break; }
                }
                if (hit) { found = start + done - carry + i; break; }
            }
        }
        if (found) break;

        carry = (total >= OVERLAP) ? OVERLAP : total;
        if (carry) memmove(buf, buf + total - carry, carry);
        done += want;
    }

    free(buf);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("[ue4r] AOB 扫描 %.1f MB，耗时 %.0f ms，命中=0x%llx\n",
           size / 1048576.0, ms, (unsigned long long)found);
    return found;
}

// ---------------- ADRP + LDR 解析（定位 UE4 全局变量常用手法） ----------------
// ADRP Xd, page    ; 0x9F000000 掩码 == 0x90000000
// LDR  Xd, [Xn,#off]; 0xFFC00000 掩码 == 0xF9400000（64 位无符号偏移）
inline uint64_t adrp_page(uint64_t pc, uint32_t insn) {
    uint64_t immlo = (insn >> 29) & 0x3;
    uint64_t immhi = (insn >> 5) & 0x7FFFF;
    int64_t imm = (int64_t)((immhi << 2) | immlo);
    if (imm & (1LL << 20)) imm -= (1LL << 21);       // 21 位符号扩展
    return (pc & ~0xFFFULL) + ((uint64_t)imm << 12);
}

inline uint64_t adrp_ldr_global(mach_port_t task, uint64_t insnAddr) {
    uint32_t i0 = rd32(task, insnAddr);
    uint32_t i1 = rd32(task, insnAddr + 4);
    if ((i0 & 0x9F000000) != 0x90000000) {
        printf("[ue4r] 0x%llx 不是 ADRP (0x%08x)\n", (unsigned long long)insnAddr, i0);
        return 0;
    }
    uint64_t page = adrp_page(insnAddr, i0);
    if ((i1 & 0xFFC00000) != 0xF9400000) {
        printf("[ue4r] 0x%llx 不是 LDR(64) (0x%08x)\n", (unsigned long long)(insnAddr + 4), i1);
        return 0;
    }
    uint32_t imm12 = (i1 >> 10) & 0xFFF;
    uint64_t off = (uint64_t)imm12 * 8;
    printf("[ue4r] ADRP page=0x%llx LDR off=0x%llx -> 全局地址=0x%llx\n",
           (unsigned long long)page, (unsigned long long)off,
           (unsigned long long)(page + off));
    return page + off;
}

// ---------------- ULevel.Actors 遍历 ----------------
// TArray 布局：{ void* Data; int32 Num; int32 Max; }
inline bool dump_actor_list(mach_port_t task, uint64_t worldPtr,
                            uint64_t offPersistentLevel, uint64_t offActors,
                            int maxPrint) {
    if (!worldPtr) { printf("[ue4r] world 为空\n"); return false; }
    uint64_t level = rd64(task, worldPtr + offPersistentLevel);
    printf("[ue4r] UWorld=0x%llx + 0x%llx -> ULevel=0x%llx\n",
           (unsigned long long)worldPtr, (unsigned long long)offPersistentLevel,
           (unsigned long long)level);
    if (!level) return false;

    uint64_t data = 0;
    int32_t num = 0, max = 0;
    if (!read_mem(task, level + offActors, &data, 8)) return false;
    if (!read_mem(task, level + offActors + 8, &num, 4)) return false;
    if (!read_mem(task, level + offActors + 12, &max, 4)) return false;

    printf("[ue4r] Actors: Data=0x%llx Num=%d Max=%d\n",
           (unsigned long long)data, num, max);
    if (num <= 0 || num > 8192 || !data) {
        printf("[ue4r] Num 不合理（期望 1..8192）—— 偏移错或链路失效\n");
        return false;
    }

    int printed = 0;
    for (int i = 0; i < num && printed < maxPrint; i++) {
        uint64_t a = rd64(task, data + (uint64_t)i * 8);
        if (!a) continue;
        printf("  actor[%d] = 0x%llx\n", i, (unsigned long long)a);
        printed++;
    }
    return printed > 0;
}

} // namespace ue4r
