@echo off
REM ============================================================================
REM  gt_harness.bat - GNE reproduction harness   (ADDITIVE / new file)
REM
REM  Why this exists: the per-milestone evidence runners quoted in
REM  docs/progress_report.md (gt_smoke, gt_007, gt_008, gt_008b, gt_009,
REM  gt_010a, gt_011a|b|c, gt_013a, gt_014a) were never committed. Only
REM  tools\gt_015a.bat and tools\gt_regress.bat exist in the tree, so the
REM  historical PASS claims - with their DET signatures - were not
REM  reproducible from the repository. This runner closes that gap.
REM
REM  Per scene it does TWO full runs and requires, for both runs:
REM    rc == 0                         both runs
REM    a "PASS" marker                 present
REM    no "ERROR:" line                absent
REM    no RID leak lines (RID.*of type)    absent
REM    the "sig=" line identical       DET (when the scene prints one)
REM  main_007 / main_008 print no sig= line, so they are gated on
REM  rc/marker/ERROR only and reported as det=N/A.
REM
REM  This file does not modify, replace or depend on tools\gt_regress.bat or
REM  tools\gt_015a.bat, and it never commits anything.
REM
REM  Usage:
REM    gt_harness.bat                  all scenes (007..015 + 012 as XFAIL)
REM    gt_harness.bat main_014         one scene
REM    gt_harness.bat main_012 XFAIL   one scene, known WIP (reported, not gated)
REM  Exit: 0 = PASS, 1 = FAIL, 90 = engine missing
REM ============================================================================
setlocal EnableDelayedExpansion

set "GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe"
set "PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke"
if "%GT_HARNESS_TMP%"=="" (set "TMPC=%TEMP%\gt_harness") else (set "TMPC=%GT_HARNESS_TMP%")
if "%GT_HARNESS_MAXSEC%"=="" (set "MAXSEC=240") else (set "MAXSEC=%GT_HARNESS_MAXSEC%")
REM  Optional positional overrides:  gt_harness.bat <scene> [XFAIL] [tmpdir] [maxsec]
REM  (preferred over the env vars: `set VAR=value && ...` silently appends a
REM  trailing space to the value when used inline on a cmd command line.)
if not "%~3"=="" set "TMPC=%~3"
if not "%~4"=="" set "MAXSEC=%~4"
set "REPORT=%TMPC%\gt_harness_report.txt"

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)
if not exist "%TMPC%" mkdir "%TMPC%" >nul 2>&1
if exist "%REPORT%" del /q "%REPORT%"

set /a N_SCENE=0
set /a N_OK=0
set /a N_BAD=0
set /a N_XFAIL=0
set /a N_DET=0
set /a N_TOUT=0

if "%~1"=="" goto :run_all
call :scene "%~1" "%~2"
goto :summary

:run_all
call :scene main_007
call :scene main_008
call :scene main_008b
call :scene main_009
call :scene main_010
call :scene main_011
call :scene main_013
call :scene main_014
call :scene main_015
call :scene main_012 XFAIL
goto :summary

:scene
set "NAME=%~1"
set "XFAIL=%~2"
set /a N_SCENE+=1
set "LOGA=%TMPC%\%NAME%_a.log"
set "LOGB=%TMPC%\%NAME%_b.log"
set "SIGA=%TMPC%\%NAME%_a.sig"
set "SIGB=%TMPC%\%NAME%_b.sig"
del /q "%LOGA%" "%LOGB%" "%SIGA%" "%SIGB%" >nul 2>&1

call :run_one "%NAME%" "%LOGA%"
set "RCA=%RC%"
call :run_one "%NAME%" "%LOGB%"
set "RCB=%RC%"

set "S1="
set "S2="
findstr /c:"sig=" "%LOGA%" > "%SIGA%" 2>nul
findstr /c:"sig=" "%LOGB%" > "%SIGB%" 2>nul
if exist "%SIGA%" set /p S1=<"%SIGA%"
if exist "%SIGB%" set /p S2=<"%SIGB%"

