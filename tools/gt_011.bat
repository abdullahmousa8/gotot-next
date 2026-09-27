@echo off
REM ============================================================================
REM  gt_011.bat - GOTOT-011 3-strategy runner (before / after measurement + DET)
REM
REM  Usage:  gt_011.bat <state> [maxsec]        state = before | after
REM          gt_011.bat after 120
REM          gt_011.bat before 600
REM
REM  Runs main_011 once per strategy (--strategy=0|1|2) through
REM  harnesses\run_scene.ps1 and prints, per strategy:
REM    rc, the "finalize_ms=<n>" timing line (Commit-2 instrumentation) and the
REM    "GOTOT-NEXT 011-DET <sig>" line.
REM
REM  R2 (evidence collision): every state/strategy gets its OWN log, sig and png
REM  path, so before- and after-measurements cannot overwrite each other, and the
REM  scene's own default outputs (main_011.gd:51-52, a fixed machine path) are
REM  overridden via --sigf= / --png= (the only tokens main_011.gd understands,
REM  see main_011.gd:168-173 - note: "--png", NOT "--pngf").
REM
REM  Exit: 0 all three strategies green, 1 otherwise.
REM ============================================================================
setlocal EnableDelayedExpansion

set "STATE=%~1"
if "%STATE%"=="" set "STATE=after"
set "MAXSEC=%~2"
if "%MAXSEC%"=="" set "MAXSEC=120"
set "RSP=%~dp0harnesses\run_scene.ps1"
set "OUTDIR=%TEMP%\gt011"
if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>&1
if not exist "%RSP%" (
  echo [FATAL] runner missing: %RSP%
  exit /b 90
)

set /a BAD=0
for %%S in (0 1 2) do call :one %%S

echo.
echo === gt_011 summary: state=%STATE% maxsec=%MAXSEC% failed=%BAD%/3 ===
if "%BAD%"=="0" (
  echo GT_011: PASS - 3 strategies green - state=%STATE%
  exit /b 0
)
echo GT_011: FAIL - state=%STATE%
exit /b 1

:one
set "S=%~1"
set "LOG=%OUTDIR%\gt011_%STATE%_s%S%.log"
set "SIG=%OUTDIR%\gt011_%STATE%_s%S%.sig"
set "PNG=%OUTDIR%\gt011_%STATE%_s%S%.png"
del /q "%LOG%" "%SIG%" "%PNG%" >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -File "%RSP%" -Scene main_011 -Log "%LOG%" -MaxSec %MAXSEC% -UserArgs "-- --strategy=%S% --sigf=%SIG% --png=%PNG%"
set "RC=!errorlevel!"

set "FMS="
findstr /c:"finalize_ms=" "%LOG%" > "%OUTDIR%\_fms.tmp" 2>nul
if exist "%OUTDIR%\_fms.tmp" set /p FMS=<"%OUTDIR%\_fms.tmp"
set "SIGLINE="
findstr /c:"011-DET" "%LOG%" > "%OUTDIR%\_sig.tmp" 2>nul
if exist "%OUTDIR%\_sig.tmp" set /p SIGLINE=<"%OUTDIR%\_sig.tmp"

echo   strategy=%S% rc=!RC!
if not "!FMS!"=="" echo     !FMS!
if not "!SIGLINE!"=="" echo     !SIGLINE!
if not "!RC!"=="0" set /a BAD+=1
goto :eof
