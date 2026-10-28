#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_deb.py —— 手工打 .deb（不依赖 dpkg-deb，macOS runner 上没有）

为什么自己写：
  Theos 的 `make package` 需要 dpkg-deb/fakeroot，CI 的 macOS runner 默认没有；
  而 .deb 本质就是一个 ar 归档里放三样东西，用 Python 写十几行就够。

rootless 布局（Dopamine / ElleKit）：
  /var/jb/usr/lib/TweakInject/<NAME>.dylib
  /var/jb/usr/lib/TweakInject/<NAME>.plist     ← 过滤器（限定注入哪个 app）

用法：
  python3 make_deb.py AREsp tweak ../build/AREsp.deb
"""

import io
import os
import sys
import glob
import gzip
import tarfile
import time
import struct


def find_dylib(mod_dir, name):
    pats = [
        os.path.join(mod_dir, ".theos", "obj", "*", name + ".dylib"),
        os.path.join(mod_dir, "packages", "**", name + ".dylib"),
    ]
    for p in pats:
        hits = glob.glob(p, recursive=True)
        if hits:
            hits.sort(key=lambda x: ("arm64e" not in x, len(x)))
            return hits[0]
    return None


def add_file(tar, arcname, data, mode=0o644):
    info = tarfile.TarInfo(arcname)
    info.size = len(data)
    info.mode = mode
    info.mtime = int(time.time())
    tar.addfile(info, io.BytesIO(data))


def make_tar_gz(entries):
    """entries: [(arcname, bytes, mode)]"""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as tar:
        for arcname, data, mode in entries:
            add_file(tar, arcname, data, mode)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def ar_member(out, name, data):
    out.write(b"!<arch>\n" if out.tell() == 0 else b"")
    hdr = "{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n".format(
        name, str(int(time.time())), "0", "0", "100644", str(len(data)))
    out.write(hdr.encode("ascii"))
    out.write(data)
    if len(data) % 2:
        out.write(b"\n")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    name, mod_dir, outpath = sys.argv[1], sys.argv[2], sys.argv[3]

    dylib = find_dylib(mod_dir, name)
    if not dylib:
        print("ERROR: 找不到 %s.dylib（先 make all）" % name)
        return 1
    plist = os.path.join(mod_dir, name + ".plist")
    if not os.path.exists(plist):
        print("ERROR: 找不到过滤器 %s" % plist)
        return 1

    with open(dylib, "rb") as f:
        dylib_data = f.read()
    with open(plist, "rb") as f:
        plist_data = f.read()

    print("dylib = %s (%d 字节)" % (dylib, len(dylib_data)))

    control = (
        "Package: com.ar.esp\n"
        "Name: AREsp\n"
        "Version: 1.0.0\n"
        "Architecture: iphoneos-arm64\n"
        "Description: AR ESP tweak (injects into UAGame only)\n"
        "Maintainer: ar\n"
        "Author: ar\n"
        "Section: Tweaks\n"
        "Depends: ellekit\n"
    )

    # control.tar.gz
    control_tar = make_tar_gz([
        ("./control", control.encode(), 0o644),
    ])
    # data.tar.gz：rootless 路径
    inject = "var/jb/usr/lib/TweakInject"
    data_tar = make_tar_gz([
        ("%s/%s.dylib" % (inject, name), dylib_data, 0o755),
        ("%s/%s.plist" % (inject, name), plist_data, 0o644),
    ])

    os.makedirs(os.path.dirname(os.path.abspath(outpath)), exist_ok=True)
    with open(outpath, "wb") as out:
        out.write(b"!<arch>\n")
        for member, blob in (("debian-binary", b"2.0\n"),
                             ("control.tar.gz", control_tar),
                             ("data.tar.gz", data_tar)):
            hdr = "{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n".format(
                member, str(int(time.time())), "0", "0", "100644", str(len(blob)))
            out.write(hdr.encode("ascii"))
            out.write(blob)
            if len(blob) % 2:
                out.write(b"\n")

    print("已生成 %s (%d 字节)" % (outpath, os.path.getsize(outpath)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
