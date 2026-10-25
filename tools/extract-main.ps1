# extract-main.ps1 - pull only what static analysis needs out of an .ipa
#
# ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI).
#
# Extracts, from a decrypted ipa:
#   <Out>\Arena                 main Mach-O binary (the only thing offsets live in)
#   <Out>\Info.plist            version numbers
#   <Out>\frameworks.txt        list of bundled dylibs/frameworks (ACE components, engine)
#
# A full ipa is several GB; this pulls a few hundred MB instead.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File extract-main.ps1 -Ipa "D:\Arena.ipa" -Out "D:\arena-analysis"

param(
    [Parameter(Mandatory = $true)][string]$Ipa,
    [string]$Out = "arena-analysis"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa" }
$full = (Resolve-Path $Ipa).Path
"ipa  : $full"
"size : {0:N1} MB" -f ((Get-Item $full).Length / 1MB)

if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

$zip = [IO.Compression.ZipFile]::OpenRead($full)
try {
    $entries = @()
    foreach ($e in $zip.Entries) {
        $entries += , @{ name = ($e.FullName -replace '\\', '/'); entry = $e }
    }

    $appPrefix = $null
    foreach ($x in $entries) {
        if ($x.name -match '^(Payload/[^/]+\.app/)') { $appPrefix = $Matches[1]; break }
    }
    if (-not $appPrefix) { throw "no Payload/*.app/ content in this ipa" }
    $appName = ($appPrefix -replace '^Payload/', '' -replace '/$', '')
    "app  : $appName"

    # main executable: direct child of <app>/, no dot, biggest
    $execEntry = $null
    $best = -1
    foreach ($x in $entries) {
        if ($x.name -match ('^' + [regex]::Escape($appPrefix) + '[^/]+$')) {
            $leaf = ($x.name -split '/')[-1]
            if ($leaf -notmatch '\.' -and $x.entry.Length -gt $best) {
                $best = $x.entry.Length
                $execEntry = $x.entry
            }
        }
    }
    if (-not $execEntry) { throw "main executable not found under $appPrefix" }

    $execOut = Join-Path $Out $execEntry.Name
    [IO.Compression.ZipFileExtensions]::ExtractToFile($execEntry, $execOut, $true)
    "main : $execOut  ({0:N1} MB)" -f ($execEntry.Length / 1MB)

    $plistEntry = $null
    foreach ($x in $entries) {
        if ($x.name -eq ($appPrefix + 'Info.plist')) { $plistEntry = $x.entry; break }
    }
    if ($plistEntry) {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($plistEntry, (Join-Path $Out 'Info.plist'), $true)
        "plist: Info.plist"
    }

    # frameworks / dylibs / plugin bundles
    $lines = @()
    foreach ($x in $entries) {
        $rel = $x.name.Substring($appPrefix.Length)
        if ($rel -match '^(Frameworks|PlugIns)/.+/[^/]+$') { $lines += ('{0}  {1}' -f $rel, $x.entry.Length) }
    }
    $fwOut = Join-Path $Out 'frameworks.txt'
    if ($lines.Count -gt 0) {
        $lines | Sort-Object | Set-Content $fwOut -Encoding ASCII
        "fw   : $fwOut ($($lines.Count) entries)"
        $lines | Sort-Object | Select-Object -First 20 | ForEach-Object { "   $_" }
        if ($lines.Count -gt 20) { "   ... ($($lines.Count - 20) more)" }
    } else {
        "fw   : none (no Frameworks/ or PlugIns/ files)"
        "none" | Set-Content $fwOut -Encoding ASCII
    }

    ''
    "done. send these to the analyst:"
    "  $execOut"
    "  $(Join-Path $Out 'Info.plist')"
    "  $fwOut"
    "size of payload to send: {0:N1} MB" -f ((Get-ChildItem $Out -File | Measure-Object -Property Length -Sum).Sum / 1MB)
} finally {
    $zip.Dispose()
}
