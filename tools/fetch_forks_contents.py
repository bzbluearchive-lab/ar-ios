import base64
import json
import os
import time
import urllib.request

TOKEN = None
try:
    TOKEN = open(os.path.expanduser("~/.ar_gh_token"), "r").read().strip()
except Exception:
    pass

HDR = {
    "User-Agent": "dsh",
    "Accept": "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
}
if TOKEN:
    HDR["Authorization"] = "token " + TOKEN

OUT = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\forks"
os.makedirs(OUT, exist_ok=True)

WANT = [
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/libkfd/info/dynamic_info.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/libkfd/info/static_info.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/fun/offsets.m"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/fun/krw.c"),
    ("Hagly1337/ios_injector", "aloha/kfd/Exploit/libkfd/info/dynamic_info.h"),
    ("Hagly1337/ios_injector", "aloha/kfd/Exploit/libkfd/info/static_info.h"),
    ("3a1/iOSEsp", "frontend/src/overlay.m"),
]


def api(url, tries=4):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=HDR)
            with urllib.request.urlopen(req, timeout=45) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:
            last = e
            time.sleep(2 + i * 2)
    raise last


for repo, path in WANT:
    name = repo.split("/")[-1] + "__" + path.replace("/", "_")
    dst = os.path.join(OUT, name)
    try:
        j = api("https://api.github.com/repos/%s/contents/%s" % (repo, path))
        if isinstance(j, list):
            print("  %s/%s 是目录，跳过" % (repo, path))
            continue
        data = base64.b64decode(j["content"])
        open(dst, "wb").write(data)
        txt = data.decode("utf-8", "replace")
        flag = ("20B101" in txt) or ("16.1.1" in txt)
        print("OK   %-28s %-52s %6d B  含16.1.1: %s" % (repo.split("/")[-1], path, len(data), flag))
        if "dynamic_info.h" in path:
            for line in txt.split("\n"):
                if "// iOS" in line or "kern_version" in line:
                    print("        " + line.strip()[:130])
    except Exception as e:
        print("FAIL %-28s %-52s %s" % (repo.split("/")[-1], path, e))
