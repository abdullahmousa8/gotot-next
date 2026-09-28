# =============================================================================
#  run_scene.ps1 - single-scene runner used by gt_harness.bat
#
#  Why a separate script instead of an inline powershell -Command: nested
#  quoting inside a .bat is unreliable, and the naive Start-Process + timed
#  WaitForExit pattern does NOT populate Process.ExitCode when output is
#  redirected. That defect made the harness report rc=0 for main_012, which
#  really exits 123 (caught by using main_012 as a known-FAIL control).
#  This version uses System.Diagnostics.Process directly, so:
#     exit code   = the engine's real exit code
#     91          = run killed after -MaxSec (timeout)
#     92          = exit code unavailable (should not happen)
#
#  Usage: powershell -NoProfile -ExecutionPolicy Bypass -File run_scene.ps1 `
#           -Scene main_014 -Log C:\Temp\gt_harness\main_014_a.log -MaxSec 240
# =============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [Parameter(Mandatory = $true)][string]$Log,
    [int]$MaxSec = 240,
    # Extra engine-user args appended verbatim AFTER the scene URL. Godot splits
    # engine args from user args at "--", so a caller passes e.g.
    #   -UserArgs "-- --strategy=1 --sigf=C:\tmp\s1.txt --png=C:\tmp\s1.png"
    [string]$UserArgs = ''
)

$exe = 'C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe'
$proj = 'C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke'

if (-not (Test-Path $exe)) {
    Write-Output "[FATAL] engine exe missing: $exe"
    exit 90
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
# Note: no inner double quotes needed - neither path contains spaces.
$argline = '--path ' + $proj + ' --rendering-method forward_plus res://' + $Scene + '.tscn'
if ($UserArgs -ne '') { $argline = $argline + ' ' + $UserArgs }
$psi.Arguments = $argline
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
[void]$p.Start()

$outT = $p.StandardOutput.ReadToEndAsync()
$errT = $p.StandardError.ReadToEndAsync()

$finished = $p.WaitForExit($MaxSec * 1000)
if (-not $finished) {
    try { $p.Kill() } catch { }
    $p.WaitForExit()
}

[IO.File]::WriteAllText($Log, $outT.Result)
[IO.File]::WriteAllText($Log + '.err', $errT.Result)

if (-not $finished) {
    Write-Output "[run_scene] TIMEOUT after $MaxSec s: killed pid $($p.Id)"
    exit 91
}
if ($null -eq $p.ExitCode) {
    Write-Output "[run_scene] exit code unavailable for $Scene"
    exit 92
}
exit $p.ExitCode
