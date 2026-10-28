// kfd_glue.c —— 唯一 include vendor/kfd161 的翻译单元（按 **C** 编译）
//
// 位置：src/Kernel/kfd_glue.c
// 作用：把 libkfd 的 4 个公开 API（kopen / kread / kwrite / kclose）与 kern_versions[]
//       收在这一个文件里，向外只暴露 kfd_glue.h 里那 11 个纯 C 函数
//       （10 个原有 + phystokv 门面 kfdg_phystokv）。
//
// 为什么这个文件必须是 .c（不能是 .mm/.cpp）：
//   vendor/kfd161 是 C 源码。perf.h:47/62/63/64 有
//     `*event = (volatile struct perfmon_event){};`
//   这一类 C99 复合字面量；C++ 里它等于"用临时量给 volatile 聚合体赋值"，
//   clang 报 `no viable overloaded '='`。C 里这条语句完全合法。
//
// 为什么 vendor 只能被这一个 TU include：
//   libkfd.h:150-205 带 kfd_init / kfd_free / kopen / kread / kwrite / kclose 的**定义**，
//   dynamic_info.h:111 带 `const struct dynamic_info kern_versions[]` 的**定义**，
//   static_info.h:10 带可变全局 `long long t1sz_boot`。
//   多一个 TU include 一次，就是成批 duplicate symbol。
//
// include 路径说明：
//   本文件位于 <repo>/src/Kernel/kfd_glue.c，vendored libkfd 位于 <repo>/vendor/kfd161/。
//   引号形式 include 先按**本文件所在目录**解析，所以
//       src/Kernel/ + ../../ = <repo>/   →   ../../vendor/kfd161/libkfd.h
//   两个 ../ 缺一不可：只写 ../ 会落到 <repo>/src/vendor/... （不存在），
//   写 ../../../ 会跑到仓库外面去。libkfd.h 内部的 "libkfd/common.h" 同理，
//   是相对 vendor/kfd161/ 解析的，不用我们操心。

/*
 * ★ 下面这几个 libc / BSD 头必须放在 vendor 头**之前** ★
 *
 * 原因：libkfd 的函数体是在 include 的瞬间就被解析的，而 common.h:8-20 的 include 清单
 * 只带了 errno.h / mach/mach.h / pthread.h / semaphore.h / stdatomic.h / stdbool.h /
 * stdio.h / stdlib.h / sys/ioctl.h / sys/mman.h / sys/syscall.h / sys/sysctl.h / unistd.h，
 * 缺了下面这些。C99 起"隐式声明函数"在 clang 上是**硬错误**，缺一个就编不过
 * （上游 kfd 是把这些头跟 App 的其它源码一起拽进来的，我们这张白纸 TU 必须自己补）：
 *
 *   <stdint.h>        common.h:25-35 的 int8_t/uint8_t/.../uint64_t；perf.h:14 的 UINT16_MAX
 *   <stdio.h>         common.h:43 的 printf（vendor 自己也 include，这里只是显式化）
 *   <stdlib.h>        common.h:189/198 的 malloc / free
 *   <string.h>        info.h:111/131 的 memcmp/strlen、info.h:117/137 的 strstr、
 *                     common.h:142 的 strerror、landa.h:63 的 memset
 *   <strings.h>       common.h:191/197 的 bzero、info.h:69-70 的 bcopy
 *                     （Darwin 上这两个在 <strings.h> 而不是 <string.h>）
 *   <fcntl.h>         perf.h:82 的 open("/dev/aes_0", O_RDWR) 与 O_RDWR 常量本身
 *   <sys/time.h>      common.h:162-174 的 struct timeval / gettimeofday / timersub
 *   <sys/resource.h>  info.h:90-91 的 struct rlimit / setrlimit / RLIMIT_NOFILE
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "../../vendor/kfd161/libkfd.h"
#include "kfd_glue.h"

/*
 * ============================================================================
 * method 编号 -> libkfd 的 puaf_method 枚举
 * ============================================================================
 * libkfd.h:21-25
 *   enum puaf_method { puaf_physpuppet, puaf_smith, puaf_landa };
 * 真实数值：puaf_physpuppet = 0、puaf_smith = 1、puaf_landa = 2。
 *
 * ★ 门面对外的约定是 0=physpuppet / 1=landa / 2=smith，与上面**不同**。
 *   把调用方的数字直接透传给 kopen 的话，选 1（landa）会实际走到 smith。
 *   所以这里一律按**名字**取枚举值，映射表只有这一处。
 */
