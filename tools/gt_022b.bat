@echo off
REM ============================================================================
REM  gt_022b.bat - GNE-022b city-scale GI integration standing gate
REM  Single run of main_022b; requires INTEGRATION PASS with HDR I1 evidence.
REM  Marker checks only (rc / PASS / FAIL / ERROR / RID). The NUMERIC I1
REM  validation (dh_u >= 0.5) lives in main_022b.gd itself (float math in
REM  GDScript); this bat echoes the raw HDR delta lines so a CVS row can
REM  parse them from a fresh log. No literal-sig comparison: the 022b sig
REM  embeds float deltas that legitimately vary with driver/pipeline order.
REM ============================================================================
setlocal EnableDelayedExpansion

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF=C:\Users\opc\AppData\Local\Temp\opencode\gt022b_sig.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt022b_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

if exist "%SIGF%" del /q "%SIGF%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-022b RUN 1 ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_022b.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC=%errorlevel%
if exist "%SIGF%" set /p SIG=<"%SIGF%"

echo.
echo --- rc=%RC% ---
echo --- sig: "%SIG%"

set FAILED=0

if not "%RC%"=="0" set FAILED=1

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

echo --- I1 HDR evidence (fresh log):
for /f "delims=" %%L in ('findstr /c:"S22B: GI umbra" "%OUT%"') do echo --- %%L
for /f "delims=" %%L in ('findstr /c:"S22B: GI lit" "%OUT%"') do echo --- %%L

echo.
if "%FAILED%"=="0" (
  echo GT_022B: PASS
  exit /b 0
) else (
  echo GT_022B: FAIL
  exit /b 1
)