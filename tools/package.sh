#!/usr/bin/env bash
# Entitlement bisect: one ipa per entitlement set, unique bundle id each, so the
# device can hold them all at once and we learn which entitlement kills launch.
#
#   E1 get-task-allow
#   E2 cs.debugger + get-task-allow
#   E3 task_for_pid-allow + get-task-allow
#   E4 system-task-ports + get-task-allow
#   E5 no-sandbox + get-task-allow
#
# Every variant bundles the same binaries (app + ARDaemon + ARPayload.dylib) and has
# the "注入 SpringBoard" button, so the first variant that launches can be used to
# test the injection path immediately.
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

ent() {  # ent <file> <key...>
    local f="$1"; shift
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
        echo '<plist version="1.0"><dict>'
        for k in "$@"; do echo "<key>$k</key><true/>"; done
        echo '</dict></plist>'
    } > "$f"
}

ent build/ents/E1.plist get-task-allow
ent build/ents/E2.plist com.apple.private.cs.debugger get-task-allow
ent build/ents/E3.plist task_for_pid-allow get-task-allow
ent build/ents/E4.plist com.apple.system-task-ports get-task-allow
ent build/ents/E5.plist com.apple.private.security.no-sandbox get-task-allow

mk_app() {  # mk_app <dir> <bundle-id> <display-name>
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

pack() {  # pack <app-dir> <ipa-name>
    rm -rf build/_ipa && mkdir -p build/_ipa/Payload
    cp -a "$1" build/_ipa/Payload/
    ( cd build/_ipa && zip -qry "../$2" Payload )
    echo "packed $2"
}

build_variant() {  # build_variant <tag> <bundle-id> <display> <plist>
    local tag="$1" bid="$2" disp="$3" pl="$4"
    echo
    echo "=== $tag ($disp) ==="
    mk_app "build/$tag.app" "$bid" "$disp"
    ldid -S"$pl" "build/$tag.app/ARHost"
    [ -n "${dmn:-}" ] && ldid -S"$pl" "build/$tag.app/ARDaemon"
    ldid -S "build/$tag.app/ARPayload.dylib"
    echo "--- entitlements ---"
    ldid -e "build/$tag.app/ARHost" | grep -E '<key>' | sed 's/^[[:space:]]*//' | sed 's#</\?key>##g' || true
    pack "build/$tag.app" "ARHost-$tag-${disp// /_}.ipa"
}

build_variant E1 com.local.are1 "AR E1" build/ents/E1.plist
build_variant E2 com.local.are2 "AR E2" build/ents/E2.plist
build_variant E3 com.local.are3 "AR E3" build/ents/E3.plist
build_variant E4 com.local.are4 "AR E4" build/ents/E4.plist
build_variant E5 com.local.are5 "AR E5" build/ents/E5.plist

echo
echo "================ artifacts ================"
ls -lh build/*.ipa