static int kfdg_puaf_enum(int method)
{
    switch (method) {
    case 0:
        return (int)puaf_physpuppet;
    case 1:
        return (int)puaf_landa;
    case 2:
        return (int)puaf_smith;
    default:
        return -1;
    }
}

static const char* kfdg_puaf_name(int method)
{
    switch (method) {
    case 0:
        return "physpuppet";
    case 1:
        return "landa";
    case 2:
        return "smith";
    default:
        return "(非法)";
    }
}

/* kern_versions[] 的条目数（不写死 510，按 sizeof 算，表更新了也不会错） */
static u64 kfdg_version_count(void)
{
    return (u64)(sizeof(kern_versions) / sizeof(kern_versions[0]));
}

/*
 * ============================================================================
 * 公开实现
 * ============================================================================
 */

uint64_t kfdg_open(uint64_t puaf_pages, int method)
{
    /* libkfd.h:173-174 的硬边界：越界时 kopen 里那两条 assert 会直接 abort。 */
    const u64 puaf_pages_min = 16;
    const u64 puaf_pages_max = 65536;

    const int puaf = kfdg_puaf_enum(method);
    if (puaf < 0) {
        printf("[kfdg] open 失败：method=%d 非法，只接受 0=physpuppet 1=landa 2=smith\n",
               method);
        return 0;
    }

    if ((puaf_pages < (uint64_t)puaf_pages_min) || (puaf_pages > (uint64_t)puaf_pages_max)) {
        printf("[kfdg] open 失败：puaf_pages=%llu 超出 kfd 允许区间 [%llu, %llu]"
               "（libkfd.h:173-174 的 assert 一旦越界就是 abort）\n",
               (unsigned long long)puaf_pages,
               (unsigned long long)puaf_pages_min,
               (unsigned long long)puaf_pages_max);
        return 0;
    }

    printf("[kfdg] 调用 kopen(puaf_pages=%llu, puaf_method=%d(%s), kread_method=%d(sem_open), "
           "kwrite_method=%d(sem_open))\n",
           (unsigned long long)puaf_pages,
           puaf, kfdg_puaf_name(method),
           (int)kread_sem_open,
           (int)kwrite_sem_open);

    /*
     * kread/kwrite 固定 sem_open 这一对，理由（这一版源码的硬约束）：
     *   kwrite_sem_open 完全寄生在 kread_sem_open 上（kwrite_sem_open.h:8-15 复用
     *   kread 的 krkw_maximum_id / krkw_method_data，allocate/search 全部转发给
     *   kread_sem_open_*），kread 不走 sem_open 时它没有可用的 psemnode 对象。
     */
    const u64 h = kopen((u64)puaf_pages,
                        (u64)puaf,
                        (u64)kread_sem_open,
                        (u64)kwrite_sem_open);

    if (h == 0) {
        printf("[kfdg] open 失败：kopen 返回 0（malloc_bzero 没拿到内存）\n");
        return 0;
    }

    printf("[kfdg] open 成功：handle=0x%llx\n", (unsigned long long)h);
    return (uint64_t)h;
}

void kfdg_close(uint64_t handle)
{
    if (handle == 0) {
        printf("[kfdg] close：句柄为 0，什么都不做\n");
        return;
    }

    printf("[kfdg] 调用 kclose(0x%llx)\n", (unsigned long long)handle);
    kclose(handle);
    printf("[kfdg] close 完成（perf 的 fd / shared_page、PUAF、KRKW 对象都已释放）\n");
}

