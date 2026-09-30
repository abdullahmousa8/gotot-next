@echo off
REM ============================================================================
REM  gt_012hzb.bat - GNE-012-HZB clean culling gate (Tier 2, path A, scene-only)
REM  d1 then d2; both must print "GNE 012-HZB: PASS" with byte-identical
REM  integer sigs. The sig is parsed from the FULL LOG (sig d1: "...") so no
REM  sidecar file is trusted. Wall-clock is evidence-only (KI-001).
REM ============================================================================
setlocal

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt012hzb_full.txt
set TMPF=C:\Users\opc\AppData\Local\Temp\opencode\gt012hzb_siglines.txt
set TMPF2=C:\Users\opc\AppData\Local\Temp\opencode\gt012hzb_sigline2.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)
if exist "%OUT%" del /q "%OUT%"

echo === GNE-012-HZB RUN 1 (d1) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_012_hzb.tscn >> "%OUT%" 2>&1
set RC1=%errorlevel%

echo === GNE-012-HZB RUN 2 (d2) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_012_hzb.tscn >> "%OUT%" 2>&1
set RC2=%errorlevel%

echo.
echo --- rc1=%RC1%  rc2=%RC2% ---
set FAILED=0
if not "%RC1%"=="0" set FAILED=1
if not "%RC2%"=="0" set FAILED=1
findstr /c:"sighzb:" "%OUT%" > "%TMPF%"
set L1=
set /p L1=<"%TMPF%"
set L2=
more +1 "%TMPF%" > "%TMPF2%"
set /p L2=<"%TMPF2%"
set L1=%L1:"=%
set L2=%L2:"=%
echo --- sig: "%L1%"
if "%L1%"=="" echo [FAIL] no sig line in log
if "%L1%"=="" set FAILED=1
if not "%L1%"=="%L2%" echo [FAIL] DET: sig differs between runs
if not "%L1%"=="%L2%" set FAILED=1
findstr /c:"GNE 012-HZB: PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (echo [FAIL] no PASS marker & set FAILED=1)
findstr /c:"GNE 012-HZB: FAIL code=" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] FAIL marker present & set FAILED=1)
findstr /c:"ERROR:" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] ERROR: line present & set FAILED=1)
findstr /r /c:"RID.*of type" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] RID cleanup lines present & set FAILED=1)

echo.
if "%FAILED%"=="0" (
  echo GT_012HZB: PASS
  exit /b 0
) else (
  echo GT_012HZB: FAIL
  exit /b 1
)