REM --- marker / error audit on run A (run B is the DET replicate) ---
set "MK=0"
findstr /c:"PASS" "%LOGA%" >nul 2>&1
if not errorlevel 1 set "MK=1"
set "BAD=0"
if not "%RCA%"=="0" set "BAD=1"
if not "%RCB%"=="0" set "BAD=1"
if "%MK%"=="0" set "BAD=1"
findstr /c:"ERROR:" "%LOGA%" >nul 2>&1
if not errorlevel 1 set "BAD=1"
findstr /c:"ERROR:" "%LOGB%" >nul 2>&1
if not errorlevel 1 set "BAD=1"
findstr /r /c:"RID.*of type" "%LOGA%" >nul 2>&1
if not errorlevel 1 set "BAD=1"
findstr /r /c:"RID.*of type" "%LOGB%" >nul 2>&1
if not errorlevel 1 set "BAD=1"

REM --- DET: first sig= line must be byte-for-byte identical across runs ---
set "SA=0"
if exist "%SIGA%" for %%F in ("%SIGA%") do set "SA=%%~zF"
set "SB=0"
if exist "%SIGB%" for %%F in ("%SIGB%") do set "SB=%%~zF"
set "DET=N/A"
if not "%SA%"=="0" (
  if "%SA%"=="%SB%" (
    if "!S1!"=="!S2!" ( set "DET=OK" ) else ( set "DET=DIFF" )
  ) else ( set "DET=DIFF" )
)
if "!DET!"=="DIFF" set "BAD=1"
if "!DET!"=="OK" set /a N_DET+=1

REM --- timeout audit: :run_one returns 91 when it killed a run at MAXSEC ---
set "TOUT=0"
if "%RCA%"=="91" set "TOUT=1"
if "%RCB%"=="91" set "TOUT=1"
if "!TOUT!"=="1" set /a N_TOUT+=1

if "!BAD!"=="1" (
  if "!XFAIL!"=="XFAIL" (
    set /a N_XFAIL+=1
    set "RES=XFAIL-known-WIP-reported-not-gating"
  ) else (
    set /a N_BAD+=1
    if "!TOUT!"=="1" (set "RES=TIMEOUT-exceeded-MAXSEC") else (set "RES=BAD")
  )
) else (
  set /a N_OK+=1
  if "!XFAIL!"=="XFAIL" (set "RES=UNEXPECTED-PASS-promote-to-gate") else (set "RES=OK")
)

echo     %NAME%  rc=%RCA%/%RCB%  marker=%MK%  det=%DET%  %RES%
echo     %NAME%  rc=%RCA%/%RCB%  marker=%MK%  det=%DET%  %RES%>>"%REPORT%"
if not "!S1!"=="" (
  echo       sig=!S1!
  echo       sig=!S1!>>"%REPORT%"
)
if "!BAD!"=="1" (
  echo       logs: %LOGA%
  echo       logs: %LOGA%>>"%REPORT%"
)
goto :eof

:summary
echo.
echo --- scenes=%N_SCENE%  ok=%N_OK%  bad=%N_BAD%  xfail=%N_XFAIL%  timeouts=%N_TOUT%  with-DET=%N_DET% ---
echo --- scenes=%N_SCENE%  ok=%N_OK%  bad=%N_BAD%  xfail=%N_XFAIL%  timeouts=%N_TOUT%  with-DET=%N_DET% --->>"%REPORT%"
echo --- logs and per-scene sig files: %TMPC% ---
if "%N_BAD%"=="0" (
  echo GT_HARNESS: PASS
  exit /b 0
)
echo GT_HARNESS: FAIL
exit /b 1

REM ---------------------------------------------------------------------------
REM  :run_one <scene> <log>
REM  Runs one scene through PowerShell so that (a) the child's exit code is
REM  captured and (b) a hard per-run wall-clock limit exists: a scene that does
REM  not terminate within MAXSEC is killed (only THAT pid) and reported as 91.
REM  Motivation (measured, 2026-09-27): main_011 runs 60 full GPU frames and
REM  then scans 1920x1080 readbacks in GDScript inside _finalize(); its wall
REM  time is minutes, so an unbounded runner cannot sweep it safely.
REM  Sets RC (0 ok, 91 timeout, else the engine's exit code).
REM ---------------------------------------------------------------------------
:run_one
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_scene.ps1" -Scene "%~1" -Log "%~2" -MaxSec %MAXSEC%
set "RC=%errorlevel%"
goto :eof
