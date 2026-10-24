#!/usr/bin/env bash
# Assemble ipa variants.
#
#   ARHost-J-ents.ipa   app signed WITH host/entitlements.plist (task port entitlements)
#   ARHost-B-full.ipa   app signed vanilla (no entitlements) — known-good control
#
# Both bundles are identical (binary + Info.plist + ARDaemon + ARPayload.dylib);
# only the app's signature differs, so a launch difference isolates entitlements.
set -euo pipefail
cd "$(dirname "$0")/.."

PB=/usr/libexec/PlistBuddy
command -v ldid >/dev/null || { echo "ERROR: ldid not found"; exit 1; }

host_bin=$(find host/.theos/obj    -maxdepth 4 -name 'ARHost'  -type f 2>/dev/null | head -1 || true)
dmn=$(find      daemon/.theos/obj  -maxdepth 4 -name 'ARDaemon' -type f 2>/dev/null | head -1 || true)
pay=$(find      payload/.theos/obj -maxdepth 4 -name '*ARPayload*.dylib' -type f 2>/dev/null | head -1 || true)

echo "host   = ${host_bin:-<missing>}"
echo "daemon = ${dmn:-<missing>}"
echo "payload= ${pay:-<missing>}"
[ -n "$host_bin" ] || { echo "ERROR: ARHost binary not found"; exit 1; }
[ -n "$pay" ] || { echo "ERROR: payload dylib not found"; exit 1; }

rm -rf build
mkdir -p build

mk_app() {  # mk_app <dir> <bundle-id> <display-name>
    local dir="$1" bid="$2" disp="$3"
    rm -rf "$dir"; mkdir -p "$dir"
    cp "$host_bin" "$dir/ARHost"
    cp host/Info.plist "$dir/Info.plist"
    [ -n "${dmn:-}" ] && cp "$dmn" "$dir/ARDaemon" && chmod +x "$dir/ARDaemon"
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

echo
echo "=== J: WITH entitlements (task port) ==="
mk_app build/ARJ.app com.local.arj "AR J"
ldid -Shost/entitlements.plist build/ARJ.app/ARHost
echo "--- signed entitlements ---"
ldid -e build/ARJ.app/ARHost | grep -E '<key>' | sed 's/^[[:space:]]*//' | sed 's#</\?key>##g' || true
[ -n "${dmn:-}" ] && ldid -Sdaemon/entitlements.plist build/ARJ.app/ARDaemon
ldid -S build/ARJ.app/ARPayload.dylib
pack build/ARJ.app ARHost-J-ents.ipa

echo
echo "=== B: control, vanilla sign ==="
mk_app build/ARB.app com.local.arb "AR B"
ldid -S build/ARB.app/ARHost
[ -n "${dmn:-}" ] && ldid -S build/ARB.app/ARDaemon
ldid -S build/ARB.app/ARPayload.dylib
pack build/ARB.app ARHost-B-full.ipa

echo
echo "================ verify ================"
for ipa in build/*.ipa; do
    echo "--- $ipa ---"
    unzip -l "$ipa" | sed -n '4,12p'
done
ls -lh build/*.ipa
shasum -a 256 build/*.ipa
