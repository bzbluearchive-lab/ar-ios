# dbg-check.ps1 - run check-ipa.ps1 and report the exact failing line
$ErrorActionPreference = 'Stop'
try {
    & "$PSScriptRoot\check-ipa.ps1" -Ipa $args[0] -WorkDir "$env:TEMP\ipa-dbg"
} catch {
    "=== CAUGHT ==="
    "message : $($_.Exception.Message)"
    "line    : $($_.InvocationInfo.ScriptLineNumber)"
    "code    : $($_.InvocationInfo.Line.Trim())"
    "stack   : $($_.ScriptStackTrace)"
}
