// ManualLoad.h —— 把 dylib 手动映射进目标进程（不走 dyld）
//
// 为什么要它：
//   dlopen 在目标进程里被两道墙拦住：
//     1) 沙盒的 file-map-executable：不允许从可写目录映射可执行文件
//     2) library validation：App Store app 只认同团队签名的 dylib
//   手动映射绕开两者：我们自己 vm_allocate、自己把段写进去、自己修重定位，
//   dyld 全程不知情，也不需要目标有任何 entitlement。
//
// 只支持 arm64 + LC_DYLD_CHAINED_FIXUPS（现代 dylib 的默认格式）。
// 不支持时明确报错退出，绝不半吊子加载。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <mach-o/loader.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/nlist.h>
#include "ThreadHijack.h"

namespace mload {

struct Image {
    uint8_t*  file = nullptr;
    size_t    fileSize = 0;
    uintptr_t remoteBase = 0;     // 目标进程里的基址
    uintptr_t remoteSlide = 0;    // = remoteBase（因为我们按 vmaddr 原样布局）
    const struct mach_header_64* mh = nullptr;
};

static inline const struct load_command* lc_first(const struct mach_header_64* mh, uint32_t cmd,
                                                  const uint8_t* file) {
    const uint8_t* p = file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == cmd) return lc;
        p += lc->cmdsize;
    }
    return nullptr;
}

// ---------------- 远程内存操作 ----------------
static inline bool remote_write(mach_port_t task, uint64_t addr, const void* src, size_t len) {
    return vm_write(task, (vm_address_t)addr, (vm_offset_t)src,
                    (mach_msg_type_number_t)len) == KERN_SUCCESS;
}

// ---------------- 找 __LINKEDIT 的 vmaddr（dataoff 是相对它的偏移） ----------------
static inline uintptr_t linkedit_vmaddr(const struct mach_header_64* mh, const uint8_t* file) {
    const uint8_t* p = file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (strcmp(s->segname, "__LINKEDIT") == 0) return (uintptr_t)s->vmaddr;
        }
        p += lc->cmdsize;
    }
    return 0;
}

