import json
import os
import urllib.request

TOKEN = open(os.path.expanduser(r"~\AppData\Local\ar_gh_token"), "r").read().strip() if False else None
try:
    TOKEN = open(os.path.expanduser("~/.ar_gh_token"), "r").read().strip()
except Exception:
    TOKEN = None

HDR = {
    "User-Agent": "dsh",
    "Accept": "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
}
if TOKEN:
    HDR["Authorization"] = "token " + TOKEN

REPOS = [
    "sourcelocation/Picasso-v3",
    "3a1/iOSEsp",
    "Lrdsnow/PureKFD",
    "Hagly1337/ios_injector",
]


def api(url):
    req = urllib.request.Request(url, headers=HDR)
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def raw(url):
    req = urllib.request.Request(url, headers=HDR)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


for repo in REPOS:
    print("=" * 70)
    print(repo)
    try:
        info = api("https://api.github.com/repos/" + repo)
        branch = info.get("default_branch", "main")
        tree = api("https://api.github.com/repos/%s/git/trees/%s?recursive=1" % (repo, branch))
    except Exception as e:
        print("  取树失败: %s" % e)
        continue

    paths = [t["path"] for t in tree.get("tree", []) if t["type"] == "blob"]
    cand = [p for p in paths if p.endswith("dynamic_info.h") or p.endswith("static_info.h")]
    esp = [p for p in paths if p.lower().endswith((".m", ".mm", ".swift", ".c", ".cpp"))
           and ("esp" in p.lower() or "overlay" in p.lower() or "kfd" in p.lower())]
    print("  文件数=%d  dynamic_info/static_info=%s" % (len(paths), cand))
    if esp:
        print("  相关实现文件(前8): %s" % esp[:8])

    for p in cand:
        try:
            blob = raw("https://raw.githubusercontent.com/%s/%s/%s" % (repo, branch, p))
        except Exception as e:
            print("    拉取 %s 失败: %s" % (p, e))
            continue
        txt = blob.decode("utf-8", "replace")
        has20b101 = ("20B101" in txt) or ("16.1.1" in txt)
        vers = [l.strip() for l in txt.split("\n") if "kern_version" in l or "// iOS" in l]
        print("    --- %s (%d 字节)  含16.1.1/20B101: %s" % (p, len(blob), has20b101))
        for v in vers[:12]:
            print("        " + v[:120])
