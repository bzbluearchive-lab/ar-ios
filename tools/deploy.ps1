# deploy.ps1 - push whole tree, build on GitHub Actions, download ipas to Desktop
#
# ASCII only; Windows PowerShell 5.1 reads .ps1 as ANSI and non-ASCII can break parsing.
#
# TOKEN HANDLING (learned the hard way):
#   The token is NEVER stored in this repo. It is read from, in order:
#     1. -Token parameter
#     2. $env:AR_GH_TOKEN
#     3. %USERPROFILE%\.ar_gh_token
#   Committing a token to a public repo makes GitHub secret-scanning revoke it instantly.
#
# REPO NOTES:
#   - private repos on this account have no macOS minutes left, so the repo is flipped
#     public for the build (public repos get free runners).
#   - the repo also carries other workflows (build-esp / build-probe / ...), so runs are
#     matched by workflow name "build" AND our commit sha.
#
# FAILURE MODES THIS VERSION FIXES (all hit for real):
#   1. Queue took longer than the old 10 min window, script re-privatized the repo, and the
#      still-queued run could never start (no minutes on private) -> stuck forever.
#      Fix: try/finally + if the run is still queued at timeout, keep the repo public and
#      do NOT cancel our run.
#   2. Duplicate queued runs from earlier dispatches held the concurrency slot.
#      Fix: cancel every queued/in_progress run of workflow "build" before dispatching.

