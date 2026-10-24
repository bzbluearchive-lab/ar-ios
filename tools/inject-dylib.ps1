# inject-dylib.ps1 - add an LC_LOAD_DYLIB load command to a thin 64-bit Mach-O
#
# ASCII only. Windows PowerShell 5.1 compatible.
# Works by writing into the existing header padding (between the end of the load
# commands and the first byte of real section data). No file shifting, no offsets
# change, so LC_SEGMENT/LC_CODE_SIGNATURE offsets stay valid.
#
# Usage:
#   powershell -File inject-dylib.ps1 -Binary path\to\App -Dylib "@executable_path/PlugIns/ARCheat.dylib"

param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$Dylib,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes($Binary)

function RD32([int]$o) { return [int64]$bytes[$o] -bor ([int64]$bytes[$o+1] -shl 8) -bor ([int64]$bytes[$o+2] -shl 16) -bor ([int64]$bytes[$o+3] -shl 24) }
function PAIR([int]$o) { return (RD32 $o) + (RD32 ($o+4)) * 4294967296 }

$magic = RD32 0
$magicHex = '{0:x8}' -f $magic
if ($magicHex -ne 'feedfacf') {
    "MAGIC=0x$magicHex -> not a thin little-endian 64-bit Mach-O (feedfacf). Aborting."
    exit 1
}

$ncmds      = [int](RD32 16)
$sizeofcmds = [int](RD32 20)
$lcStart    = 32
$lcEnd      = $lcStart + $sizeofcmds
"ncmds=$ncmds sizeofcmds=$sizeofcmds lcEnd=0x{0:x} fileSize=0x{1:x}" -f $lcEnd, $bytes.Length

# ---- walk load commands: collect existing LC_LOAD_DYLIB and the first data offset ----
$off = $lcStart
$existing = @()
$minData = $bytes.Length
for ($i = 0; $i -lt $ncmds; $i++) {
    if ($off + 8 -gt $bytes.Length) { "load command table overruns file"; exit 1 }
    $cmd     = RD32 $off
    $cmdsize = RD32 ($off + 4)
    if ($cmdsize -le 0 -or ($off + $cmdsize) -gt $lcEnd) { "bad cmdsize at 0x{0:x}" -f $off; exit 1 }

    if ($cmd -eq 0xC) {                       # LC_LOAD_DYLIB
        $no = RD32 ($off + 8)
        $s = ""
        for ($k = $off + $no; $k -lt $off + $cmdsize; $k++) {
            if ($bytes[$k] -eq 0) { break }
            $s += [char]$bytes[$k]
        }
        $existing += $s
    }
    elseif ($cmd -eq 0x19) {                  # LC_SEGMENT_64
        $nsects = RD32 ($off + 64)
        $segFileOff = RD32 ($off + 40)
        if ($segFileOff -gt 0 -and $segFileOff -lt $minData) { $minData = $segFileOff }
        for ($s2 = 0; $s2 -lt $nsects; $s2++) {
            $so = $off + 72 + $s2 * 80
            $soff = RD32 ($so + 48)
            if ($soff -gt 0 -and $soff -lt $minData) { $minData = $soff }
        }
    }
    elseif ($cmd -eq 0x1D) {                  # LC_CODE_SIGNATURE
        $dat = RD32 ($off + 8)
        if ($dat -gt 0 -and $dat -lt $minData) { $minData = $dat }
    }
    $off += $cmdsize
}

"existing LC_LOAD_DYLIB:"
if ($existing.Count -eq 0) { "  (none)" } else { foreach ($e in $existing) { "  $e" } }
"first data offset = 0x{0:x}, header padding = {1} bytes" -f $minData, ($minData - $lcEnd)

if ($Check) { exit 0 }

if ($existing -contains $Dylib) { "already present, nothing to do"; exit 0 }

# ---- build the new load command (must be 8-byte aligned) ----
$nameBytes = [Text.Encoding]::ASCII.GetBytes($Dylib)
$need = 24 + $nameBytes.Length + 1
$need = [int]([math]::Ceiling($need / 8.0) * 8)
$padding = $minData - $lcEnd
"need=$need bytes, padding=$padding bytes"
if ($padding -lt $need) {
    "NOT ENOUGH PADDING: cannot add the load command without shifting file data. Aborting."
    exit 2
}

$p = $lcEnd
$bytes[$p + 0] = 0x0C; $bytes[$p + 1] = 0; $bytes[$p + 2] = 0; $bytes[$p + 3] = 0     # LC_LOAD_DYLIB
$bytes[$p + 4] = [byte]($need -band 0xFF); $bytes[$p + 5] = [byte](($need -shr 8) -band 0xFF)
$bytes[$p + 6] = 0; $bytes[$p + 7] = 0
$bytes[$p + 8] = 24; $bytes[$p + 9] = 0; $bytes[$p + 10] = 0; $bytes[$p + 11] = 0     # name offset
$bytes[$p + 12] = 2; $bytes[$p + 13] = 0; $bytes[$p + 14] = 0; $bytes[$p + 15] = 0    # timestamp
$bytes[$p + 16] = 0x00; $bytes[$p + 17] = 0x00; $bytes[$p + 18] = 0x01; $bytes[$p + 19] = 0  # current 0x10000
$bytes[$p + 20] = 0x00; $bytes[$p + 21] = 0x00; $bytes[$p + 22] = 0x01; $bytes[$p + 23] = 0  # compat 0x10000
for ($k = 0; $k -lt $nameBytes.Length; $k++) { $bytes[$p + 24 + $k] = $nameBytes[$k] }
for ($k = 24 + $nameBytes.Length; $k -lt $need; $k++) { $bytes[$p + $k] = 0 }

# ncmds + 1, sizeofcmds + need
$newNcmds = $ncmds + 1
$newSize  = $sizeofcmds + $need
$bytes[16] = [byte]($newNcmds -band 0xFF);        $bytes[17] = [byte](($newNcmds -shr 8) -band 0xFF)
$bytes[18] = [byte](($newNcmds -shr 16) -band 0xFF); $bytes[19] = [byte](($newNcmds -shr 24) -band 0xFF)
$bytes[20] = [byte]($newSize -band 0xFF);         $bytes[21] = [byte](($newSize -shr 8) -band 0xFF)
$bytes[22] = [byte](($newSize -shr 16) -band 0xFF); $bytes[23] = [byte](($newSize -shr 24) -band 0xFF)

[IO.File]::WriteAllBytes($Binary, $bytes)
"PATCHED ncmds=$newNcmds sizeofcmds=$newSize"
"added LC_LOAD_DYLIB -> $Dylib"
