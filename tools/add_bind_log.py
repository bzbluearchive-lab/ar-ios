import re

p = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\src\Inject\ManualLoad.h"
s = open(p, encoding="utf-8").read()

# 1) 打印 fixups 头信息与 import_format
old_hdr = '''    const struct dyld_chained_fixups_header* hdr =
        (const struct dyld_chained_fixups_header*)(img.file + fixupsFileOff);'''
new_hdr = '''    const struct dyld_chained_fixups_header* hdr =
        (const struct dyld_chained_fixups_header*)(img.file + fixupsFileOff);
    printf("[mload] fixups 头: format=%u starts=%u imports=%u symbols=%u\\n",
           hdr->import_format, hdr->starts_offset, hdr->imports_offset, hdr->symbols_offset);
    if (hdr->import_format != DYLD_CHAINED_IMPORT) {
        printf("[mload] 警告: import_format=%u 不是 1(Ptr)，import 表步长可能不同\\n",
               hdr->import_format);
    }'''
if old_hdr in s:
    s = s.replace(old_hdr, new_hdr)
else:
    print("!! hdr 锚点没匹配")

# 2) 每个 bind 打印 ordinal / 符号名 / 解析地址
old_bind = '''                void* resolved = dlsym(RTLD_DEFAULT, sym);
                if (!resolved && sym[0] == '_') resolved = dlsym(RTLD_DEFAULT, sym + 1);
                if (!resolved) printf("[mload]   bind 未解析 %s\\n", sym);'''
new_bind = '''                void* resolved = dlsym(RTLD_DEFAULT, sym);
                if (!resolved && sym[0] == '_') resolved = dlsym(RTLD_DEFAULT, sym + 1);
                printf("[mload]   bind[%u] %-28s -> 0x%llx%s\\n",
                       bd.ordinal, sym, (unsigned long long)(uintptr_t)resolved,
                       resolved ? "" : "   << 未解析！");'''
if old_bind in s:
    s = s.replace(old_bind, new_bind)
else:
    print("!! bind 锚点没匹配")

open(p, "w", encoding="utf-8", newline="\n").write(s)

depth = 0
for ln in s.split("\n"):
    t = re.sub(r'"(\\.|[^"\\])*"', '""', ln).split("//")[0]
    for ch in t:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
print("depth =", depth)
print("has fixups header log:", "fixups 头" in s)
print("has per-bind log:", "bind[%u]" in s)
