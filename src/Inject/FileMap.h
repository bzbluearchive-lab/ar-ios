// FileMap.h —— 用"文件映射"而不是"拷贝字节"把 dylib 送进目标进程
//
// 为什么这条路值得试：
//   我们之前的手动映射是把 dylib 的字节 vm_write 到 vm_allocate 出来的**匿名页**里，
//   然后跳到上面执行 —— 实测（自测C 对照）证明：
//     · 新线程执行**已签名页**（编译进我们二进制的函数）：完全正常
//     · 新线程执行**匿名映射页**（手工拷贝的字节）：进程当场被杀
//   也就是说拦的是"页面没有签名覆盖"，不是"没有权限"。
//
//   文件映射可以让页面的 backstore 就是磁盘上那个**已被签名覆盖的文件**
//   （我们打包时用 ldid 签过），内核/AMFI 看到的 provenance 是 vnode 而不是 anon。
//   如果校验只看 provenance，这条路就能过。
//
// 做法：
//   1. 在**我们自己进程**里 mmap 那个 dylib 文件（只读）
//   2. 用 mach_make_memory_entry_64 把这块映射变成可共享的 memory object
//   3. vm_map 到目标进程，权限 R+X
//   4. 目标里执行 mapped_base + symbol_offset
//
// 注意：这条路的成败完全取决于系统是否接受"文件映射页"作为可执行来源，
// 所以在自己进程里先验一遍，再拿去打游戏。

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

namespace filemap {

struct Mapped {
    vm_address_t remote = 0;      // 目标进程里的基址
    vm_size_t    size = 0;        // 映射长度
    void*        localMap = nullptr;  // 我们自己进程里的映射（用于解析符号）
    size_t       localSize = 0;
    int          fd = -1;
};

// 在本地解析符号，返回它在文件映射内的偏移（dylib 的 vmaddr 从 0 起算）
static inline uintptr_t sym_offset(const uint8_t* file, size_t fileSize, const char* name) {
    if (!file || !name) return 0;
    const struct mach_header_64* mh = (const struct mach_header_64*)file;
    if (mh->magic != MH_MAGIC_64) return 0;

    const struct symtab_command* sym = nullptr;
    const uint8_t* p = file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SYMTAB) { sym = (const struct symtab_command*)lc; break; }
        p += lc->cmdsize;
    }
    if (!sym) return 0;

    const struct nlist_64* nl = (const struct nlist_64*)(file + sym->symoff);
    const char* strs = (const char*)(file + sym->stroff);
    for (uint32_t i = 0; i < sym->nsyms; i++) {
        if (nl[i].n_un.n_strx == 0) continue;
        if ((nl[i].n_type & N_TYPE) != N_SECT) continue;
        if (nl[i].n_sect == 0 || nl[i].n_value == 0) continue;
        const char* nm = strs + nl[i].n_un.n_strx;
        if (nm[0] == '_') nm++;
        if (strcmp(nm, name) == 0) return (uintptr_t)nl[i].n_value;
    }
    return 0;
}

