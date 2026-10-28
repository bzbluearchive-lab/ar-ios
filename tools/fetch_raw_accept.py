import os
import time
import urllib.request

TOKEN = None
try:
    TOKEN = open(os.path.expanduser("~/.ar_gh_token"), "r").read().strip()
except Exception:
    pass

OUT = r"C:\Users\92072\Documents\deepseek-harness\default-workspace\arena-ios\vendor\forks"
os.makedirs(OUT, exist_ok=True)

TARGETS = [
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/libkfd/info/dynamic_info.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/libkfd.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/libkfd/info.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/fun/offsets.h"),
    ("sourcelocation/Picasso-v3", "Picasso/Exploit/KFDSwift/fun/kfd.m"),
    ("Lrdsnow/PureKFD", "PureKFD/Backend/Exploit/KFD.swift"),
]


def fetch_raw(repo, path, tries=5):
    """用 Accept: application/vnd.github.raw 直接拿原始字节"""
    url = "https://api.github.com/repos/%s/contents/%s" % (repo, path)
    hdr = {
        "User-Agent": "dsh",
        "Accept": "application/vnd.github.raw",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if TOKEN:
        hdr["Authorization"] = "token " + TOKEN
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=hdr)
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception as e:
            last = e
            time.sleep(2 + i * 3)
    raise last


for repo, path in TARGETS:
    name = repo.split("/")[-1] + "__" + path.replace("/", "_")
    dst = os.path.join(OUT, name)
    try:
        data = fetch_raw(repo, path)
        open(dst, "wb").write(data)
        txt = data.decode("utf-8", "replace")
        has161 = ("16.1" in txt) or ("22.1.0" in txt)
        print("OK   %-14s %-52s %7d B  含16.1/22.1.0: %s" % (repo.split("/")[-1], path, len(data), has161))
    except Exception as e:
        print("FAIL %-14s %-52s %s" % (repo.split("/")[-1], path, e))
