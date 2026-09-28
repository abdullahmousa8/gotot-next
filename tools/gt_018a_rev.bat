@echo off
REM ============================================================================
REM  gt_018a_rev.bat - GNE-018-rev R1/R7 harness (standalone; no XFAIL machinery)
REM  d1 then d2; both must print "GNE 018-REV: PASS" with byte-identical sig
REM  lines. R7 (cone source age <= 1 frame) is asserted inside the scene.
REM ============================================================================
setlocal

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF=C:\Users\opc\AppData\Local\Temp\opencode\gt018rev_sig.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt018rev_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)
if exist "%SIGF%" del /q "%SIGF%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-018-REV RUN 1 (d1) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_018_rev.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC1=%errorlevel%
if exist "%SIGF%" set /p SIG1=<"%SIGF%"

echo === GNE-018-REV RUN 2 (d2) ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_018_rev.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC2=%errorlevel%
if exist "%SIGF%" set /p SIG2=<"%SIGF%"

echo.
echo --- rc1=%RC1%  rc2=%RC2% ---
echo --- sig d1: "%SIG1%"
echo --- sig d2: "%SIG2%"

set FAILED=0
if not "%RC1%"=="0" set FAILED=1
if not "%RC2%"=="0" set FAILED=1
if "%SIG1%"=="" (echo [FAIL] run1 no sig & set FAILED=1)
if "%SIG2%"=="" (echo [FAIL] run2 no sig & set FAILED=1)
if not "%SIG1%"=="%SIG2%" (echo [FAIL] DET: sig differs & set FAILED=1)
findstr /c:"GNE 018-REV: PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (echo [FAIL] no PASS marker & set FAILED=1)
findstr /c:"GNE 018-REV: FAIL code=" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] FAIL marker present & set FAILED=1)
findstr /c:"ERROR:" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] ERROR: line present & set FAILED=1)
findstr /r /c:"RID.*of type" "%OUT%" >nul 2>&1
if not errorlevel 1 (echo [FAIL] RID cleanup lines present & set FAILED=1)

echo.
if "%FAILED%"=="0" (
  echo GT_018A_REV: PASS
  exit /b 0
) else (
  echo GT_018A_REV: FAIL
  exit /b 1
)
