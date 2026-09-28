@echo off
REM ============================================================================
REM  gt_regress.bat - GNE regression sweep
REM  Runs every historical smoke scene and requires its own PASS marker.
REM  015 must stay green with 011/013/014 alive.
REM  Subroutine-per-scene: no for-variable/percent adjacency, fully portable.
REM ============================================================================
setlocal

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set TMPC=C:\Users\opc\AppData\Local\Temp\opencode
set OUT=%TMPC%\gt_regress.txt
set FAILED=0

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)
if exist "%OUT%" del /q "%OUT%"

call :scene main_007
call :scene main_008
call :scene main_008b
call :scene main_009
call :scene main_010
call :scene main_011
call :scene main_012 XFAIL
call :scene main_013
call :scene main_014
call :scene main_015

echo.
if "%FAILED%"=="0" (
  echo GT_REGRESS: PASS
  exit /b 0
)
echo GT_REGRESS: FAIL
exit /b 1

:scene
set NAME=%1
REM %2 = XFAIL for known WIP milestones that are deferred by Architect decision
REM (012 = occlusion WIP). They are reported, never counted as gate failures.
REM 2026-09-28: fixed XFAIL semantics - a deferred scene no longer
REM resets/erases failures recorded by earlier scenes (KI-013).
set XFAIL=%2
set LOGF=%TMPC%\reg_%NAME%.log
set SIGF=%TMPC%\reg_%NAME%.txt
if exist "%LOGF%" del /q "%LOGF%"
if exist "%SIGF%" del /q "%SIGF%"
echo === %NAME% ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://%NAME%.tscn -- --sigf="%SIGF%" > "%LOGF%" 2>&1
set RC=%errorlevel%
type "%LOGF%" >> "%OUT%"
set HAS=0
set OK=0
findstr /c:"GNE" "%LOGF%" >nul 2>&1
if not errorlevel 1 set HAS=1
findstr /c:"PASS" "%LOGF%" >nul 2>&1
if not errorlevel 1 set OK=1
set BAD=0
if not "%RC%"=="0" set BAD=1
if "%HAS%"=="0" set BAD=1
if "%OK%"=="0" set BAD=1
findstr /c:"ERROR:" "%LOGF%" >nul 2>&1
if not errorlevel 1 set BAD=1
if "%BAD%"=="1" if not "%XFAIL%"=="XFAIL" set FAILED=1
if "%BAD%"=="1" if "%XFAIL%"=="XFAIL" echo     XFAIL (known WIP, deferred - not gating)
if "%BAD%"=="0" if "%XFAIL%"=="XFAIL" echo     UNEXPECTED PASS - XFAIL milestone now passes; promote it to a gate and drop the marker
echo     rc=%RC% marker=%HAS% pass=%OK% bad=%BAD%
exit /b 0
