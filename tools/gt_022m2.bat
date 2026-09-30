@echo off
REM ============================================================================
REM  gt_022m2.bat - GNE-022 section 11-M2 standing gate: criterion (b-field)
REM  Spec: docs/spec_022_gi_scoping.md section 11-M2.
REM
REM  TWO runs (d1/d2) of main_022_m2; requires the registered criterion verdict
REM  (M2: criterion_b_field=PASS), rc=0 on both runs, and a byte-equal signature.
REM  The criterion itself lives in the scene (float math in GDScript); the value
REM  is pinned as a LITERAL in tools/verify_baseline.txt, exactly like
REM  gt_016a..gt_022b, so no threshold is duplicated in this wrapper.
REM
REM  Section 11 is untouched by this gate: its official state stays FAIL
REM  (criterion b) under the R1 closure wording, and main_022_loop.gd is frozen.
REM ============================================================================
setlocal EnableDelayedExpansion

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF1=C:\Users\opc\AppData\Local\Temp\opencode\gt022m2_sig1.txt
set SIGF2=C:\Users\opc\AppData\Local\Temp\opencode\gt022m2_sig2.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt022m2_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

if exist "%SIGF1%" del /q "%SIGF1%"
if exist "%SIGF2%" del /q "%SIGF2%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-022-M2 RUN 1 (d1) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_022_m2.tscn -- --sigf="%SIGF1%" >> "%OUT%" 2>&1
set RC1=%errorlevel%
set SIG1=
if exist "%SIGF1%" set /p SIG1=<"%SIGF1%"

echo === GNE-022-M2 RUN 2 (d2) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_022_m2.tscn -- --sigf="%SIGF2%" >> "%OUT%" 2>&1
set RC2=%errorlevel%
set SIG2=
if exist "%SIGF2%" set /p SIG2=<"%SIGF2%"

echo.
echo --- rc1=%RC1%  rc2=%RC2% ---
echo --- sig d1: "%SIG1%"
echo --- sig d2: "%SIG2%"

set FAILED=0

if not "%RC1%"=="0" (
  echo [FAIL] run d1 rc=%RC1%
  set FAILED=1
)
if not "%RC2%"=="0" (
  echo [FAIL] run d2 rc=%RC2%
  set FAILED=1
)
if "%SIG1%"=="" (
  echo [FAIL] empty signature d1
  set FAILED=1
)
if "%SIG2%"=="" (
  echo [FAIL] empty signature d2
  set FAILED=1
)
if not "%SIG1%"=="%SIG2%" (
  echo [FAIL] DET: sig differs between d1 and d2
  set FAILED=1
)

findstr /c:"M2: criterion_b_field=PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no b-field PASS verdict in output
  set FAILED=1
)
findstr /c:"M2: criterion_b_field=FAIL" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] b-field FAIL verdict present in output
  set FAILED=1
)
findstr /c:"ERROR:" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] ERROR: line present in output
  set FAILED=1
)
findstr /r /c:"RID.*of type" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] RID cleanup lines present
  set FAILED=1
)
findstr /c:"M2: ratio10_min=" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no ratio evidence line in output
  set FAILED=1
)

echo --- evidence (fresh log):
for /f "delims=" %%L in ('findstr /c:"M2: rule=" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"M2: ratio10_min=" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"M2: intervals_below_rule=" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"M2: criterion_b_field=" "%OUT%"') do echo --- %%L

echo.
if "%FAILED%"=="0" (
  echo GT_022M2: PASS
  exit /b 0
) else (
  echo GT_022M2: FAIL
  exit /b 1
)
