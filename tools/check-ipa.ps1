# check-ipa.ps1 - verify a dumped/decrypted .ipa before shipping it anywhere
#
# ASCII only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI, so non-ASCII
# characters can break parsing. All messages are English by design.
#
# Checks:
#   1. Payload/*.app layout + main executable
#   2. Mach-O architecture (thin / fat, arm64 / arm64e)
#   3. LC_ENCRYPTION_INFO(_64) cryptid -> 0 means really decrypted
#   4. CFBundleShortVersionString / CFBundleVersion (best effort, binary plists included)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File check-ipa.ps1 -Ipa "D:\Arena-decrypted.ipa"

param(
    [Parameter(Mandatory = $true)][string]$Ipa,
    [string]$WorkDir = "$env:TEMP\ipa-check"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa" }
$full = (Resolve-Path $Ipa).Path
$item = Get-Item $full
"ipa      : $full"
"size     : {0:N1} MB" -f ($item.Length / 1MB)

$zip = [IO.Compression.ZipFile]::OpenRead($full)
try {
    # collect entries with normalized separators (Windows packers may write backslashes)
    $entries = @()
    foreach ($e in $zip.Entries) {
        $entries += , @{ name = ($e.FullName -replace '\\', '/'); entry = $e }
    }

    $appPrefix = $null
    foreach ($x in $entries) {
        if ($x.name -match '^(Payload/[^/]+\.app/)') { $appPrefix = $Matches[1]; break }
    }
    if (-not $appPrefix) { "FAIL: no Payload/*.app/ content in this ipa"; exit 1 }
    $appName = ($appPrefix -replace '^Payload/', '' -replace '/$', '')
    "app      : $appName"

    # main executable: direct child of <app>/, no dot in the name, largest such file
    $execEntry = $null
    $bestLen = -1
    foreach ($x in $entries) {
        if ($x.name -match ('^' + [regex]::Escape($appPrefix) + '[^/]+$')) {
            $leaf = ($x.name -split '/')[-1]
            if ($leaf -notmatch '\.' -and $x.entry.Length -gt $bestLen) {
                $bestLen = $x.entry.Length
                $execEntry = $x.entry
            }
        }
    }
    if (-not $execEntry) { "FAIL: main executable not found under $appPrefix"; exit 1 }
    "exec     : $($execEntry.Name)  ({0:N1} MB)" -f ($execEntry.Length / 1MB)

    if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
    New-Item -ItemType Directory -Path $WorkDir | Out-Null
    $execPath = Join-Path $WorkDir $execEntry.Name
    [IO.Compression.ZipFileExtensions]::ExtractToFile($execEntry, $execPath, $true)

    # ---------------- Mach-O ----------------
    $fs = [IO.File]::OpenRead($execPath)
    $br = New-Object IO.BinaryReader($fs)

    function ReadBE32([IO.BinaryReader]$r, [IO.FileStream]$s, [int64]$pos) {
        $s.Position = $pos
        $b = $r.ReadBytes(4)
        return ([int64]$b[0] * 16777216) + ([int64]$b[1] * 65536) + ([int64]$b[2] * 256) + [int64]$b[3]
    }
    function ArchName([int64]$cpu, [int64]$sub) {
        $c = ($cpu -band 0xFFFFFF)
        $s = ($sub -band 0x00FFFFFF)
        if ($c -eq 12) { if ($s -eq 2) { return 'arm64e' } else { return 'arm64' } }
        if ($c -eq 7) { return 'armv7' }
        return ('cpu=0x{0:x}' -f $cpu)
    }

    $fs.Position = 0
    $magic = [int64]$br.ReadUInt32()
    $isFat = ($magic -eq 3405691582 -or $magic -eq 3405691583)

    $slices = @()
    if ($isFat) {
        $n = [int64](ReadBE32 $br $fs 4)
        for ($k = 0; $k -lt $n; $k++) {
            $base = 8 + $k * 20
            $cpu = ReadBE32 $br $fs $base
            $sub = ReadBE32 $br $fs ($base + 4)
            $off = ReadBE32 $br $fs ($base + 8)
            $slices += , @($cpu, $sub, $off)
        }
        "format   : FAT ($n slices)"
    } else {
        $slices += , @([int64]0, [int64]0, [int64]0)
        "format   : thin"
    }

    $ok = $true
    foreach ($sl in $slices) {
        $offset = [int64]$sl[2]
        $fs.Position = $offset
        $m = [int64]$br.ReadUInt32()
        $mHex = '{0:x8}' -f $m
        if ($mHex -ne 'feedfacf') {
            "  slice @0x{0:x}: not MH_MAGIC_64 (magic=0x{1})" -f $offset, $mHex
            $ok = $false
            continue
        }
        $cputype = [int64]$br.ReadUInt32()
        $cpusub  = [int64]$br.ReadUInt32()
        $filetype = [int64]$br.ReadUInt32()
        $ncmds   = [int64]$br.ReadUInt32()
        $null    = [int64]$br.ReadUInt32()   # sizeofcmds
        $arch = ArchName $cputype $cpusub

        $pos = $offset + 32
        $cryptid = -1
        for ($i = 0; $i -lt $ncmds; $i++) {
            $fs.Position = $pos
            $cmd = [int64]$br.ReadUInt32()
            $sz = [int64]$br.ReadUInt32()
            if ($sz -le 0) { break }
            if ($cmd -eq 0x2C -or $cmd -eq 0x21) {
                $fs.Position = $pos + 16
                $cryptid = [int64]$br.ReadUInt32()
            }
            $pos += $sz
        }
        $verdict = 'no encryption LC -> unencrypted'
        if ($cryptid -eq 0) { $verdict = 'cryptid=0 -> DECRYPTED' }
        elseif ($cryptid -gt 0) { $verdict = "cryptid=$cryptid -> STILL ENCRYPTED"; $ok = $false }
        "  slice @0x{0,-10:x} {1,-7} filetype={2} ncmds={3}  {4}" -f $offset, $arch, $filetype, $ncmds, $verdict
    }
    $br.Close(); $fs.Close()

    # ---------------- Info.plist ----------------
    $plist = $null
    foreach ($x in $entries) {
        if ($x.name -eq ($appPrefix + 'Info.plist')) { $plist = $x.entry; break }
    }
    if ($plist) {
        $ms = New-Object IO.MemoryStream
        $st = $plist.Open(); $st.CopyTo($ms); $st.Close()
        $bytes = $ms.ToArray(); $ms.Close()
        $head = [Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min(8, $bytes.Length))
        if ($head.StartsWith('<?xml') -or $head.StartsWith('<plist')) {
            $txt = [Text.Encoding]::UTF8.GetString($bytes)
            foreach ($k in @('CFBundleShortVersionString', 'CFBundleVersion', 'CFBundleIdentifier', 'MinimumOSVersion')) {
                if ($txt -match "<key>$k</key>\s*<string>([^<]+)</string>") { "  $k = $($Matches[1])" }
            }
        } else {
            $run = ''
            $tokens = @()
            foreach ($by in $bytes) {
                if ($by -ge 32 -and $by -lt 127) { $run += [char]$by }
                else { if ($run.Length -ge 2) { $tokens += $run }; $run = '' }
            }
            if ($run.Length -ge 2) { $tokens += $run }
            foreach ($k in @('CFBundleShortVersionString', 'CFBundleVersion', 'CFBundleIdentifier')) {
                for ($i = 0; $i -lt $tokens.Count - 1; $i++) {
                    if ($tokens[$i] -eq $k) {
                        "  $k ~ $($tokens[$i + 1])   (binary plist, approximate)"
                        break
                    }
                }
            }
        }
    }

    ''
    if ($ok) {
        'RESULT: OK - encryption bit is 0, package is ready for static analysis'
        exit 0
    } else {
        'RESULT: FAIL - at least one slice is still encrypted'
        exit 2
    }
} finally {
    $zip.Dispose()
}