int kfdg_read(uint64_t handle, uint64_t kaddr, void* uaddr, uint64_t size)
{
    if (handle == 0) {
        printf("[kfdg] read 拒绝：句柄为 0\n");
        return -1;
    }
    if (uaddr == NULL) {
        printf("[kfdg] read 拒绝：目标用户态缓冲为 NULL\n");
        return -1;
    }
    if (kaddr == 0) {
        printf("[kfdg] read 拒绝：kaddr=0\n");
        return -1;
    }
    if (size == 0) {
        printf("[kfdg] read 拒绝：size=0\n");
        return -1;
    }
    if ((size % sizeof(u64)) != 0) {
        printf("[kfdg] read 拒绝：size=%llu 不是 8 的倍数。kread 内部按 u64 步进"
               "（krkw.h:8-16），余数字节会被静默丢掉；要 4 字节就按 8 字节读回来再截断\n",
               (unsigned long long)size);
        return -1;
    }

    kread(handle, kaddr, uaddr, size);
    return 0;
}

int kfdg_write(uint64_t handle, void* uaddr, uint64_t kaddr, uint64_t size)
{
    if (handle == 0) {
        printf("[kfdg] write 拒绝：句柄为 0\n");
        return -1;
    }
    if (uaddr == NULL) {
        printf("[kfdg] write 拒绝：源用户态缓冲为 NULL\n");
        return -1;
    }
    if (kaddr == 0) {
        printf("[kfdg] write 拒绝：kaddr=0\n");
        return -1;
    }
    if (size == 0) {
        printf("[kfdg] write 拒绝：size=0\n");
        return -1;
    }
    if ((size % sizeof(u64)) != 0) {
        printf("[kfdg] write 拒绝：size=%llu 不是 8 的倍数（kwrite 内部同样按 u64 步进，"
               "见 kwrite_dup.h:65-68）\n",
               (unsigned long long)size);
        return -1;
    }

    /* ★ 顺序是 (句柄, 用户态源, 内核态目的, 长度) —— 与 libkfd.h:38 完全一致 */
    kwrite(handle, uaddr, kaddr, size);
    return 0;
}

int kfdg_version_index(const char* kern_version)
{
    if (kern_version == NULL) {
        printf("[kfdg] version_index：入参为 NULL\n");
        return -1;
    }
    if (kern_version[0] == '\0') {
        printf("[kfdg] version_index：入参是空串（sysctl kern.version 没读到东西？）\n");
        return -1;
    }

    /*
     * 逐字比对 .kern_version（不含尾部的 \r\n —— 由调用方先剥掉）。
     * struct dynamic_info 里这个字段的真实名字是 kern_version，
     * 定义在 vendor/kfd161/libkfd/info/dynamic_info.h:9：
     *     const char* kern_version;
     * （同一结构体里还有 device_id / os_version / build_id，见 dynamic_info.h:106-108。）
     */
    const u64 total = kfdg_version_count();
    u64 i;

    for (i = 0; i < total; i++) {
        const char* table_version = kern_versions[i].kern_version;
        if (table_version == NULL) {
            continue;
        }
        if (strcmp(kern_version, table_version) == 0) {
            return (int)i;
        }
    }

    printf("[kfdg] version_index 未命中：kern_versions[] 共 %llu 条，没有一条的 .kern_version "
           "与 \"%s\" 逐字相等\n",
           (unsigned long long)total,
           kern_version);
    return -1;
}