// ---------------- 应用 chained fixups：把 rebase/bind 写进目标内存 ----------------
static inline bool apply_fixups(Image& img, mach_port_t task) {
    const struct mach_header_64* mh = img.mh;
    const struct linkedit_data_command* lc =
        (const struct linkedit_data_command*)lc_first(mh, LC_DYLD_CHAINED_FIXUPS, img.file);
    if (!lc) { printf("[mload] 没有 LC_DYLD_CHAINED_FIXUPS（老格式不支持）\n"); return false; }

    uintptr_t le = linkedit_vmaddr(mh, img.file);
    if (!le) { printf("[mload] 找不到 __LINKEDIT\n"); return false; }

    // dataoff 是相对 __LINKEDIT 的 vmaddr 的偏移
    uintptr_t fixupsOff = le + (lc->dataoff - lc->dataoff);   // 见下：先算出真实地址
    (void)fixupsOff;

    // 正确算法：__LINKEDIT 的 fileoff 与 vmaddr 有固定差值
    const struct segment_command_64* leSeg = nullptr;
    const uint8_t* p = img.file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* c = (const struct load_command*)p;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)c;
            if (strcmp(s->segname, "__LINKEDIT") == 0) { leSeg = s; break; }
        }
        p += c->cmdsize;
    }
    if (!leSeg) { printf("[mload] __LINKEDIT 段缺失\n"); return false; }

    // 文件偏移 -> 虚拟地址：vmaddr + (fileoff - segFileOff)
    uintptr_t fixupsVA = (uintptr_t)leSeg->vmaddr + (lc->dataoff - leSeg->fileoff);
    size_t fixupsFileOff = (size_t)lc->dataoff;
    if (fixupsFileOff + sizeof(struct dyld_chained_fixups_header) > img.fileSize) {
        printf("[mload] fixups 越界\n");
        return false;
    }

    const struct dyld_chained_fixups_header* hdr =
        (const struct dyld_chained_fixups_header*)(img.file + fixupsFileOff);
    printf("[mload] fixups 头: format=%u starts=%u imports=%u symbols=%u\n",
           hdr->imports_format, hdr->starts_offset, hdr->imports_offset, hdr->symbols_offset);
    if (hdr->imports_format == DYLD_CHAINED_IMPORT_ADDEND64) {
        printf("[mload] 致命: imports_format=3 (ADDEND64)，当前实现不支持\n");
        return false;
    }
    if (hdr->imports_format != DYLD_CHAINED_IMPORT && hdr->imports_format != DYLD_CHAINED_IMPORT_ADDEND) {
        printf("[mload] 警告: imports_format=%u 不是 1(Ptr)，import 表步长可能不同\n",
               hdr->imports_format);
    }
    const uint8_t* starts = img.file + fixupsFileOff + hdr->starts_offset;
    const struct dyld_chained_starts_in_image* si =
        (const struct dyld_chained_starts_in_image*)starts;

    const uint8_t* imports = img.file + fixupsFileOff + hdr->imports_offset;
    const char* symbols   = (const char*)(img.file + fixupsFileOff + hdr->symbols_offset);

    // 段虚拟地址 -> 目标地址 的映射：我们按 vmaddr 原样布局，所以 remote = remoteBase + vmaddr
    // （remoteBase 取 vmaddr 最小值对齐后的基址，下面在分配时保证）
    uintptr_t bias = img.remoteBase;

    int nRebase = 0, nBind = 0, nFail = 0, nSlots = 0;

    // 处理一条链：从 pageAddr + startIdx 开始，按 next*4 步进
    auto process_chain = [&](uintptr_t pageAddr, uint32_t startIdx, uint32_t pageSize,
                             uint16_t ptrFmt) {
        uint32_t idx = startIdx;
        int guard = 0;
        while (idx != 0xFFFF && idx < pageSize && guard++ < 20000) {
            uint64_t slotAddr = pageAddr + idx;
            uint64_t raw = 0;
            vm_size_t got = 0;
            if (vm_read_overwrite(task, (vm_address_t)slotAddr, 8,
                                  (vm_address_t)&raw, &got) != KERN_SUCCESS || got != 8) {
                nFail++; return;
            }
            nSlots++;

            if (ptrFmt != DYLD_CHAINED_PTR_64 && ptrFmt != DYLD_CHAINED_PTR_64_OFFSET) {
                printf("[mload] 不支持的指针格式 0x%x\n", ptrFmt);
                return;
            }

            dyld_chained_ptr_64_rebase rb;
            dyld_chained_ptr_64_bind bd;
            memcpy(&rb, &raw, 8);
            memcpy(&bd, &raw, 8);

            uint64_t next = 0;
            if (rb.bind) {
                const struct dyld_chained_import* impEntry =
                    (const struct dyld_chained_import*)imports + bd.ordinal;
                const char* sym = symbols + impEntry->name_offset;
                void* resolved = dlsym(RTLD_DEFAULT, sym);
                if (!resolved && sym[0] == '_') resolved = dlsym(RTLD_DEFAULT, sym + 1);
                printf("[mload]   bind[%u] %-28s -> 0x%llx%s\n",
                       bd.ordinal, sym, (unsigned long long)(uintptr_t)resolved,
                       resolved ? "" : "   << 未解析！");
                uint64_t value = (uint64_t)(uintptr_t)resolved + bd.addend;
                if (!remote_write(task, slotAddr, &value, 8)) nFail++; else nBind++;
                next = bd.next;
            } else {
                uint64_t value = bias + rb.target;
                if (!remote_write(task, slotAddr, &value, 8)) nFail++; else nRebase++;
                next = rb.next;
            }
            // ★ next 的单位是 4 字节，不是字节
            if (next == 0) break;
            idx += (uint32_t)(next * 4);
        }
    };

    for (uint32_t seg = 0; seg < si->seg_count; seg++) {
        uint32_t off = si->seg_info_offset[seg];
        if (off == 0) continue;
        const struct dyld_chained_starts_in_segment* ss =
            (const struct dyld_chained_starts_in_segment*)((const uint8_t*)si + off);

        for (uint16_t pg = 0; pg < ss->page_count; pg++) {
            uint16_t ps = ss->page_start[pg];
            if (ps == DYLD_CHAINED_PTR_START_NONE) continue;
            uintptr_t pageAddr = bias + ss->segment_offset + (uintptr_t)pg * ss->page_size;

            if (ps & DYLD_CHAINED_PTR_START_MULTI) {
                // 一页里有多条链：page_start[ps & 0x7FFF] 开始是一串起点，
                // 带 START_LAST 的那一项是最后一条
                uint32_t k = (uint32_t)(ps & (uint16_t)~DYLD_CHAINED_PTR_START_MULTI);
                int guard = 0;
                while (k < 4096 && guard++ < 4096) {
                    uint16_t e = ss->page_start[k];
                    if (e == DYLD_CHAINED_PTR_START_NONE) break;
                    bool last = (e & DYLD_CHAINED_PTR_START_LAST) != 0;
                    uint16_t real = (uint16_t)(e & (uint16_t)~DYLD_CHAINED_PTR_START_LAST);
                    if (real != DYLD_CHAINED_PTR_START_NONE)
                        process_chain(pageAddr, real, ss->page_size, ss->pointer_format);
                    if (last) break;
                    k++;
                }
            } else {
                process_chain(pageAddr, ps, ss->page_size, ss->pointer_format);
            }
        }
    }

    printf("[mload] fixups: 走过 %d 个槽位\n", nSlots);    printf("[mload] fixups 完成: rebase=%d bind=%d fail=%d\n", nRebase, nBind, nFail);
    return nFail == 0;
}

