@echo off
REM ============================================================================
REM  gt_017a.bat - GNE-017 Textures smoke/det runner
REM  Run 1 (d1) then Run 2 (d2); both must print "GNE 017: PASS" and
REM  emit the SAME sig line. Any "FAIL code=" / ERROR / RID cleanup fails the gate.
REM ============================================================================
setlocal EnableDelayedExpansion

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF=C:\Users\opc\AppData\Local\Temp\opencode\gt017_sig.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt017_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

if exist "%SIGF%" del /q "%SIGF%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-017 RUN 1 (d1) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_017.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC1=%errorlevel%
if exist "%SIGF%" set /p SIG1=<"%SIGF%"

echo === GNE-017 RUN 2 (d2) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_017.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC2=%errorlevel%
if exist "%SIGF%" set /p SIG2=<"%SIGF%"

echo.
echo --- rc1=%RC1%  rc2=%RC2% ---
echo --- sig d1: "%SIG1%"
echo --- sig d2: "%SIG2%"

set FAILED=0

if not "%RC1%"=="0" set FAILED=1
if not "%RC2%"=="0" set FAILED=1
if "%SIG1%"=="" (
  echo [FAIL] run1 produced no sig
  set FAILED=1
)
if "%SIG2%"=="" (
  echo [FAIL] run2 produced no sig
  set FAILED=1
)
if not "%SIG1%"=="%SIG2%" (
  echo [FAIL] DET: sig differs between d1 and d2
  set FAILED=1
)

findstr /c:"GNE 017: PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no PASS marker in output
  set FAILED=1
)
findstr /c:"GNE 017: FAIL code=" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] FAIL marker present in output
  set FAILED=1
)
findstr /c:"ERROR:" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] ERROR: line present in output
  set FAILED=1
)
findstr /c:"RID allocations of type" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] RID cleanup lines present
  set FAILED=1
)

echo.
if "%FAILED%"=="0" (
  echo GT_017A: PASS
  exit /b 0
) else (
  echo GT_017A: FAIL
  exit /b 1
)
