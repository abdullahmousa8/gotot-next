# GNE Continuous Verification System - v1 (workflow v2)
# One command: build -> gt_regress -> literal-signature gates -> GI checks ->
# error scan -> summary + history row. Exit 0 only when everything passes.
param(
  [switch]$SkipBuild
)
$ErrorActionPreference = 'Continue'
$root = 'C:\Users\opc\Documents\AI_ENGINE\godot-next-engine'
$master = 'C:\Users\opc\Documents\AI_ENGINE\godot-master'
$exe = Join-Path $master 'bin\godot.windows.editor.dev.x86_64.console.exe'
$proj = Join-Path $root 'demo\gpu_smoke'
$logDir = Join-Path $env:TEMP 'opencode\gne_verify'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$script:overall = $true
$script:lines = @()
function Record($name, $ok, $detail) {
  $script:lines += ('{0,-14} {1}  {2}' -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $detail)
  if (-not $ok) { $script:overall = $false }
}
if (-not $SkipBuild) {
  Set-Location $master
  scons platform=windows target=editor dev_build=yes custom_modules="$root\modules" -j6 > (Join-Path $logDir 'build.log') 2>&1
  $ok = ($LASTEXITCODE -eq 0)
  Record 'build' $ok ('rc=' + $LASTEXITCODE)
  if (-not $ok) { $script:lines | ForEach-Object { Write-Output $_ }; Write-Output 'GNE_VERIFY: FAIL'; exit 1 }
}
# regress sweep
& (Join-Path $root 'tools\gt_regress.bat') > (Join-Path $logDir 'regress.log') 2>&1
$ok = ((Select-String -LiteralPath (Join-Path $logDir 'regress.log') -Pattern 'GT_REGRESS: PASS' | Measure-Object).Count -ge 1)
Record 'gt_regress' $ok ''
# literal gates
$baseline = @{}
foreach ($ln in [System.IO.File]::ReadAllLines((Join-Path $root 'tools\verify_baseline.txt'))) {
  if ($ln -match '^([^#][^=]*)=(.+)$') { $baseline[$Matches[1].Trim()] = $Matches[2].Trim() }
}
foreach ($g in @('gt_016a','gt_017a','gt_018a','gt_018a_rev','gt_019a','gt_020a','gt_021a')) {
  $lf = Join-Path $logDir ($g + '.log')
  & (Join-Path $root ('tools\' + $g + '.bat')) > $lf 2>&1
  $m = Select-String -LiteralPath $lf -Pattern 'sig d1: "([^"]+)"' | Select-Object -Last 1
  $sig = ''
  if ($m) { $sig = $m.Matches[0].Groups[1].Value }
  $exp = $baseline[$g]
  $ok = ($sig -ne '' -and $sig -eq $exp)
  Record $g $ok ('sig=' + $sig)
}
# GI checks (token-based)
& $exe --path $proj --rendering-method forward_plus res://main_022_gate2.tscn > (Join-Path $logDir 'gi_gate2.log') 2>&1
$g2 = Join-Path $logDir 'gi_gate2.log'
$ok = (((Select-String -LiteralPath $g2 -Pattern 'GATE2: NEG PASS' | Measure-Object).Count -ge 1) -and ((Select-String -LiteralPath $g2 -Pattern 'GATE2: POS PASS' | Measure-Object).Count -ge 1))
Record 'gi_gate2' $ok ''
& $exe --path $proj --rendering-method forward_plus res://main_022_shade.tscn > (Join-Path $logDir 'gi_shade.log') 2>&1
$ok = ((Select-String -LiteralPath (Join-Path $logDir 'gi_shade.log') -Pattern 'SHADE: GATE2 PASS' | Measure-Object).Count -ge 1)
Record 'gi_shade' $ok ''
# error scan
$errs = @()
Get-ChildItem (Join-Path $logDir '*.log') | ForEach-Object {
  $hit = Select-String -LiteralPath $_.FullName -Pattern 'ERROR:|invalid ID' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' }
  if ($hit) { $errs += $hit }
}
Record 'no_errors' ($errs.Count -eq 0) ($errs.Count.ToString() + ' error lines')
# summary
Write-Output '==== GNE VERIFY ===='
$script:lines | ForEach-Object { Write-Output $_ }
if ($script:overall) { Write-Output 'GNE_VERIFY: PASS' } else { Write-Output 'GNE_VERIFY: FAIL' }
# history
$commit = (& git -C $root rev-parse --short HEAD 2>$null)
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
$compact = (($script:lines | ForEach-Object { $p = ($_ -split '\s+'); $p[0] + ':' + $p[1] }) -join ';')
$row = ($stamp + "`t" + $commit + "`t" + $(if ($script:overall) { 'PASS' } else { 'FAIL' }) + "`t" + $compact)
[System.IO.File]::AppendAllText((Join-Path $root 'tools\verify_history.tsv'), ($row + "`n"), (New-Object System.Text.UTF8Encoding($false)))
if ($script:overall) { exit 0 } else { exit 1 }