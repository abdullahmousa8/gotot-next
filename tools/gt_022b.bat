@echo off
REM ============================================================================
REM  gt_022b.bat - GNE-022b city-scale GI integration standing gate
REM  TWO runs (d1/d2) of main_022b; requires INTEGRATION PASS with HDR I1
REM  evidence AND byte-equal signatures across runs (DET).
REM
REM  The criteria live in main_022b.gd ONLY (float math in GDScript):
REM    I1    : umbra HDR gain > measured floor
REM    floors: repeat (same-frame re-read) and neg (same draw, GI off) must BOTH
REM            be exactly 0.0 - they are controls, not tolerances.
REM    det   : two GI-on draws byte-equal
REM  This bat asserts markers + DET and echoes the raw evidence lines. It does
REM  NOT re-implement the numeric criterion, so the threshold cannot drift
REM  between the scene and the harness (the perf020 lesson).
REM
REM  On the signature: it carries float deltas and NO wall-clock timing, so it
REM  is compared d1-vs-d2 here. Once it has been stable across independent
REM  sessions it may be promoted to a literal in tools/verify_baseline.txt;
REM  until then this bat is what keeps it honest.
REM ============================================================================
setlocal EnableDelayedExpansion

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF1=C:\Users\opc\AppData\Local\Temp\opencode\gt022b_sig1.txt
set SIGF2=C:\Users\opc\AppData\Local\Temp\opencode\gt022b_sig2.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt022b_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

if exist "%SIGF1%" del /q "%SIGF1%"
if exist "%SIGF2%" del /q "%SIGF2%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-022b RUN 1 (d1) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_022b.tscn -- --sigf="%SIGF1%" >> "%OUT%" 2>&1
set RC1=%errorlevel%
set SIG1=
if exist "%SIGF1%" set /p SIG1=<"%SIGF1%"

echo === GNE-022b RUN 2 (d2) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_022b.tscn -- --sigf="%SIGF2%" >> "%OUT%" 2>&1
set RC2=%errorlevel%
set SIG2=
if exist "%SIGF2%" set /p SIG2=<"%SIGF2%"

echo.
echo --- rc1=%RC1% rc2=%RC2% ---
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

findstr /c:"S22B: PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no PASS marker in output
  set FAILED=1
)
findstr /c:"S22B: FAIL" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] FAIL marker present in output
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
findstr /c:"S22B: GI umbra" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no I1 HDR evidence line in output
  set FAILED=1
)
findstr /c:"S22B: NEG floors" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no NEG/repeat floor control line in output
  set FAILED=1
)

echo --- evidence (fresh log):
for /f "delims=" %%L in ('findstr /c:"S22B: NEG floors" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"S22B: GI umbra" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"S22B: GI lit" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"S22B: I1(umbra indirect)" "%OUT%"') do echo --- %%L

echo.
if "%FAILED%"=="0" (
  echo GT_022B: PASS
  exit /b 0
) else (
  echo GT_022B: FAIL
  exit /b 1
)
