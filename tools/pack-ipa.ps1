# pack-ipa.ps1 - build a proper .ipa with forward-slash entry names
#
# ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI).
#
# Why this exists: Compress-Archive on Windows writes zip entries with backslashes
# ("Payload\App.app\App"). Real IPAs use forward slashes, and some unzip
# implementations on device refuse the backslash form. This packs entries manually.
#
# Usage:
#   powershell -File pack-ipa.ps1 -Root stage-probe -Out out\ARProbe-injected.ipa
#     -Root must be the directory that CONTAINS "Payload"

param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$base = (Resolve-Path $Root).Path
if (-not (Test-Path (Join-Path $base 'Payload'))) {
    throw "no Payload directory under $base"
}
$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
if (Test-Path $Out) { Remove-Item $Out -Force }

# 只打包 Payload/ 下的内容；散落在根目录的临时 zip 之类一律不进包
$files = Get-ChildItem (Join-Path $base 'Payload') -Recurse -File
"packing $($files.Count) files from $base\Payload"

$fs = [IO.File]::Create($Out)
$zip = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($base.Length).TrimStart('\', '/') -replace '\\', '/'
        $entry = $zip.CreateEntry($rel, [IO.Compression.CompressionLevel]::Optimal)
        $es = $entry.Open()
        $ins = [IO.File]::OpenRead($f.FullName)
        try { $ins.CopyTo($es) } finally { $ins.Close(); $es.Close() }
        "  $rel  ($($f.Length))"
    }
} finally {
    $zip.Dispose()
    $fs.Close()
}

$item = Get-Item $Out
"written: $($item.FullName)"
"size   : $($item.Length) bytes"
"sha256 : $((Get-FileHash $Out -Algorithm SHA256).Hash)"

# sanity: entry names must use forward slashes
$z = [IO.Compression.ZipFile]::OpenRead($item.FullName)
try {
    $bad = @($z.Entries | Where-Object { $_.FullName.Contains('\') })
    if ($bad.Count -gt 0) {
        "WARN: $($bad.Count) entries still contain backslashes"
        exit 1
    }
    "entry names: OK (forward slashes)"
} finally { $z.Dispose() }
