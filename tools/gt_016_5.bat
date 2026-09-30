@echo off
REM ============================================================================
REM  gt_016_5.bat - GNE-016.5 SLICE1 blocking gate (standalone wrapper)
REM  The scene is fail-closed: SLICE1 FAIL exits 41, PASS exits 0.
REM  This wrapper requires rc=0 AND the PASS marker AND no FAIL/ERROR/RID.
REM  (gne_verify.ps1 also invokes the scene directly with the same checks.)
REM ============================================================================
setlocal

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt016_5_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)
if exist "%OUT%" del /q "%OUT%"

echo === GNE-016.5 SLICE1 RUN ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_016_5.tscn >> "%OUT%" 2>&1
set RC=%errorlevel%
echo --- rc=%RC% ---

set FAILED=0
if not "%RC%"=="0" set FAILED=1
findstr /c:"GNE 016.5: SLICE1 PASS" "%OUT%" >nul 2>&1
if errorlevel 1 echo [FAIL] no SLICE1 PASS marker
if errorlevel 1 set FAILED=1
findstr /c:"GNE 016.5: SLICE1 FAIL" "%OUT%" >nul 2>&1
if not errorlevel 1 echo [FAIL] SLICE1 FAIL marker present
if not errorlevel 1 set FAILED=1
findstr /c:"ERROR:" "%OUT%" >nul 2>&1
if not errorlevel 1 echo [FAIL] ERROR: line present
if not errorlevel 1 set FAILED=1
findstr /r /c:"RID.*of type" "%OUT%" >nul 2>&1
if not errorlevel 1 echo [FAIL] RID cleanup lines present
if not errorlevel 1 set FAILED=1

echo.
if "%FAILED%"=="0" (
  echo GT_016_5: PASS
  exit /b 0
) else (
  echo GT_016_5: FAIL
  exit /b 1
)
