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
# --- binary provenance stamp (GNE-025) -------------------------------------
# WHY: the engine binary carries no build identity. godot-master has no commits
# (0 refs), so methods.py:181-206 leaves git_hash="" and the generated
# core/version_hash.gen.cpp holds GODOT_VERSION_HASH="" / TIMESTAMP 0. Nothing
# inside the exe can therefore be read back to attribute it to a tree. A stamp
# written next to the exe after every build is NOT enough on its own: when
# scons ends "up to date" no new binary is produced, yet a fresh stamp would
# still be written and would falsely certify a stale binary.
#
# The gate closes exactly that hole: the stamp is REFRESHED ONLY IF the exe
# content actually changed across the scons invocation (SHA-256 before/after).
# If scons relinked nothing, the previous stamp is kept and validated as-is, so
# a tree change with no relink is detected instead of being certified.
#
# Inputs are hashed, not timestamps: scons uses Decider("MD5-timestamp")
# (SConstruct:584) and a stale "up to date" is precisely the case under test.
$stampFile = Join-Path $master 'bin\gne_provenance.txt'
$modDir = Join-Path $root 'modules\gne_render'
function Get-ExeSha {
  if (-not (Test-Path $exe)) { return '' }
  return (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
}
function Get-TreeHead { return (& git -C $root rev-parse HEAD 2>$null | Select-Object -First 1) }
# Dirty state is scoped to the compiled module: tools/ and demo/ changes do not
# alter the binary, so treating them as dirty would fail CVS on its own output
# (verify_history.tsv is appended by this very script).
function Get-ModuleState {
  $dirty = @(git -C $root status --porcelain -- modules/gne_render 2>$null)
  $acc = New-Object System.Text.StringBuilder
  Get-ChildItem -LiteralPath $modDir -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.hpp', '.c', '.py') } |
    Sort-Object FullName | ForEach-Object {
      $rel = $_.FullName.Substring($modDir.Length + 1).Replace('\', '/')
      # Hash NORMALIZED text, not raw bytes. core.autocrlf=true rewrites LF->CRLF
      # on checkout, so byte hashing reports a content change for a file git
      # considers clean - which would make this gate fire on its own repo ops.
      $raw = [System.IO.File]::ReadAllText($_.FullName)
      $norm = $raw.Replace("`r`n", "`n").Replace("`r", "`n")
      $bytes = [System.Text.Encoding]::UTF8.GetBytes($norm)
      $sha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
      [void]$acc.Append($rel + '=' + [System.BitConverter]::ToString($sha).Replace('-', '') + "`n")
    }
  return @{ Digest = (Get-FileHash -InputStream ([System.IO.MemoryStream]::new([System.Text.Encoding]::UTF8.GetBytes($acc.ToString()))) -Algorithm SHA256).Hash
            Dirty  = $dirty.Count }
}
function Read-Stamp {
  if (-not (Test-Path $stampFile)) { return $null }
  $kv = @{}
  foreach ($ln in [System.IO.File]::ReadAllLines($stampFile)) {
    # Keys contain digits (exe_sha256), so the class must admit them; a
    # narrower pattern would silently drop keys and make the gate vacuous.
    if ($ln -match '^([a-z0-9_]+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] }
  }
  if ($kv.Count -eq 0) { return $null }
  return $kv
}
$exeBefore = Get-ExeSha
$relinked = $false
if (-not $SkipBuild) {
  Set-Location $master
  scons platform=windows target=editor dev_build=yes custom_modules="$root\modules" -j6 > (Join-Path $logDir 'build.log') 2>&1
  $ok = ($LASTEXITCODE -eq 0)
  Record 'build' $ok ('rc=' + $LASTEXITCODE)
  if (-not $ok) { $script:lines | ForEach-Object { Write-Output $_ }; Write-Output 'GNE_VERIFY: FAIL'; exit 1 }
  $exeAfter = Get-ExeSha
  # A real relink is detected by content change, never by mtime or exit code.
  $relinked = ($exeAfter -ne '' -and $exeAfter -ne $exeBefore)
  if ($relinked) {
    $st = Get-ModuleState
    $stamp = @(
      'exe_sha256=' + $exeAfter
      'tree_head=' + (Get-TreeHead)
      'module_digest=' + $st.Digest
      'module_dirty=' + $st.Dirty
      'flags=platform=windows,target=editor,dev_build=yes,custom_modules=modules/gne_render'
    ) -join "`n"
    [System.IO.File]::WriteAllText($stampFile, $stamp + "`n")
    Write-Output ('GNE_STAMP: RELINK exe=' + $exeAfter.Substring(0, 12) + ' tree=' + (Get-TreeHead).Substring(0, 7) + ' module=' + $st.Digest.Substring(0, 12))
  } else {
    Write-Output ('GNE_STAMP: NO_RELINK exe=' + $(if ($exeAfter) { $exeAfter.Substring(0, 12) } else { 'missing' }) + ' (stamp not refreshed)')
  }
}
# Provenance gate: the stamp must exist, describe THIS binary, and describe the
# CURRENT tree and module content. Any mismatch exits non-zero before the sweep.
$stamp = Read-Stamp
$exeNow = Get-ExeSha
$stNow = Get-ModuleState
$headNow = Get-TreeHead
$why = @()
if (-not $stamp) {
  $why += 'no provenance stamp (binary never relinked by this tool)'
} else {
  # --- HARD GATE: these describe the BINARY and its build inputs. A mismatch
  # means the exe on disk is not the one the stamp describes. ---
  if ($stamp.exe_sha256 -ne $exeNow) { $why += ('exe changed since stamp: ' + $stamp.exe_sha256.Substring(0, 12) + ' -> ' + $(if ($exeNow) { $exeNow.Substring(0, 12) } else { 'missing' })) }
  if ($stamp.module_digest -ne $stNow.Digest) { $why += ('module source changed since stamp: ' + $stamp.module_digest.Substring(0, 12) + ' -> ' + $stNow.Digest.Substring(0, 12)) }
  if ([int]$stamp.module_dirty -ne 0) { $why += ('module worktree dirty at stamp time: ' + $stamp.module_dirty) }
}
if ($stNow.Dirty -ne 0) { $why += ('module worktree dirty now: ' + $stNow.Dirty) }
# --- INFORMATIONAL: tree_head records WHICH revision was built. HEAD moves on
# every commit, including commits that touch only tools/ or docs/ and cannot
# change the compiled binary, so a difference here is NOT binary drift. It is
# reported, never enforced. Only module_digest + exe_sha256 + module_dirty bind
# the binary to its build inputs. ---
$headMoved = ($stamp -and ($stamp.tree_head -ne $headNow))
$provOk = ($why.Count -eq 0)
$provDetail = if ($provOk) { ('exe=' + $exeNow.Substring(0, 12) + ' module=' + $stNow.Digest.Substring(0, 12) + ' dirty=0 head=' + $headNow.Substring(0, 7) + $(if ($headMoved) { ' (built at ' + $stamp.tree_head.Substring(0, 7) + ', informational)' } else { '' })) }
              else { ($why -join '; ') }
Record 'provenance' $provOk $provDetail
if (-not $provOk) { $script:lines | ForEach-Object { Write-Output $_ }; Write-Output 'GNE_VERIFY: FAIL'; exit 1 }
# module presence boot gate (GNE-024): catches a Godot build made without
# custom_modules, where the gne_render module silently vanishes while the
# build still succeeds. Runs here - after the build, before the long sweep -
# and ALSO under -SkipBuild, so it always validates the binary CVS will use.
# A failure stops CVS immediately rather than after the 10-scene sweep.
$bootLog = Join-Path $logDir 'module_boot.log'
if (Test-Path $bootLog) { Remove-Item $bootLog -Force }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\gne_boot_gate.ps1') `
  -Exe $exe -Proj $proj -LogPath $bootLog > (Join-Path $logDir 'module_boot_gate.log') 2>&1
$bootRc = $LASTEXITCODE
$bootVerdict = Select-String -LiteralPath (Join-Path $logDir 'module_boot_gate.log') `
  -Pattern 'MODULE_BOOT: (PASS|FAIL) :: (.*)' | Select-Object -Last 1
$bootDetail = ''
if ($bootVerdict) { $bootDetail = $bootVerdict.Matches[0].Groups[2].Value.Trim() }
$ok = ($bootRc -eq 0) -and $bootVerdict -and ($bootVerdict.Matches[0].Groups[1].Value -eq 'PASS')
Record 'module_boot' $ok $bootDetail
if (-not $ok) { $script:lines | ForEach-Object { Write-Output $_ }; Write-Output 'GNE_VERIFY: FAIL'; exit 1 }
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
# Material v2 X1 gate (016.5). gt_regress runs main_007..main_015 only and no
# literal gate covers 016.5, so the scene - and the missing-channel X1
# contract it asserts - had no place in CVS: a failing SLICE1 verdict only
# printed and still exited 0. The scene is now fail-closed (exit 41), and this
# step requires the real exit code, the PASS marker, the absence of an explicit
# SLICE1 FAIL, and a clean error scan. The ERROR: filter mirrors the global
# error scan so the PowerShell NativeCommandError wrapper is not miscounted as
# a scene error.
$lf = Join-Path $logDir 'mat_v2_x1.log'
if (Test-Path $lf) { Remove-Item $lf -Force }
& $exe --path $proj --rendering-method forward_plus res://main_016_5.tscn > $lf 2>&1
$x1rc = $LASTEXITCODE
$x1pass = ((Select-String -LiteralPath $lf -Pattern 'GNE 016\.5: SLICE1 PASS' | Measure-Object).Count -ge 1)
$x1fail = ((Select-String -LiteralPath $lf -Pattern 'GNE 016\.5: SLICE1 FAIL' | Measure-Object).Count -ge 1)
$x1err = @()
if (Test-Path $lf) {
  $x1err = @(Select-String -LiteralPath $lf -Pattern 'ERROR:|invalid ID' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' })
}
$xd = ('rc=' + $x1rc + ' pass=' + $x1pass + ' fail=' + $x1fail + ' errs=' + $x1err.Count)
$xm = Select-String -LiteralPath $lf -Pattern 'X1 all-unset body_px=(\d+)/(\d+) diff_px=(\d+) max_lsb=(\d+)' | Select-Object -Last 1
if ($xm) {
  $xd += (' body_px=' + $xm.Matches[0].Groups[1].Value + '/' + $xm.Matches[0].Groups[2].Value + ' diff_px=' + $xm.Matches[0].Groups[3].Value + ' max_lsb=' + $xm.Matches[0].Groups[4].Value)
}
Record 'mat_v2_x1' (($x1rc -eq 0) -and $x1pass -and (-not $x1fail) -and ($x1err.Count -eq 0)) $xd
# Material v2 channel -> clustered-light loop gate (main_016_cluster_channel).
# This is the only CVS row that can catch the material channels ceasing to feed
# the cluster loop, because the 018/019/023 gates measure culling counts, shadow
# counts and GI gain - none of them vary a material channel. The scene isolates
# the cluster term chromatically: the global directional term is nulled by
# pointing the light along +Z (perpendicular to the face normal) and the single
# cluster light is pure red, so a red-only excess over the neutral grey response
# can only come from the cluster loop consuming alb.
# Requires BOTH exit code 0 AND the GATE PASS marker, and rejects GATE FAIL or
# any ERROR: line, so the row is fail-closed rather than print-only.
$lf = Join-Path $logDir 'mat_channel_cluster.log'
if (Test-Path $lf) { Remove-Item $lf -Force }
& $exe --path $proj --rendering-method forward_plus res://main_016_cluster_channel.tscn > $lf 2>&1
$ccrc = $LASTEXITCODE
$ccpass = ((Select-String -LiteralPath $lf -Pattern 'GNE 016\.cclus: GATE PASS' | Measure-Object).Count -ge 1)
$ccfail = ((Select-String -LiteralPath $lf -Pattern 'GNE 016\.cclus: GATE FAIL' | Measure-Object).Count -ge 1)
$ccerr = @()
if (Test-Path $lf) {
  $ccerr = @(Select-String -LiteralPath $lf -Pattern 'ERROR:|invalid ID|SCRIPT ERROR' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' })
}
$ccsig = ''
$ccm = Select-String -LiteralPath $lf -Pattern 'GNE 016\.cclus: sig=(\S+)' | Select-Object -Last 1
if ($ccm) { $ccsig = $ccm.Matches[0].Groups[1].Value }
$ccdetail = ('rc=' + $ccrc + ' pass=' + $ccpass + ' fail=' + $ccfail + ' errs=' + $ccerr.Count)
if ($ccsig -ne '') { $ccdetail += ' sig=' + $ccsig }
Record 'mat_channel_cluster' (($ccrc -eq 0) -and $ccpass -and (-not $ccfail) -and ($ccerr.Count -eq 0)) $ccdetail
# Frustum culling gate (main_018_frustum_cull). The only CVS row that can
# catch a cluster light surviving outside the view frustum, or the dual-
# condition protocol silently degrading: membership (gpu_light_debug_cluster
# must show the id absent from all 24 depth slices) AND contribution (C_R must
# fall to 0). A pixel-only check could not tell culling from distance decay -
# that distinction was refuted in practice when a pixel test passed a rear
# cull whose light was still present in the cluster.
# Requires exit 0 AND the GATE PASS marker; rejects GATE FAIL, ERROR:,
# invalid ID and SCRIPT ERROR. The scene exits 71 when any condition fails.
$lf = Join-Path $logDir 'frustum_cull.log'
if (Test-Path $lf) { Remove-Item $lf -Force }
& $exe --path $proj --rendering-method forward_plus res://main_018_frustum_cull.tscn > $lf 2>&1
$fcrd = $LASTEXITCODE
$fcpass = ((Select-String -LiteralPath $lf -Pattern 'GNE 018\.frus: GATE PASS' | Measure-Object).Count -ge 1)
$fcfail = ((Select-String -LiteralPath $lf -Pattern 'GNE 018\.frus: GATE FAIL' | Measure-Object).Count -ge 1)
$fcerr = @()
if (Test-Path $lf) {
  $fcerr = @(Select-String -LiteralPath $lf -Pattern 'ERROR:|invalid ID|SCRIPT ERROR' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' })
}
$fcdetail = ('rc=' + $fcrd + ' pass=' + $fcpass + ' fail=' + $fcfail + ' errs=' + $fcerr.Count)
$fcm = Select-String -LiteralPath $lf -Pattern 'GNE 018\.frus: sig=(\S+)' | Select-Object -Last 1
if ($fcm) { $fcdetail += ' sig=' + $fcm.Matches[0].Groups[1].Value }
Record 'frustum_cull' (($fcrd -eq 0) -and $fcpass -and (-not $fcfail) -and ($fcerr.Count -eq 0)) $fcdetail
# Cluster light capacity gate (main_019_cluster_overflow). Covers the
# 16-slot cap: the cull appends under `if (slot < 16u)` while the count is an
# uncapped atomicAdd, so this row is the regression guard for the fragment read
# bound that was fixed to j < 16u (a 64 bound on a stride of 16 read past the
# end of the buffer for the last cluster).
#
# --spacing is REQUIRED and is the point of the row. Without it every light
# lands on one point, the flood surface measures 0.000 at N=16, and the run
# would gate on a degenerate all-zero case. With spacing, light 0 stays on the
# surface and C_ref is live (24.951), so the liveness precondition c_ref > 1.0
# actually has teeth: a regression that zeroed the cluster path would drop
# C_ref to 0 and fail here. The signature records the measured cap value so the
# co-located collapse stays visible in logs even though it is not gated.
$lf = Join-Path $logDir 'cluster_overflow.log'
if (Test-Path $lf) { Remove-Item $lf -Force }
& $exe --path $proj --rendering-method forward_plus res://main_019_cluster_overflow.tscn -- '--spacing' > $lf 2>&1
$corc = $LASTEXITCODE
$copass = ((Select-String -LiteralPath $lf -Pattern 'GNE 019\.ovf: GATE PASS' | Measure-Object).Count -ge 1)
$cofail = ((Select-String -LiteralPath $lf -Pattern 'GNE 019\.ovf: GATE FAIL' | Measure-Object).Count -ge 1)
$coerr = @()
if (Test-Path $lf) {
  $coerr = @(Select-String -LiteralPath $lf -Pattern 'ERROR:|invalid ID|SCRIPT ERROR' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' })
}
$codetail = ('rc=' + $corc + ' pass=' + $copass + ' fail=' + $cofail + ' errs=' + $coerr.Count)
$com = Select-String -LiteralPath $lf -Pattern 'GNE 019\.ovf: sig=(\S+)' | Select-Object -Last 1
if ($com) { $codetail += ' sig=' + $com.Matches[0].Groups[1].Value }
Record 'cluster_overflow' (($corc -eq 0) -and $copass -and (-not $cofail) -and ($coerr.Count -eq 0)) $codetail
# GI checks (token-based)
& $exe --path $proj --rendering-method forward_plus res://main_022_gate2.tscn > (Join-Path $logDir 'gi_gate2.log') 2>&1
$g2 = Join-Path $logDir 'gi_gate2.log'
$ok = (((Select-String -LiteralPath $g2 -Pattern 'GATE2: NEG PASS' | Measure-Object).Count -ge 1) -and ((Select-String -LiteralPath $g2 -Pattern 'GATE2: POS PASS' | Measure-Object).Count -ge 1))
Record 'gi_gate2' $ok ''
& $exe --path $proj --rendering-method forward_plus res://main_022_shade.tscn > (Join-Path $logDir 'gi_shade.log') 2>&1
$ok = ((Select-String -LiteralPath (Join-Path $logDir 'gi_shade.log') -Pattern 'SHADE: GATE2 PASS' | Measure-Object).Count -ge 1)
Record 'gi_shade' $ok ''
# M1 standing gate (023 HDR): bat markers + numeric validation of the FRESH
# M1 line (h0/h1/gain recompute within 1e-9 absolute ~1e6x above double noise
# at magnitude ~2 and far below the 0.01 signal floor; gain >= 0.01). No
# literal-sig comparison: the 023 sig embeds wall-clock timings by design.
$lf = Join-Path $logDir 'gt_023a.log'
& (Join-Path $root 'tools\gt_023a.bat') > $lf 2>&1
$m1ok = $false
$m1detail = ''
$m1pass = ((Select-String -LiteralPath $lf -Pattern 'GT_023A: PASS' | Measure-Object).Count -ge 1)
$mm = Select-String -LiteralPath $lf -Pattern 'GNE 023: M1 hdr lit h0=([0-9.eE+-]+) h1=([0-9.eE+-]+) gain=([0-9.eE+-]+) sane=(\w+)' | Select-Object -Last 1
if ($m1pass -and $mm) {
  $h0 = [double]$mm.Matches[0].Groups[1].Value
  $h1 = [double]$mm.Matches[0].Groups[2].Value
  $gain = [double]$mm.Matches[0].Groups[3].Value
  $sane = $mm.Matches[0].Groups[4].Value -eq 'True'
  $m1ok = $sane -and ([Math]::Abs($h1 - $h0 - $gain) -le 1e-9) -and ($gain -ge 0.01)
  $m1detail = ('h0=' + $h0 + ' h1=' + $h1 + ' gain=' + $gain)
}
Record 'gt_023a' $m1ok $m1detail
# render regression: golden byte-compare (018 + 019)
foreach ($gs in @(@('main_018','main_018.png'), @('main_019','main_019.png'))) {
  $scene = $gs[0]
  $gold = Join-Path $root ('tools\golden\' + $gs[1])
  $cur = Join-Path $logDir ($scene + '_cur.png')
  if (Test-Path $cur) { Remove-Item $cur }
  & $exe --path $proj --rendering-method forward_plus ("res://" + $scene + ".tscn") -- ("--shot=" + $cur) > (Join-Path $logDir ($scene + '_render.log')) 2>&1
  $ok = $false
  if ((Test-Path $gold) -and (Test-Path $cur)) {
    $h1 = (Get-FileHash $gold -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $cur -Algorithm SHA256).Hash
    $ok = ($h1 -eq $h2)
  }
  Record ('render_' + $scene) $ok ''
}
# error scan
$errs = @()
Get-ChildItem (Join-Path $logDir '*.log') | ForEach-Object {
  $hit = Select-String -LiteralPath $_.FullName -Pattern 'ERROR:|invalid ID' | Where-Object { $_.Line -notmatch 'FullyQualifiedErrorId|NativeCommandError' }
  if ($hit) { $errs += $hit }
}
Record 'no_errors' ($errs.Count -eq 0) ($errs.Count.ToString() + ' error lines')
# perf 020 regression gate (contract 20_perf) --------------------------------
# WHY: gt_020a runs the presentation scene twice (d1, d2) and prints
# wall_avg_us per pass. That average spans the whole cold run with first-frame
# shader and pipeline warmup included, so the only defensible threshold form is
# an absolute ceiling - a per-frame p50 gate would need p50 stability proven
# across machines first, which is not yet done.
#
# The 31 historical samples are deliberately NOT the basis of this threshold.
# They spread 6.9x (avg min 16125, max 110977, peak max 533645) and are
# left-skewed, so any percentile of that population is meaningless. Three
# samples 35 minutes apart on different commits read 24747 / 110977 / 40617,
# which is host contamination, not a code change.
#
# The basis is a controlled quiet-host measurement of 10 samples (5 runs x 2
# passes, 2026-09-30): min 26703, median 28846, mean 28873, max 30679 us, a
# 1.15x spread. FAIL sits at 40000 = 1.30x above that max, which clears the
# 1.70x spread of wall_peak_us but stays far below the 110977 us a contaminated
# host produced. Gating uses the WORSE pass, not the last one, so a regression
# in either d1 or d2 is caught. wall_peak_us is recorded but never gated: on its
# own it spreads 1.70x.
#
# KNOWN LIMIT (not a defect, but a fact to keep): the threshold is relative to
# this machine and this scene. Slower hardware will read a false alarm, and a
# genuinely correct optimisation would be indistinguishable from noise at this
# sample size. Re-baseline rather than widen if that happens.
$perf = ''
$lf20 = Join-Path $logDir 'gt_020a.log'
$m20 = Select-String -LiteralPath $lf20 -Pattern 'wall_avg_us=(\d+) wall_peak_us=(\d+)' | Select-Object -Last 1
if ($m20) { $perf = ('020:' + $m20.Matches[0].Groups[1].Value + '/' + $m20.Matches[0].Groups[2].Value) }
$avgAll = @()
foreach ($mm in (Select-String -LiteralPath $lf20 -Pattern 'wall_avg_us=(\d+)')) { $avgAll += [int]$mm.Matches[0].Groups[1].Value }
# The threshold is only meaningful for the binary it was measured on. A rebuild
# changes exe_sha256, and every historical row becomes incomparable - yet the
# constant below would keep judging the new binary against the old one's
# numbers, silently. The same reasoning as the provenance stamp above: a gate
# that cannot detect that its own basis is void is not a gate. So the basis is
# pinned to a file, and a mismatch BLOCKS instead of warning. Re-baseline by
# re-measuring on quiet hardware and rewriting the file - never by widening.
$p020base = @{}
$pb = Join-Path $root 'tools\perf020_baseline.txt'
if (Test-Path -LiteralPath $pb) {
  foreach ($ln in [System.IO.File]::ReadAllLines($pb)) {
    if ($ln -match '^\s*([a-z_0-9]+)\s*=\s*(.+?)\s*$') { $p020base[$Matches[1]] = $Matches[2] }
  }
}
$basisExe = if ($p020base.ContainsKey('exe_sha256')) { $p020base['exe_sha256'].ToUpper() } else { '' }
$basisOk = ($basisExe -ne '') -and ($exeNow -eq $basisExe)
if ($avgAll.Count -gt 0) {
  $w020 = ($avgAll | Measure-Object -Maximum).Maximum
  # WARN is a severity label INSIDE failure, not a pass. The first cut tested
  # -ne 'FAIL' on a chain that marked >40000 as FAIL, so 110977 - the exact
  # contaminated-host value used above to justify the ceiling - landed on WARN
  # and passed the build silently. Ordering the elseif the other way is the
  # only reading consistent with the rationale above.
  $p020 = if ($w020 -gt 46000) { 'WARN' } elseif ($w020 -gt 40000) { 'FAIL' } else { 'PASS' }
  if (-not $basisOk) {
    Record 'perf020' $false ('STALE BASIS: threshold was measured on exe=' + $(if ($basisExe) { $basisExe.Substring(0, 12) } else { '<none recorded>' }) + ' but this binary is ' + $(if ($exeNow) { $exeNow.Substring(0, 12) } else { 'missing' }) + '. The reading ' + $w020 + 'us is NOT comparable - re-measure and rewrite tools/perf020_baseline.txt. Do not widen the threshold.')
  } else {
    Record 'perf020' ($p020 -eq 'PASS') ($w020.ToString() + 'us worst of ' + $avgAll.Count + ' [FAIL>40000 WARN>46000, basis exe=' + $exeNow.Substring(0, 12) + ']')
  }
} else {
  Record 'perf020' $false 'no wall_avg_us in gt_020a.log'
}
# summary
Write-Output '==== GNE VERIFY ===='
$script:lines | ForEach-Object { Write-Output $_ }
if ($perf -ne '') { Write-Output ('perf ' + $perf) }
if ($script:overall) { Write-Output 'GNE_VERIFY: PASS' } else { Write-Output 'GNE_VERIFY: FAIL' }
# history
$commit = (& git -C $root rev-parse --short HEAD 2>$null)
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
$compact = (($script:lines | ForEach-Object { $p = ($_ -split '\s+'); $p[0] + ':' + $p[1] }) -join ';')
$row = ($stamp + "`t" + $commit + "`t" + $(if ($script:overall) { 'PASS' } else { 'FAIL' }) + "`t" + $compact + "`t" + $perf)
[System.IO.File]::AppendAllText((Join-Path $root 'tools\verify_history.tsv'), ($row + "`n"), (New-Object System.Text.UTF8Encoding($false)))
if ($script:overall) { exit 0 } else { exit 1 }