uint64_t kfdg_kernel_base(uint64_t handle)
{
    if (handle == 0) {
        printf("[kfdg] kernel_base 失败：句柄为 0\n");
        return 0;
    }

    /*
     * 变量名故意叫 kfd：info.h:15 的 kfd_offset(field) 宏展开成
     * kern_versions[kfd->info.env.vid].field，需要作用域里有这个名字。
     * 本函数不直接用那个宏（下面写的是完整成员路径），但保持同名便于对照。
     *
     * struct kfd 的真实成员路径（vendor/kfd161/libkfd.h:137-143 定义了
     *   struct kfd { struct info info; struct perf perf; ... }）：
     *   kfd->info.env.vid      —— libkfd.h:56（info.h:114/135 写入命中下标）
     *   kfd->perf.kernel_slide —— libkfd.h:75（perf.h:101 写入）
     */
    struct kfd* kfd = (struct kfd*)(usize)handle;

    const u64 vid = kfd->info.env.vid;
    const u64 total = kfdg_version_count();

    if (vid >= total) {
        printf("[kfdg] kernel_base 失败：kfd->info.env.vid=%llu 越界（表只有 %llu 条）\n",
               (unsigned long long)vid,
               (unsigned long long)total);
        return 0;
    }

    const u64 static_base = kern_versions[vid].kernelcache__kernel_base;
    if (static_base == 0) {
        printf("[kfdg] kernel_base 失败：条目 [%llu] 的 kernelcache__kernel_base = 0\n",
               (unsigned long long)vid);
        return 0;
    }

    const u64 slide = kfd->perf.kernel_slide;
    if (slide == 0) {
        printf("[kfdg] kernel_base 失败：perf.kernel_slide = 0（perf_run 没走到最后一行，"
               "perf.h:99-101 就是写它的地方；常见原因是 /dev/aes_0 打不开或 perfmon ioctl 被拦）\n");
        return 0;
    }

    /* 与 perf.h:100 完全同一行：base = 条目.kernelcache__kernel_base + kernel_slide */
    const u64 base = static_base + slide;

    printf("[kfdg] kernel_base = 0x%llx（条目 [%llu] kernelcache__kernel_base = 0x%llx + "
           "slide = 0x%llx）\n",
           (unsigned long long)base,
           (unsigned long long)vid,
           (unsigned long long)static_base,
           (unsigned long long)slide);

    return (uint64_t)base;
}

uint64_t kfdg_current_proc(uint64_t handle)
{
    if (handle == 0) {
        printf("[kfdg] current_proc 失败：句柄为 0\n");
        return 0;
    }
    {
        /* info.kaddr.current_proc —— libkfd.h:63 */
        const struct kfd* kfd = (const struct kfd*)(usize)handle;
        return (uint64_t)kfd->info.kaddr.current_proc;
    }
}

uint64_t kfdg_kernel_proc(uint64_t handle)
{
    if (handle == 0) {
        printf("[kfdg] kernel_proc 失败：句柄为 0\n");
        return 0;
    }
    {
        /* info.kaddr.kernel_proc —— libkfd.h:69 */
        const struct kfd* kfd = (const struct kfd*)(usize)handle;
        return (uint64_t)kfd->info.kaddr.kernel_proc;
    }
}

uint64_t kfdg_kernel_slide(uint64_t handle)
{
    if (handle == 0) {
        printf("[kfdg] kernel_slide 失败：句柄为 0\n");
        return 0;
    }
    {
        /* perf.kernel_slide —— libkfd.h:75 */
        const struct kfd* kfd = (const struct kfd*)(usize)handle;
        return (uint64_t)kfd->perf.kernel_slide;
    }
}

uint64_t kfdg_read_u64(uint64_t handle, uint64_t kaddr)
{
    u64 value = 0;

    /* kfdg_read 自己会打印拒绝原因，这里不用重复打 */
    if (kfdg_read(handle, kaddr, &value, (uint64_t)sizeof(value)) != 0) {
        return 0;
    }

    return (uint64_t)value;
}

