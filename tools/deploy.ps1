# deploy.ps1 - push whole tree, build on GitHub Actions, download ipas to Desktop
#
# ASCII only; Windows PowerShell 5.1 reads .ps1 as ANSI and non-ASCII can break parsing.
#
# TOKEN HANDLING (learned the hard way):
#   The token is NEVER stored in this repo. It is read from, in order:
#     1. -Token parameter
#     2. $env:AR_GH_TOKEN
#     3. %USERPROFILE%\.ar_gh_token   (a file outside the project, one line, no newline needed)
#   Committing a token to a public repo makes GitHub secret-scanning revoke it instantly.
#
# Repo notes:
#   - private repos on this account have no macOS minutes left, so the repo is flipped
#     public for the build (public repos get free runners) and restored afterwards.
#   - the repo also carries someone else's build-esp workflow, so runs are matched by
#     workflow name "build" AND our commit sha.

param(
    [string]$Message = "update",
    [string]$Token,
    [string]$RepoName = "ar-ios",
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$api = 'https://api.github.com'

if (-not $Token) { $Token = $env:AR_GH_TOKEN }
if (-not $Token) {
    $tf = Join-Path $env:USERPROFILE '.ar_gh_token'
    if (Test-Path $tf) { $Token = (Get-Content $tf -Raw).Trim() }
}
if (-not $Token) { throw "no token: pass -Token, set AR_GH_TOKEN, or put it in %USERPROFILE%\.ar_gh_token" }

$hdr = @{
    Authorization          = "token $Token"
    'User-Agent'           = 'ar-deploy'
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}

$me = Invoke-RestMethod -Uri "$api/user" -Headers $hdr -Method Get
$login = $me.login
$base = "$api/repos/$login/$RepoName"

# ---------- 1. push whole tree as ONE commit ----------
$exclude = '(^|\\)(build|\.theos|build-logs|\.git|out)(\\|$)'
$files = Get-ChildItem $root -Recurse -File -Force |
    Where-Object { $_.FullName.Replace($root + '\', '') -notmatch $exclude } |
    ForEach-Object { [pscustomobject]@{ Rel = ($_.FullName.Replace($root + '\', '') -replace '\\', '/'); Full = $_.FullName } } |
    Sort-Object Rel

$ref = Invoke-RestMethod -Uri "$base/git/ref/heads/main" -Headers $hdr -Method Get
$headSha = $ref.object.sha
$headCommit = Invoke-RestMethod -Uri "$base/git/commits/$headSha" -Headers $hdr -Method Get

$entries = @()
foreach ($f in $files) {
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($f.Full))
    $blob = Invoke-RestMethod -Uri "$base/git/blobs" -Headers $hdr -Method Post -Body ([Text.Encoding]::UTF8.GetBytes((@{ content = $b64; encoding = 'base64' } | ConvertTo-Json)))
    $entries += @{ path = $f.Rel; mode = '100644'; type = 'blob'; sha = $blob.sha }
}
$tree = Invoke-RestMethod -Uri "$base/git/trees" -Headers $hdr -Method Post -Body ([Text.Encoding]::UTF8.GetBytes((@{ base_tree = $headCommit.tree.sha; tree = $entries } | ConvertTo-Json -Depth 6)))
$nc = Invoke-RestMethod -Uri "$base/git/commits" -Headers $hdr -Method Post -Body ([Text.Encoding]::UTF8.GetBytes((@{ message = $Message; tree = $tree.sha; parents = @($headSha) } | ConvertTo-Json -Depth 4)))
$null = Invoke-RestMethod -Uri "$base/git/refs/heads/main" -Headers $hdr -Method Patch -Body ([Text.Encoding]::UTF8.GetBytes((@{ sha = $nc.sha } | ConvertTo-Json)))
"pushed $($nc.sha.Substring(0,8))  ($($files.Count) files)"

if ($NoBuild) { return }

# ---------- 2. public -> dispatch ----------
$null = Invoke-RestMethod -Uri $base -Headers $hdr -Method Patch -Body ([Text.Encoding]::UTF8.GetBytes((@{ private = $false } | ConvertTo-Json)))
"repo set public (free runners)"
$wfs = Invoke-RestMethod -Uri "$base/actions/workflows" -Headers $hdr -Method Get
$bw = $wfs.workflows | Where-Object { $_.path -eq '.github/workflows/build.yml' }
$null = Invoke-RestMethod -Uri "$base/actions/workflows/$($bw.id)/dispatches" -Headers $hdr -Method Post -Body ([Text.Encoding]::UTF8.GetBytes((@{ ref = 'main' } | ConvertTo-Json)))
"dispatched build"

# ---------- 3. wait ----------
$run = $null
Start-Sleep -Seconds 15
for ($i = 1; $i -le 40; $i++) {
    $runs = Invoke-RestMethod -Uri "$base/actions/runs?per_page=8" -Headers $hdr -Method Get
    $run = $runs.workflow_runs | Where-Object { $_.name -eq 'build' -and $_.head_sha -eq $nc.sha } | Select-Object -First 1
    if ($run) {
        "$($run.status)/$($run.conclusion)"
        if ($run.status -eq 'completed') { break }
    }
    Start-Sleep -Seconds 15
}

# ---------- 4. fetch or report ----------
$out = Join-Path $root 'out'
if ($run -and $run.conclusion -eq 'success') {
    $arts = Invoke-RestMethod -Uri "$base/actions/runs/$($run.id)/artifacts" -Headers $hdr -Method Get
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Path $out | Out-Null
    Invoke-WebRequest -Uri $arts.artifacts[0].archive_download_url -Headers $hdr -OutFile (Join-Path $out 'r.zip') -UseBasicParsing
    Expand-Archive -Path (Join-Path $out 'r.zip') -DestinationPath $out -Force
    Get-ChildItem "C:\Users\92072\Desktop" -Filter 'ARHost*.ipa' -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem $out -Filter '*.ipa' | Sort-Object Name | ForEach-Object {
        Copy-Item $_.FullName "C:\Users\92072\Desktop\$($_.Name)" -Force
        "  $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB"
    }
    "copied to Desktop"
} elseif ($run) {
    $j = Invoke-RestMethod -Uri "$base/actions/runs/$($run.id)/jobs" -Headers $hdr -Method Get
    foreach ($s in $j.jobs[0].steps) { "  {0,-24} {1}/{2}" -f $s.name, $s.status, $s.conclusion }
    $lf = Join-Path $env:TEMP 'deploy.log'
    Invoke-WebRequest -Uri "$base/actions/jobs/$($j.jobs[0].id)/logs" -Headers $hdr -OutFile $lf -UseBasicParsing
    Get-Content $lf -Encoding UTF8 | Select-String -Pattern 'error:|ERROR|No such|undeclared|expected|##\[error\]' | Select-Object -Last 15 | ForEach-Object { $_.Line -replace '^\S+Z\s*', '' }
} else {
    "no run found for this commit"
}

# ---------- 5. restore private ----------
$null = Invoke-RestMethod -Uri $base -Headers $hdr -Method Patch -Body ([Text.Encoding]::UTF8.GetBytes((@{ private = $true } | ConvertTo-Json)))
"repo restored private"