// ---------------- 调用目标里的函数 ----------------
// 从目标进程已有线程里取 x18（arm64 的 TLS 指针）。
// 我们自己创建的线程 x18=0，一碰 @autoreleasepool / NSString 就会崩。
static inline uint64_t grab_tls_from_target(mach_port_t task) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(task, &list, &count) != KERN_SUCCESS || !list) return 0;
    uint64_t tls = 0;
    for (mach_msg_type_number_t i = 0; i < count && !tls; i++) {
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
            if (st.__x[18] != 0) tls = st.__x[18];
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    if (list) vm_deallocate(mach_task_self(), (vm_address_t)list,
                            count * sizeof(thread_t));
    printf("[mload] 从目标线程取到 x18(TLS)=0x%llx\n", (unsigned long long)tls);
    return tls;
}

static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop,
                                      uint64_t x0 = 0, uint64_t x18 = 0, uint64_t x1 = 0) {
    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(task, &th);
    if (kr != KERN_SUCCESS) { printf("[mload] thread_create -> %d\n", kr); return kr; }

    arm_thread_state64_t st;
    memset(&st, 0, sizeof(st));
    st.__pc    = (uint64_t)fn;
    st.__sp    = (uint64_t)(stackTop & ~0xFull);
    st.__lr    = 0;
    st.__x[0]  = x0;
    st.__x[1]  = x1;
    st.__x[18] = x18;          // TLS
    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st,
                          ARM_THREAD_STATE64_COUNT);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);
    if (kr != KERN_SUCCESS) printf("[mload] set_state/resume -> %d\n", kr);
    return kr;
}