/*
 * ============================================================================
 * 物理地址 → 内核虚拟地址
 * ============================================================================
 * 逐行移植 vendor/kfd161/libkfd/perf.h:220-238 的 phystokv()：
 *
 *   :222  const u64 PTOV_TABLE_SIZE = 8;                 → 本文件 kfdg_phystokv 里的 PTOV_TABLE_SIZE
 *   :223  const u64 gVirtBase = kfd->perf.gVirtBase;     → 同名局部量
 *   :224  const u64 gPhysBase = kfd->perf.gPhysBase;     → 同名局部量
 *   :225  const u64 gPhysSize = kfd->perf.gPhysSize;     → 同名局部量
 *   :228  ptov_table = &kfd->perf.ptov_table[0];         → 同名局部指针
 *   :230  for (i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++)
 *   :231-233  if ((pa >= ptov_table[i].pa) && (pa < (ptov_table[i].pa + ptov_table[i].len)))
 *                 return pa - ptov_table[i].pa + ptov_table[i].va;
 *   :236  assert(!((pa < gPhysBase) || ((pa - gPhysBase) >= gPhysSize)));
 *   :237  return pa - gPhysBase + gVirtBase;
 *
 * 改动只有一处：:236 那条 assert 换成"越界就打印并返回 0"。
 *   理由与本文件其它函数一致 —— kfd161 里 assert 展开成 abort()（common.h:113-136），
 *   真机上等于进程当场消失且日志可能丢在块缓冲里；phystokv 的入参来自我们自己算出来的
 *   页表项，越界是"我们的推导错了"，必须是可恢复的返回值而不是 abort。
 *
 * 另外加了一条前置检查（不属于原函数）：perf 还没被 perf_run 填过时
 * （gVirtBase / gPhysSize 都是 0）直接返回 0，避免拿着 0 做减法得出一个"看起来像地址"的垃圾值。
 */
uint64_t kfdg_phystokv(uint64_t handle, uint64_t pa)
{
    /* perf.h:222 */
    const u64 PTOV_TABLE_SIZE = 8;

    if (handle == 0) {
        printf("[kfdg] phystokv 失败：句柄为 0\n");
        return 0;
    }

    {
        const struct kfd* kfd = (const struct kfd*)(usize)handle;

        /* perf.h:223-225 */
        const u64 gVirtBase = kfd->perf.gVirtBase;
        const u64 gPhysBase = kfd->perf.gPhysBase;
        const u64 gPhysSize = kfd->perf.gPhysSize;

        /* perf.h:228（原版用 auto，本仓库 vendor/kfd161/libkfd/perf.h:228 已改成显式类型） */
        const struct ptov_table_entry* ptov_table = &kfd->perf.ptov_table[0];

        u64 i;

        /*
         * 前置检查：这四个值全部由 perf_run 写（perf.h:142-155：
         * ptov_table 在 :143、gVirtBase 在 :146、gPhysBase 在 :150、gPhysSize 在 :154）。
         * 一个都没填 = kopen 没走完 perf 那一段，此时下面两条分支都只能算出垃圾。
         */
        if ((gVirtBase == 0) || (gPhysSize == 0)) {
            printf("[kfdg] phystokv 失败：perf 还没填好（gVirtBase=0x%llx gPhysSize=0x%llx）。"
                   "这几个值和 ptov_table 都是 perf_run 里才写的（perf.h:142-155）\n",
                   (unsigned long long)gVirtBase,
                   (unsigned long long)gPhysSize);
            return 0;
        }

        /* perf.h:230-234：ptov_table 优先（表里每条覆盖 [pa, pa+len) 区间） */
        for (i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++) {
            if ((pa >= ptov_table[i].pa) && (pa < (ptov_table[i].pa + ptov_table[i].len))) {
                return (uint64_t)(pa - ptov_table[i].pa + ptov_table[i].va);
            }
        }

        /* perf.h:236 的 assert 换成下面这段：越界就是我们的页表项算错了，返回 0 让调用方停手 */
        if ((pa < gPhysBase) || ((pa - gPhysBase) >= gPhysSize)) {
            printf("[kfdg] phystokv 失败：pa=0x%llx 既不在 ptov_table[0..%llu) 里，也越界 "
                   "(gPhysBase=0x%llx gPhysSize=0x%llx → 合法区间 [0x%llx, 0x%llx))；"
                   "原版 perf.h:236 在这里是 assert，本实现改成返回 0\n",
                   (unsigned long long)pa,
                   (unsigned long long)PTOV_TABLE_SIZE,
                   (unsigned long long)gPhysBase,
                   (unsigned long long)gPhysSize,
                   (unsigned long long)gPhysBase,
                   (unsigned long long)(gPhysBase + gPhysSize));
            return 0;
        }

        /* perf.h:237 */
        return (uint64_t)(pa - gPhysBase + gVirtBase);
    }
}
