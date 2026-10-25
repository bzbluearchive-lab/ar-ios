#!/usr/bin/env bash
# Build the two candidate "real" variants now that the entitlement bisect is done.
#
# Bisect result (measured on device):
#   get-task-allow            OK
#   task_for_pid-allow        OK
#   system-task-ports         OK
#   com.apple.private.cs.debugger   CRASHES ON LAUNCH  -> never use
#   com.apple.private.security.no-sandbox  CRASHES ON LAUNCH -> never use
#
#   ARHost-K1.ipa  task_for_pid-allow + system-task-ports + get-task-allow
#   ARHost-K2.ipa  K1 + skip-library-validation + run-unsigned-code
#                  (K2 only differs if dlopen of our ldid-signed payload is rejected
#                   by library validation; the log will say so)
set -euo pipefail
cd "$(dirname "$0")/.."

PB=/usr/libexec/PlistBuddy
command -v ldid >/dev/null || { echo "ERROR: ldid not found"; exit 1; }

host_bin=$(find host/.theos/obj    -maxdepth 4 -name 'ARHost'  -type f 2>/dev/null | head -1 || true)
dmn=$(find      daemon/.theos/obj  -maxdepth 4 -name 'ARDaemon' -type f 2>/dev/null | head -1 || true)
pay=$(find      payload/.theos/obj -maxdepth 4 -name '*ARPayload*.dylib' -type f 2>/dev/null | head -1 || true)

echo "host    = ${host_bin:-<missing>}"
echo "daemon  = ${dmn:-<missing>}"
echo "payload = ${pay:-<missing>}"
[ -n "$host_bin" ] || { echo "ERROR: no ARHost binary"; exit 1; }
[ -n "$pay" ] || { echo "ERROR: no payload dylib"; exit 1; }

rm -rf build; mkdir -p build/ents

ent() {
    local f="$1"; shift
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
        echo '<plist version="1.0"><dict>'
        for k in "$@"; do echo "<key>$k</key><true/>"; done
        echo '</dict></plist>'
    } > "$f"
}

ent build/ents/K1.plist task_for_pid-allow com.apple.system-task-ports get-task-allow
# P 系列：验证 platform-application 是否能解锁受限权限
ent build/ents/P1.plist platform-application
ent build/ents/P2.plist platform-application com.apple.private.security.no-sandbox
ent build/ents/P3.plist platform-application com.apple.private.cs.debugger
ent build/ents/P4.plist platform-application task_for_pid-allow com.apple.system-task-ports get-task-allow
ent build/ents/P2b.plist platform-application com.apple.private.security.no-sandbox task_for_pid-allow com.apple.system-task-ports get-task-allow
ent build/ents/K2.plist task_for_pid-allow com.apple.system-task-ports get-task-allow \
                        com.apple.private.skip-library-validation run-unsigned-code

mk_app() {
    local dir="$1" bid="$2" disp="$3"
    rm -rf "$dir"; mkdir -p "$dir"
    cp "$host_bin" "$dir/ARHost"
    cp host/Info.plist "$dir/Info.plist"
    [ -n "${dmn:-}" ] && { cp "$dmn" "$dir/ARDaemon"; chmod +x "$dir/ARDaemon"; }
    cp "$pay" "$dir/ARPayload.dylib"
    chmod +x "$dir/ARHost"
    $PB -c "Set :CFBundleIdentifier $bid"   "$dir/Info.plist"
    $PB -c "Set :CFBundleDisplayName $disp" "$dir/Info.plist"
    $PB -c "Set :CFBundleName $disp"        "$dir/Info.plist"
}

pack() {
    rm -rf build/_ipa && mkdir -p build/_ipa/Payload
    cp -a "$1" build/_ipa/Payload/
    ( cd build/_ipa && zip -qry "../$2" Payload )
    echo "packed $2"
}

build_variant() {
    local tag="$1" bid="$2" disp="$3" pl="$4" out="$5"
    echo
    echo "=== $tag ($disp) ==="
    mk_app "build/$tag.app" "$bid" "$disp"
    ldid -S"$pl" "build/$tag.app/ARHost"
    [ -n "${dmn:-}" ] && ldid -S"$pl" "build/$tag.app/ARDaemon"
    ldid -S "build/$tag.app/ARPayload.dylib"
    echo "--- entitlements ---"
    ldid -e "build/$tag.app/ARHost" | grep -E '<key>' | sed 's/^[[:space:]]*//' | sed 's#</\?key>##g' || true
    pack "build/$tag.app" "$out"
}

# P2b = 无沙盒 + 拿 task port 的权限（这两组各自都验证过不崩）
build_variant G11 com.local.arg11 "AR G11" build/ents/P2b.plist ARHost-G11.ipa


echo
echo "================ artifacts ================"
ls -lh build/*.ipa
shasum -a 256 build/*.ipa
