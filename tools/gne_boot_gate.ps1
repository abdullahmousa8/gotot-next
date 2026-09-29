# GNE-024 module presence boot gate.
#
# WHY: building Godot without `custom_modules` still SUCCEEDS and still
# produces a binary - the gne_render module just silently vanishes, because
# modules/gne_render/config.py has no is_enabled() and the only thing that
# controls presence is custom_modules. The loss only shows up at runtime
# ("Could not find type GneRenderServer"). This gate turns that silent loss
# into a hard CVS failure.
#
# WHAT IT CHECKS, from ONE fresh boot log:
#   1. the log exists and is non-empty
#   2. the real process exit code was obtained and is 0
#   3. the registration marker "[GNE] GneRenderServer initialized." is present
#   4. no SCRIPT ERROR / ERROR: / invalid ID line accompanies it
# All four are required. A missing log, missing probe script, missing marker,
# or an unreliable exit code is a FAIL.
#
# It deliberately does NOT run main_012 and does NOT require GNE 012: PASS.
#
# NOTE Get-BootVerdict is used by BOTH the real run and -SelfTest, so the
# self test exercises the production decision path, not a re-implementation.
param(
  [string]$Exe,
  [string]$Proj,
  [string]$Script = 'res://gne_boot_probe.gd',
  [string]$LogPath,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Continue'

$MarkerPattern = '\[GNE\] GneRenderServer initialized\.'
$ErrPattern    = '(?m)^\s*.*(SCRIPT ERROR|ERROR:|invalid ID)'

# --- the ONE evaluation used by the real run and by -SelfTest ---
function Get-BootVerdict {
  param([int]$ExitCode, [string]$LogPath)

  if (-not (Test-Path $LogPath)) {
    return @{ Pass = $false; Reason = 'no log produced' }
  }
  $text = ''
  try { $text = [System.IO.File]::ReadAllText($LogPath) } catch {
    return @{ Pass = $false; Reason = 'log unreadable' }
  }
  if ($text.Trim().Length -eq 0) {
    return @{ Pass = $false; Reason = 'empty log' }
  }
  if ($ExitCode -ne 0) {
    return @{ Pass = $false; Reason = ('exit=' + $ExitCode) }
  }
  $m = ([regex]::Matches($text, $MarkerPattern)).Count
  if ($m -lt 1) {
    return @{ Pass = $false; Reason = 'registration marker missing' }
  }
  $e = ([regex]::Matches($text, $ErrPattern)).Count
  if ($e -gt 0) {
    return @{ Pass = $false; Reason = ('boot errors in log: ' + $e) }
  }
  return @{ Pass = $true; Reason = ('marker ok, exit=0, log=' + (Split-Path $LogPath -Leaf)) }
}

# --- -SelfTest: drive Get-BootVerdict with synthetic evidence ---
if ($SelfTest) {
  $td = Join-Path $env:TEMP 'gne_boot_selftest'
  if (-not (Test-Path $td)) { New-Item -ItemType Directory -Path $td -Force | Out-Null }

  $okLog   = Join-Path $td 'ok.log'
  $noMark  = Join-Path $td 'nomarker.log'
  $withErr = Join-Path $td 'witherr.log'
  $empty   = Join-Path $td 'empty.log'
  $missing = Join-Path $td 'does_not_exist.log'

  $u8 = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($okLog,
    "Godot Engine v4.8.dev.custom_build`r`n[GNE] GneRender initialized.`r`n[GNE] GneRenderServer initialized.`r`n", $u8)
  [System.IO.File]::WriteAllText($noMark,
    "Godot Engine v4.8.dev.custom_build`r`n[GNE] GneRender initialized.`r`n", $u8)
  [System.IO.File]::WriteAllText($withErr,
    "[GNE] GneRenderServer initialized.`r`nSCRIPT ERROR: Parse Error: Could not find type GneRenderServer`r`n", $u8)
  [System.IO.File]::WriteAllText($empty, '', $u8)
  if (Test-Path $missing) { Remove-Item $missing -Force }

  $cases = @(
    @{ Name = 'healthy log, rc=0';                Exit = 0; Log = $okLog;   Want = $true  },
    @{ Name = 'marker missing, rc=0';             Exit = 0; Log = $noMark;  Want = $false },
    @{ Name = 'marker present, rc!=0';            Exit = 1; Log = $okLog;   Want = $false },
    @{ Name = 'marker present with SCRIPT ERROR'; Exit = 0; Log = $withErr; Want = $false },
    @{ Name = 'log missing';                      Exit = 0; Log = $missing; Want = $false },
    @{ Name = 'empty log';                        Exit = 0; Log = $empty;   Want = $false }
  )

  $failed = 0
  foreach ($c in $cases) {
    $v = Get-BootVerdict -ExitCode $c.Exit -LogPath $c.Log
    $ok = ($v.Pass -eq $c.Want)
    if (-not $ok) { $failed++ }
    $verdict = $(if ($ok) { 'OK' } else { 'MISMATCH' })
    Write-Output ('SELFTEST ' + $verdict.PadRight(8) + ' expect=' + $c.Want.ToString().PadRight(5) +
      ' got=' + $v.Pass.ToString().PadRight(5) + ' :: ' + $c.Name + '  [' + $v.Reason + ']')
  }

  Remove-Item $okLog, $noMark, $withErr, $empty -Force -ErrorAction SilentlyContinue
  Write-Output ('SELFTEST: ' + $(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }) +
    ' (' + ($cases.Count - $failed) + '/' + $cases.Count + ')')
  if ($failed -eq 0) { exit 0 } else { exit 1 }
}