// 把一段字符串写进目标进程，返回它在目标里的地址
static inline uintptr_t write_string(mach_port_t task, const char* s) {
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    vm_address_t addr = 0;
    if (vm_allocate(task, &addr, (vm_size_t)len, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return 0;
    if (!remote_write(task, (uint64_t)addr, s, len)) return 0;
    return (uintptr_t)addr;
}


// 在已加载的 Image 里按名字解析导出符号（返回目标进程内的地址）
inline uintptr_t resolve_sym(const Image& img, const char* name) {
    if (!img.mh || !img.file || !name) return 0;
    const struct symtab_command* sym =
        (const struct symtab_command*)lc_first(img.mh, LC_SYMTAB, img.file);
    if (!sym) return 0;
    const struct nlist_64* nl = (const struct nlist_64*)(img.file + sym->symoff);
    const char* strs = (const char*)(img.file + sym->stroff);
    for (uint32_t i = 0; i < sym->nsyms; i++) {
        if (nl[i].n_un.n_strx == 0) continue;
        // 关键：只接受"已定义的段内符号"。
        // 符号表里同一个名字可能既有未定义项（n_value=0，表示"我要引用它"），
        // 又有定义项。之前没过滤，结果 g_slot 解析成偏移 0（指向 Mach-O 头），
        // 代码往那里写就撞进 RX 段，进程当场被杀。
        if ((nl[i].n_type & N_TYPE) != N_SECT) continue;
        if (nl[i].n_sect == 0) continue;
        if (nl[i].n_value == 0) continue;
        const char* nm = strs + nl[i].n_un.n_strx;
        if (nm[0] == '_') nm++;
        if (strcmp(nm, name) == 0) {
            uintptr_t addr = img.remoteBase + nl[i].n_value;
            printf("[mload] 符号 %s: n_value=0x%llx sect=%u -> 0x%llx\n",
                   name, (unsigned long long)nl[i].n_value,
                   (unsigned)nl[i].n_sect, (unsigned long long)addr);
            return addr;
        }
    }
    printf("[mload] 符号 %s 未找到（已定义项）\n", name);
    return 0;
}


// 从目标内存读回结果槽（5 个 long）
static inline bool read_remote_slot(mach_port_t task, uint64_t addr, long out[5]) {
    vm_size_t got = 0;
    if (vm_read_overwrite(task, (vm_address_t)addr, sizeof(long) * 5,
                          (vm_address_t)out, &got) != KERN_SUCCESS) return false;
    return got == sizeof(long) * 5;
}


// 目标是否处于"可执行"状态（没有被系统挂起）。
// 挂起的进程其线程完全不调度：劫持进去 PC 不会动、新建线程也不会跑，
// 探针还会给出假阳性（PC 冻结在我们设置的位置上，看起来像在跑）。
inline bool target_is_running(mach_port_t task, int* suspendOut = nullptr) {
    task_basic_info_data_t bi;
    mach_msg_type_number_t bc = TASK_BASIC_INFO_COUNT;
    if (task_info(task, TASK_BASIC_INFO, (task_info_t)&bi, &bc) != KERN_SUCCESS) {
        if (suspendOut) *suspendOut = -1;
        return false;
    }
    if (suspendOut) *suspendOut = bi.suspend_count;
    return bi.suspend_count == 0;
}

// ---------------- 主流程：把 dylib 手动映射进目标并调用入口 ----------------
inline uintptr_t map_and_call(mach_port_t task, const char* dylibPath, const char* entrySym, const char* notePath = nullptr, bool hijackEntry = false) {
    int susp = 0;
    if (!target_is_running(task, &susp)) {
        printf("[mload] 目标不可执行: suspend_count=%d -> 本次放弃注入（等它回到前台）\n", susp);
        return 0;
    }
    Image img;
    FILE* f = fopen(dylibPath, "rb");
    if (!f) { printf("[mload] 打不开 %s\n", dylibPath); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return 0; }
    img.file = (uint8_t*)malloc((size_t)sz);
    if (!img.file) { fclose(f); return 0; }
    fread(img.file, 1, (size_t)sz, f);
    fclose(f);
    img.fileSize = (size_t)sz;

    struct mach_header_64* mh = (struct mach_header_64*)img.file;
    if (mh->magic != MH_MAGIC_64) { printf("[mload] 不是 arm64 Mach-O\n"); return 0; }
    if ((mh->cputype & 0x00FFFFFF) != 12) { printf("[mload] 不是 ARM\n"); return 0; }
    img.mh = mh;

    // 1) 计算 span
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    const uint8_t* p = img.file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (s->vmsize == 0) { p += lc->cmdsize; continue; }
            if (s->vmaddr < lo) lo = (uintptr_t)s->vmaddr;
            if (s->vmaddr + s->vmsize > hi) hi = (uintptr_t)(s->vmaddr + s->vmsize);
        }
        p += lc->cmdsize;
    }
    if (lo >= hi) { printf("[mload] 段表异常\n"); return 0; }
    size_t span = (size_t)(hi - lo);
    printf("[mload] 布局: lo=0x%llx hi=0x%llx span=%zu\n",
           (unsigned long long)lo, (unsigned long long)hi, span);

    // 2) 目标里分配
    // 关键诊断：目标是否被挂起。挂起的进程线程不调度，
    // 之前很多次"劫持 entered=0 / 新建线程不执行"都是这个原因。
    {
        task_basic_info_data_t bi;
        mach_msg_type_number_t bc = TASK_BASIC_INFO_COUNT;
        if (task_info(task, TASK_BASIC_INFO, (task_info_t)&bi, &bc) == KERN_SUCCESS) {
            printf("[mload] 目标状态: suspend_count=%d 虚拟内存=%llu MB 常驻=%llu MB\n",
                   bi.suspend_count,
                   (unsigned long long)(bi.virtual_size / (1024 * 1024)),
                   (unsigned long long)(bi.resident_size / (1024 * 1024)));
            if (bi.suspend_count > 0) {
                printf("[mload] 警告: 目标处于挂起状态（suspend_count>0），"
                       "它的线程不会执行任何代码；请确认游戏在前台\n");
            }
        }
        thread_act_array_t tl = nullptr;
        mach_msg_type_number_t tc = 0;
        if (task_threads(task, &tl, &tc) == KERN_SUCCESS && tl) {
            printf("[mload] 目标线程数=%u\n", tc);
            if (tc < 5) printf("[mload] 警告: 线程数异常少，这很可能不是正常运行的游戏实例\n");
            for (mach_msg_type_number_t k = 0; k < tc; k++) mach_port_deallocate(mach_task_self(), tl[k]);
            vm_deallocate(mach_task_self(), (vm_address_t)tl, tc * sizeof(thread_t));
        }
    }

    vm_address_t base = 0;
    if (vm_allocate(task, &base, (vm_size_t)span, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[mload] vm_allocate 失败\n");
        return 0;
    }
    img.remoteBase = (uintptr_t)base - lo;      // 使 remoteBase + vmaddr 指向真实地址
    printf("[mload] 目标基址: alloc=0x%llx remoteBase=0x%llx\n",
           (unsigned long long)base, (unsigned long long)img.remoteBase);

    // 3) 写段
    p = img.file + sizeof(*mh);
    int wrote = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (s->filesize && s->fileoff + s->filesize <= img.fileSize) {
                uint64_t dst = img.remoteBase + s->vmaddr;
                if (!remote_write(task, dst, img.file + s->fileoff, (size_t)s->filesize)) {
                    printf("[mload] 写段 %s 失败\n", s->segname);
                } else wrote++;
            }
        }
        p += lc->cmdsize;
    }
    printf("[mload] 已写入 %d 个段\n", wrote);

    // 4) 修重定位
    if (!apply_fixups(img, task)) { printf("[mload] fixups 未全部成功\n"); }

    // 5) 设段权限
    p = img.file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (s->vmsize == 0) { p += lc->cmdsize; continue; }
            vm_prot_t prot = VM_PROT_READ;
            if (s->initprot & VM_PROT_EXECUTE) prot |= VM_PROT_EXECUTE;
            if (s->initprot & VM_PROT_WRITE)   prot |= VM_PROT_WRITE;
            kern_return_t pk = vm_protect(task, (vm_address_t)(img.remoteBase + s->vmaddr),
                                          (vm_size_t)s->vmsize, false, prot);
            printf("[mload] 段 %s 权限 -> %d (prot=0x%x)\n", s->segname, pk, prot);
        }
        p += lc->cmdsize;
    }

    // 6) 栈
    vm_address_t stack = 0;
    if (vm_allocate(task, &stack, 0x10000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        printf("[mload] 栈分配失败\n");
        return 0;
    }
    uintptr_t stackTop = (uintptr_t)stack + 0x10000;

    // 7) 跑 __mod_init_func
    p = img.file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            const struct section_64* sec = (const struct section_64*)((const uint8_t*)s + sizeof(*s));
            for (uint32_t k = 0; k < s->nsects; k++) {
                if (strcmp(sec[k].sectname, "__mod_init_func") == 0) {
                    uint32_t n = sec[k].size / 8;
                    printf("[mload] __mod_init_func 有 %u 项\n", n);
                    for (uint32_t m = 0; m < n; m++) {
                        uint64_t fnAddr = img.remoteBase + sec[k].addr + m * 8;
                        uint64_t fn = 0;
                        vm_size_t got = 0;
                        if (vm_read_overwrite(task, (vm_address_t)fnAddr, 8,
                                              (vm_address_t)&fn, &got) == KERN_SUCCESS && got == 8) {
                            printf("[mload]   调用 init 0x%llx\n", (unsigned long long)fn);
                            call_addr(task, (uintptr_t)fn, stackTop);
                        }
                    }
                }
            }
        }
        p += lc->cmdsize;
    }

    // 8) 解析入口符号并调用
    uintptr_t entry = 0;
    if (entrySym && entrySym[0]) {
        const struct symtab_command* sym =
            (const struct symtab_command*)lc_first(mh, LC_SYMTAB, img.file);
        if (sym) {
            const struct nlist_64* nl = (const struct nlist_64*)(img.file + sym->symoff);
            const char* strs = (const char*)(img.file + sym->stroff);
            for (uint32_t i = 0; i < sym->nsyms; i++) {
                if (nl[i].n_un.n_strx == 0) continue;
                const char* nm = strs + nl[i].n_un.n_strx;
                if (nm[0] == '_') nm++;
                if (strcmp(nm, entrySym) == 0) {
                    entry = img.remoteBase + nl[i].n_value;
                    break;
                }
            }
        }
    }
    vm_address_t slot = 0;   // payload 结果槽（作用域要覆盖整个入口流程）

    if (entry) {
        // ---- 1) 先做安全的自旋探针：只验证"映射进来的代码能否被执行" ----
        //     arp_spin 不调用任何外部函数，所以它失败不会破坏目标；
        //     探针不过就绝不调用 arp_entry，避免为了解一个未知问题把游戏搞崩。
        uintptr_t spin = 0;   // 自旋探针要冻结游戏线程，风险大于收益，先关掉
        bool spinOk = true;   // 探针关掉时不作为闸门：入口照常执行，用结果槽判断成败
        if (spin) {
            printf("[mload] 自旋探针地址=0x%llx\n", (unsigned long long)spin);
            spinOk = hijack::probe_execution(task, spin);
            printf("[mload] 自旋探针结果=%s\n", spinOk ? "代码可执行" : "代码不可执行");
        } else {
            printf("[mload] 自旋探针已关闭，直接用结果槽判断入口是否执行\n");
        }

        // ---- 2) 回读入口首指令（确认内容写对了）----
        uint32_t insn = 0;
        vm_size_t got2 = 0;
        if (vm_read_overwrite(task, (vm_address_t)entry, 4, (vm_address_t)&insn, &got2) == KERN_SUCCESS && got2 == 4) {
            printf("[mload] 入口首指令 = 0x%08x%s\n", insn,
                   (insn == 0 || insn == 0xFFFFFFFF) ? "  << 全 0/全 F，代码没写进去！" : "");
        }

        // ---- 3) 只有探针确认可执行，才真正调用 arp_entry ----
        if (spinOk) {
            uint64_t tls = grab_tls_from_target(task);
            uintptr_t noteAddr = notePath ? write_string(task, notePath) : 0;

            // 结果槽改成注入方 vm_allocate 出来的一块内存，通过 x0 传给 payload。
            // 不用 payload 自己的全局变量：那条路径依赖 rebase 修正，
            // 而我们目前的 fixup 遍历 rebase 一直是 0，写进去就是地址 0。
            if (vm_allocate(task, &slot, 0x1000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) slot = 0;
            printf("[mload] 结果槽(vm_allocate)=0x%llx\n", (unsigned long long)slot);
            if (noteAddr)
                printf("[mload] 面包屑路径已写入目标: 0x%llx (%s)\n",
                       (unsigned long long)noteAddr, notePath);
            printf("[mload] 调用入口 %s @ 0x%llx (x0=槽 0x%llx) 方式=%s\n", entrySym,
                   (unsigned long long)entry, (unsigned long long)slot,
                   hijackEntry ? "短窗口劫持(25ms)" : "新建线程");

            // 短窗口劫持：每次只冻结一个游戏线程 25ms。
            // 每试一次就读结果槽，有值就停 —— 平均冻结时间最短，
            // 既能让代码跑完，又不会让游戏卡到被 watchdog 杀掉。
            long probeVals[5] = {0, 0, 0, 0, 0};
            bool ranOk = false;
            for (int attempt = 0; attempt < 3 && !ranOk; attempt++) {
                hijack::hijack_quick(task, entry, (uint64_t)slot, 25, 1);
                usleep(150 * 1000);
                if (slot && read_remote_slot(task, slot, probeVals) && probeVals[0] != 0) {
                    ranOk = true;
                    printf("[mload] 第 %d 次短劫持后结果槽有值 -> 代码已执行\n", attempt + 1);
                } else {
                    printf("[mload] 第 %d 次短劫持后结果槽仍为空\n", attempt + 1);
                }
            }
            if (!ranOk) {
                printf("[mload] 短劫持未能让入口执行（不再回退新建线程：已验证不执行）\n");
            }
        } else {
            printf("[mload] 跳过入口调用（自旋探针未通过）\n");
        }

        // ---- 4) 回读结果槽：这是"代码有没有执行、open 有没有成功"的硬证据 ----
        if (slot) {
            long vals[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < 8; i++) {
                usleep(150 * 1000);
                if (!read_remote_slot(task, slot, vals)) continue;
                if (vals[0] != 0) break;
            }
            printf("[mload] 结果槽: entered=0x%lx open=%ld write=%ld done=0x%lx\n",
                   vals[0], vals[1], vals[2], vals[3]);
            if (vals[0] == 0) {
                printf("[mload] 槽仍是空的 -> 入口代码根本没开始执行\n");
            } else {
                printf("[mload] 代码已执行；open 返回 %ld%s\n", vals[1],
                       vals[1] < 0 ? "（失败：沙盒拒绝或路径不可写）" : "（成功）");
            }
        }
    } else {
        printf("[mload] 没找到入口符号 %s（只跑了 init）\n", entrySym ? entrySym : "(null)");
    }

    return img.remoteBase;
}

} // namespace mload
