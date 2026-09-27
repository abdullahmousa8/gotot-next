@echo off
REM ============================================================================
REM  gt_det.bat - N-run determinism runner   (ADDITIVE / new file)
REM
REM  Why this exists: a single passing run cannot validate a fix for a flaky
REM  (nondeterministic) defect - e.g. the GOTOT-014 add/remove race, which was
REM  observed to fail ~25% of the time with -4/-8 lost instances. Proving the
REM  wave fix requires REPEATED runs, so repetition is a first-class tool here.
REM
REM  Usage:  gt_det.bat <scene> <N>      e.g.  gt_det.bat main_014 23
REM  Requires for every run: rc == 0, a "PASS" marker, no "ERROR:", and the
REM  first "sig=" line identical to run 1. Prints the first "active="-bearing
REM  line of each run when the scene emits one (evidence value).
REM  Exit: 0 = all runs identical and green, 1 = any deviation, 90 = no engine.
REM ============================================================================
setlocal EnableDelayedExpansion

set "GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe"
set "PROJ=C:\Users\opc\Documents\AI_ENGINE\gotot-next\demo\gpu_smoke"
set "SCENE=%~1"
set "N=%~2"
if "%SCENE%"=="" set "SCENE=main_014"
if "%N%"=="" set "N=5"

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

set "TMPC=%TEMP%\gt_det"
if not exist "%TMPC%" mkdir "%TMPC%" >nul 2>&1

set "FIRST="
set "FIRSTLOG="
set /a OK=0
set /a BAD=0

for /l %%I in (1,1,%N%) do (
  set "LOG=%TMPC%\%SCENE%_r%%I.log"
  del /q "!LOG!" >nul 2>&1
  "%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://%SCENE%.tscn > "!LOG!" 2>&1
  set "RC=!errorlevel!"
  findstr /c:"sig=" "!LOG!" > "%TMPC%\s.tmp" 2>nul
  set "S="
  if exist "%TMPC%\s.tmp" set /p S=<"%TMPC%\s.tmp"
  findstr /c:"active=" "!LOG!" > "%TMPC%\a.tmp" 2>nul
  set "AL="
  if exist "%TMPC%\a.tmp" set /p AL=<"%TMPC%\a.tmp"
  set "MK=0"
  findstr /c:"PASS" "!LOG!" >nul 2>&1
  if not errorlevel 1 set "MK=1"
  set "MARK=ok"
  if not "!RC!"=="0" set "MARK=rc-fail"
  if "!MK!"=="0" set "MARK=no-PASS-marker"
  findstr /c:"ERROR:" "!LOG!" >nul 2>&1
  if not errorlevel 1 set "MARK=log-ERROR"
  if "!FIRST!"=="" (
    set "FIRST=!S!"
    set "FIRSTLOG=!LOG!"
  ) else (
    if not "!S!"=="!FIRST!" set "MARK=sig-differs"
  )
  if "!MARK!"=="ok" ( set /a OK+=1 ) else ( set /a BAD+=1 )
  echo run %%I of %N%: rc=!RC! marker=!MK! det=!MARK!
  if not "!S!"=="" echo   !S!
  if not "!AL!"=="" echo   active-line: !AL!
)

echo.
echo --- scene=%SCENE%  runs=%N%  ok=%OK%  bad=%BAD% ---
if "%FIRST%"=="" (
  echo [NOTE] %SCENE% prints no sig= line; DET compared nothing - use gt_harness.bat for it.
) else (
  echo --- reference sig (run 1): %FIRST%
)
if "%BAD%"=="0" (
  echo GT_DET: PASS %OK%/%N%
  exit /b 0
)
echo GT_DET: FAIL (%OK%/%N% clean runs) - logs under %TMPC%
exit /b 1