# --- normal run: one fresh boot on the real binary ---
if (-not $Exe -or -not $Proj) {
  Write-Output 'MODULE_BOOT: FAIL (missing -Exe or -Proj)'
  exit 1
}
if (-not (Test-Path $Exe)) {
  Write-Output ('MODULE_BOOT: FAIL (engine exe missing: ' + $Exe + ')')
  exit 1
}
$probeFile = Join-Path $Proj ($Script -replace '^res://', '')
if (-not (Test-Path $probeFile)) {
  Write-Output ('MODULE_BOOT: FAIL (probe script missing: ' + $probeFile + ')')
  exit 1
}
if (-not $LogPath) {
  $LogPath = Join-Path $env:TEMP 'opencode\gne_verify\module_boot.log'
}

# fresh log every run: a previous log must never be able to satisfy the gate
$errPath = $LogPath + '.err'
foreach ($p in @($LogPath, $errPath)) {
  if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
}

$exitCode = -1
$reliable = $false
try {
  # Start-Process -Wait -PassThru yields a trustworthy ExitCode even for a
  # GUI-subsystem binary, where `&` does NOT wait and reports nothing useful.
  $proc = Start-Process -FilePath $Exe `
    -ArgumentList @('--headless', '--path', $Proj, '--check-only', '--script', $Script) `
    -RedirectStandardOutput $LogPath -RedirectStandardError $errPath `
    -NoNewWindow -Wait -PassThru
  if ($proc -ne $null -and $proc.ExitCode -ne $null) {
    $exitCode = [int]$proc.ExitCode
    $reliable = $true
  }
} catch {
  Write-Output ('MODULE_BOOT: FAIL (could not start engine: ' + $_.Exception.Message + ')')
  exit 1
}

# merge stderr so the marker/error scan sees the complete boot log
if (Test-Path $errPath) {
  try {
    $et = [System.IO.File]::ReadAllText($errPath)
    if ($et.Length -gt 0) {
      [System.IO.File]::AppendAllText($LogPath, $et, (New-Object System.Text.UTF8Encoding($false)))
    }
  } catch { }
  Remove-Item $errPath -Force -ErrorAction SilentlyContinue
}

if (-not $reliable) {
  Write-Output 'MODULE_BOOT: FAIL (no reliable exit code)'
  exit 1
}

$v = Get-BootVerdict -ExitCode $exitCode -LogPath $LogPath
Write-Output ('MODULE_BOOT: ' + $(if ($v.Pass) { 'PASS' } else { 'FAIL' }) + ' :: ' + $v.Reason)
if ($v.Pass) { exit 0 } else { exit 1 }
