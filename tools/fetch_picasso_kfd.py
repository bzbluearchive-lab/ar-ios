import base64
import json
import os
import time
import urllib.request

TOKEN = None
try:
    TOKEN = open(os.path.expanduser("~/.ar_gh_token")).read().strip()
except Exception:
    pass

REPO = "sourcelocation/Picasso-v3"
ROOT = "Picasso/Exploit/KFDSwift/libkfd"
BRANCH = "main"
OUT = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\picasso_kfd"
os.makedirs(OUT, exist_ok=True)

HDR_JSON = {
    "User-Agent": "dsh",
    "Accept": "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
}
if TOKEN:
    HDR_JSON["Authorization"] = "token " + TOKEN


def get_json(url, tries=5):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=HDR_JSON)
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read().decode())
        except Exception as e:
            last = e
            time.sleep(2 + i * 2)
    raise last


def get_raw(repo, path, ref, tries=5):
    url = "https://api.github.com/repos/%s/contents/%s?ref=%s" % (repo, path, ref)
    hdr = dict(HDR_JSON)
    hdr["Accept"] = "application/vnd.github.raw"
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=hdr)
            with urllib.request.urlopen(req, timeout=90) as r:
                return r.read()
        except Exception as e:
            last = e
            time.sleep(2 + i * 3)
    raise last


# 1) 找默认分支
info = get_json("https://api.github.com/repos/" + REPO)
BRANCH = info.get("default_branch", "main")
print("默认分支:", BRANCH)

# 2) 递归列目录
tree = get_json("https://api.github.com/repos/%s/git/trees/%s?recursive=1" % (REPO, BRANCH))
paths = [t["path"] for t in tree["tree"]
         if t["type"] == "blob" and t["path"].startswith(ROOT + "/")]
print("libkfd 下文件数:", len(paths))
for p in paths:
    print("   ", p)

# 3) 下载
ok = fail = 0
for p in paths:
    rel = p[len(ROOT) + 1:]
    dst = os.path.join(OUT, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        data = get_raw(REPO, p, BRANCH)
        open(dst, "wb").write(data)
        ok += 1
    except Exception as e:
        fail += 1
        print("FAIL", rel, e)
print("\n下载完成: ok=%d fail=%d" % (ok, fail))

# 4) 核对 T8112 / 22.1.0 19:34:00 条目是否存在
dyn = os.path.join(OUT, "info", "dynamic_info.h")
if os.path.exists(dyn):
    txt = open(dyn, encoding="utf-8", errors="replace").read()
    key = "T8112"
    idx = txt.find("19:34:00")
    print("\ndynamic_info.h 大小 %d 字节" % len(txt))
    print("含 T8112:", key in txt)
    print("含 19:34:00:", idx > 0)
    # 找同时包含 19:34:00 与 T8112 的条目
    import re
    for m in re.finditer(r'kern_version\s*=\s*"([^"]*19:34:00[^"]*)"', txt):
        ver = m.group(1)
        if "T8112" in ver:
            start = txt.rfind("{", 0, m.start())
            end = txt.find("},", m.start())
            block = txt[start:end]
            print("\n=== 命中目标条目 ===")
            print(ver)
            for line in block.split("\n"):
                if "=" in line:
                    print("   " + line.strip())
            break
