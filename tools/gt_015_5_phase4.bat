@echo off
REM ============================================================================
REM  gt_015_5_phase4.bat - GOTOT-015.5 Phase 4 frame-time drift runner
REM
REM  Runs the 300-frame measurement (100 warm-up + 200 measured) and prints the
REM  per-pass wall-clock table, the frame totals and the drift. Two runs are
REM  executed so the SIGNATURE can be compared (DET on the drift line).
REM
REM  Usage: tools\gt_015_5_phase4.bat
REM  Exit: 0 = both runs green and the sig lines identical, 1 otherwise.
REM ============================================================================
setlocal EnableDelayedExpansion

set "RSP=%~dp0harnesses\run_scene.ps1"
set "OUTDIR=%TEMP%\gt015_5_phase4"
if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>&1
if not exist "%RSP%" (
  echo [FATAL] runner missing: %RSP%
  exit /b 90
)

set /a FAILED=0
set "SIG1="
set "SIG2="

for %%R in (1 2) do (
  set "LOG=%OUTDIR%\run%%R.txt"
  del /q "!LOG!" >nul 2>&1
  powershell -NoProfile -ExecutionPolicy Bypass -File "%RSP%" -Scene main_015_5_phase4 -Log "!LOG!" -MaxSec 900
  set "RC=!errorlevel!"
  if not "!RC!"=="0" (
    echo     run%%R: FAILED rc=!RC! log=!LOG!
    set /a FAILED+=1
  ) else (
    echo     run%%R: ok log=!LOG!
  )
  findstr /c:"015.5 p4: sig=" "!LOG!" > "%OUTDIR%\s%%R.tmp" 2>nul
  set "SIG%%R="
  if exist "%OUTDIR%\s%%R.tmp" set /p SIG%%R=<"%OUTDIR%\s%%R.tmp"
)

echo.
echo === Phase 4 evidence (run 1) ===
type "%OUTDIR%\run1.txt" | findstr /c:"015.5 p4:"
if "%FAILED%"=="0" (
  echo === DET: comparing signature across the two runs ===
  if "%SIG1%"=="" (
    echo GT_015_5_PHASE4: FAIL - no signature in run 1
    exit /b 1
  )
  if not "%SIG1%"=="%SIG2%" (
    echo GT_015_5_PHASE4: FAIL - signature differs between runs
    echo   run1: %SIG1%
    echo   run2: %SIG2%
    exit /b 1
  )
  echo GT_015_5_PHASE4: PASS
  exit /b 0
)
echo GT_015_5_PHASE4: FAIL - %FAILED%/2 runs failed
exit /b 1