param(
    [string]$Message = "update",
    [string]$Token,
    [string]$RepoName = "ar-ios",
    [string]$Owner,
    [int]$TimeoutMinutes = 45,
    [switch]$NoBuild,
    [switch]$Private
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

function Api([string]$method, [string]$uri, $bodyObj = $null) {
    for ($try = 1; $try -le 3; $try++) {
        try {
            if ($bodyObj -ne $null) {
                $json = $bodyObj | ConvertTo-Json -Depth 8
                return Invoke-RestMethod -Uri $uri -Headers $hdr -Method $method `
                       -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 60
            }
            return Invoke-RestMethod -Uri $uri -Headers $hdr -Method $method -TimeoutSec 60
        } catch {
            $code = 0
            if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
            if ($code -eq 401) { throw "token invalid (401)" }
            if ($code -ge 400 -and $code -lt 500) { throw }
            if ($try -eq 3) { throw }
            Start-Sleep -Seconds 5
        }
    }
}

$me = Api 'Get' "$api/user"
$login = if ($Owner) { $Owner } else { $me.login }
$base = "$api/repos/$login/$RepoName"
"login=$login repo=$RepoName"

# ---------- 1. push whole tree as one commit ----------
$exclude = '(^|\\)(build|\.theos|build-logs|\.git|out)(\\|$)'
$files = Get-ChildItem $root -Recurse -File -Force |
    Where-Object { $_.FullName.Replace($root + '\', '') -notmatch $exclude } |
    ForEach-Object { [pscustomobject]@{ Rel = ($_.FullName.Replace($root + '\', '') -replace '\\', '/'); Full = $_.FullName } } |
    Sort-Object Rel

$ref = Api 'Get' "$base/git/ref/heads/main"
$headSha = $ref.object.sha
$headCommit = Api 'Get' "$base/git/commits/$headSha"

$entries = @()
$n = 0
foreach ($f in $files) {
    $n++
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($f.Full))
    $blob = Api 'Post' "$base/git/blobs" @{ content = $b64; encoding = 'base64' }
    $entries += @{ path = $f.Rel; mode = '100644'; type = 'blob'; sha = $blob.sha }
    if ($n % 25 -eq 0 -or $n -eq $files.Count) { "  uploaded $n/$($files.Count)" }
}
$tree = Api 'Post' "$base/git/trees" @{ base_tree = $headCommit.tree.sha; tree = $entries }
$nc = Api 'Post' "$base/git/commits" @{ message = $Message; tree = $tree.sha; parents = @($headSha) }
$null = Api 'Patch' "$base/git/refs/heads/main" @{ sha = $nc.sha }
"pushed $($nc.sha.Substring(0,8)) ($($files.Count) files)"

if ($NoBuild) { return }

$run = $null
$restored = $false
function Restore-Private {
    if ($script:restored) { return }
    if (-not $Private) { "repo left PUBLIC (pass -Private to flip it back)"; $script:restored = $true; return }
    try {
        $null = Api 'Patch' $base @{ private = $true }
        "repo restored private"
    } catch { "WARN: could not restore private: $($_.Exception.Message)" }
    $script:restored = $true
}

try {
    # ---------- 2. public + cancel stale runs + dispatch ----------
    $null = Api 'Patch' $base @{ private = $false }
    "repo set public (free runners)"

    $recent = Api 'Get' "$base/actions/runs?per_page=30"
    $stale = $recent.workflow_runs | Where-Object {
        $_.name -eq 'build' -and ($_.status -eq 'queued' -or $_.status -eq 'in_progress')
    }
    foreach ($r in $stale) {
        try { $null = Api 'Post' "$base/actions/runs/$($r.id)/cancel"; "cancelled stale run #$($r.run_number)" }
        catch { "cancel #$($r.run_number) rejected (409 = already finishing)" }
    }

    $wfs = Api 'Get' "$base/actions/workflows"
    $bw = $wfs.workflows | Where-Object { $_.path -eq '.github/workflows/build.yml' }
    $null = Api 'Post' "$base/actions/workflows/$($bw.id)/dispatches" @{ ref = 'main' }
    "dispatched build"

    # ---------- 3. wait (match name AND sha) ----------
    $iterations = [int](($TimeoutMinutes * 60) / 15)
    for ($i = 1; $i -le $iterations; $i++) {
        $runs = Api 'Get' "$base/actions/runs?per_page=10"
        $run = $runs.workflow_runs | Where-Object { $_.name -eq 'build' -and $_.head_sha -eq $nc.sha } | Select-Object -First 1
        if ($run) {
            "{0,5}s: {1}/{2}" -f ($i*15), $run.status, $run.conclusion
            if ($run.status -eq 'completed') { break }
        } else { "{0,5}s: no run yet" -f ($i*15) }
        Start-Sleep -Seconds 15
    }

    if (-not $run -or $run.status -ne 'completed') {
        "runner queue timeout after $TimeoutMinutes min."
        "repo is LEFT PUBLIC on purpose: a queued job is cancelled instantly if the repo turns private."
        "wait a few minutes, then download the artifact from the run page, or re-run this script."
        $script:restored = $true
        return
    }

    # ---------- 4. fetch or report ----------
    $out = Join-Path $root 'out'
    if ($run.conclusion -eq 'success') {
        $arts = Api 'Get' "$base/actions/runs/$($run.id)/artifacts"
        if (-not $arts.artifacts -or $arts.artifacts.Count -eq 0) { throw "run succeeded but has no artifacts" }
        if (Test-Path $out) { Remove-Item $out -Recurse -Force }
        New-Item -ItemType Directory -Path $out | Out-Null
        $zip = Join-Path $out 'artifact.zip'
        Invoke-WebRequest -Uri $arts.artifacts[0].archive_download_url -Headers $hdr `
                          -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $out -Force
        Get-ChildItem "C:\Users\92072\Desktop" -Filter 'ARHost*.ipa' -ErrorAction SilentlyContinue | Remove-Item -Force
        Get-ChildItem $out -Filter '*.ipa' | Sort-Object Name | ForEach-Object {
            Copy-Item $_.FullName (Join-Path "C:\Users\92072\Desktop" $_.Name) -Force
            "  $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB"
        }
        "copied to Desktop"
    } else {
        "conclusion=$($run.conclusion)"
        $jobs = Api 'Get' "$base/actions/runs/$($run.id)/jobs"
        foreach ($st in $jobs.jobs[0].steps) { "  {0,-24} {1}/{2}" -f $st.name, $st.status, $st.conclusion }
        try {
            $lf = Join-Path $env:TEMP 'deploy.log'
            Invoke-WebRequest -Uri "$base/actions/jobs/$($jobs.jobs[0].id)/logs" -Headers $hdr `
                              -OutFile $lf -UseBasicParsing
            Get-Content $lf -Encoding UTF8 |
                Select-String -Pattern 'error:|ERROR|No such|undeclared|##\[error\]' |
                Select-Object -Last 15 | ForEach-Object { $_.Line -replace '^\S+Z\s*', '' }
        } catch {
            "job log not available yet (BlobNotFound) - open the run page in a browser"
        }
    }
}
finally {
    Restore-Private
}
"done. commit=$($nc.sha.Substring(0,8)) run=$($run.run_number)"
