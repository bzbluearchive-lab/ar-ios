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
                if (!resolved) printf("[mload]   bind 未解析 %s\n", sym);
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
static inline kern_return_t call_addr(mach_port_t task, uintptr_t fn, uintptr_t stackTop) {
    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(task, &th);
    if (kr != KERN_SUCCESS) return kr;

    arm_thread_state64_t st;
    memset(&st, 0, sizeof(st));
    st.__pc = (uint64_t)fn;
    st.__sp = (uint64_t)(stackTop & ~0xFull);
    st.__lr = 0;
    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st,
                          ARM_THREAD_STATE64_COUNT);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);
    return kr;
}

// ---------------- 主流程：把 dylib 手动映射进目标并调用入口 ----------------
inline uintptr_t map_and_call(mach_port_t task, const char* dylibPath, const char* entrySym) {
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
            vm_protect(task, (vm_address_t)(img.remoteBase + s->vmaddr),
                       (vm_size_t)s->vmsize, false, prot);
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
    if (entry) {
        printf("[mload] 调用入口 %s @ 0x%llx\n", entrySym, (unsigned long long)entry);
        call_addr(task, entry, stackTop);
    } else {
        printf("[mload] 没找到入口符号 %s（只跑了 init）\n", entrySym ? entrySym : "(null)");
    }
    return img.remoteBase;
}
} // namespace mload