// 把文件映射进目标进程（R+X），返回目标内的基址
static inline bool map_into(mach_port_t task, const char* path, Mapped& out) {
    memset(&out, 0, sizeof(out));
    out.fd = open(path, O_RDONLY);
    if (out.fd < 0) { printf("[filemap] 打不开 %s\n", path); return false; }

    struct stat st;
    if (fstat(out.fd, &st) != 0 || st.st_size <= 0) {
        printf("[filemap] fstat 失败\n");
        close(out.fd); out.fd = -1;
        return false;
    }
    size_t sz = (size_t)st.st_size;
    size_t pageSz = (size_t)getpagesize();
    size_t rounded = (sz + pageSz - 1) & ~(pageSz - 1);

    // 1) 本地只读映射（后面 mach_make_memory_entry 需要一块已映射的区域）
    // 本地映射必须同时带 PROT_EXEC：
    // 后面要向内核请求"可执行"的 memory object，如果源映射没有执行权限，
    // mach_make_memory_entry_64 会返回 KERN_PROTECTION_FAILURE(2)（实测就是这个）。
    void* local = mmap(nullptr, rounded, PROT_READ | PROT_EXEC, MAP_PRIVATE, out.fd, 0);
    if (local == MAP_FAILED) {
        printf("[filemap] 本地 mmap(READ|EXEC) 失败: %s\n", strerror(errno));
        // 退化尝试：只读映射（后面 make_memory_entry 多半会失败，但留下对照日志）
        local = mmap(nullptr, rounded, PROT_READ, MAP_PRIVATE, out.fd, 0);
        if (local == MAP_FAILED) {
            printf("[filemap] 只读映射也失败: %s\n", strerror(errno));
            close(out.fd); out.fd = -1;
            return false;
        }
        printf("[filemap] 警告: 只能拿到只读映射，执行权限请求预计会失败\n");
    }
    out.localMap = local;
    out.localSize = rounded;

    // 2) 变成可共享的 memory object，并要求可执行
    mach_port_t obj = MACH_PORT_NULL;
    // 注意类型：size 形参是 memory_object_size_t(64 位)，offset 是 memory_object_offset_t，
    // 用 vm_size_t / vm_address_t 传入在 C++ 下会"no matching function"。
    memory_object_size_t objSize = (memory_object_size_t)rounded;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(),
                                                 &objSize,
                                                 (memory_object_offset_t)(uintptr_t)local,
                                                 VM_PROT_READ | VM_PROT_EXECUTE,
                                                 &obj,
                                                 MACH_PORT_NULL);
    printf("[filemap] make_memory_entry -> %d size=%llu\n", kr, (unsigned long long)objSize);
    if (kr != KERN_SUCCESS) return false;

    // 3) 映射到目标进程（任意地址，R+X）
    vm_address_t addr = 0;
    kr = vm_map(task, &addr, (vm_size_t)rounded, 0, VM_FLAGS_ANYWHERE,
                obj, 0, FALSE,
                VM_PROT_READ | VM_PROT_EXECUTE,
                VM_PROT_READ | VM_PROT_EXECUTE,
                VM_INHERIT_NONE);
    printf("[filemap] vm_map -> %d addr=0x%llx\n", kr, (unsigned long long)addr);
    if (kr != KERN_SUCCESS) return false;

    out.remote = addr;
    out.size = (vm_size_t)rounded;
    printf("[filemap] 已映射 %s -> 目标 0x%llx (%zu 字节)\n", path,
           (unsigned long long)addr, sz);
    return true;
}

static inline void release(Mapped& m) {
    if (m.localMap && m.localMap != MAP_FAILED) munmap(m.localMap, m.localSize);
    if (m.fd >= 0) close(m.fd);
    memset(&m, 0, sizeof(m));
    m.fd = -1;
}

// 新建线程跳进文件映射里的符号
static inline kern_return_t run_entry(mach_port_t task, uintptr_t fn, uint64_t x0,
                                      uintptr_t stackTop) {
    mach_port_t th = MACH_PORT_NULL;
    kern_return_t kr = thread_create(task, &th);
    printf("[filemap] thread_create -> %d\n", kr);
    if (kr != KERN_SUCCESS) return kr;

    arm_thread_state64_t st;
    memset(&st, 0, sizeof(st));
    st.__pc = (uint64_t)fn;
    st.__sp = (uint64_t)(stackTop & ~0xFULL);
    st.__lr = 0;
    st.__x[0] = x0;
    kr = thread_set_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
    printf("[filemap] set_state -> %d (pc=0x%llx)\n", kr, (unsigned long long)fn);
    if (kr == KERN_SUCCESS) kr = thread_resume(th);
    printf("[filemap] resume -> %d\n", kr);
    return kr;
}

} // namespace filemap
