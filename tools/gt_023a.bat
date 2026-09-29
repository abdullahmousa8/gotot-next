@echo off
REM ============================================================================
REM  gt_023a.bat - GNE-023 GIxShadows + M1 (HDR) standing gate
REM  Single run of main_023; requires INTEGRATION PASS with M1 evidence.
REM  Marker checks only (rc / PASS / FAIL / ERROR / RID). The NUMERIC M1
REM  validation (h0/h1/gain recompute within 1e-9 absolute, gain >= 0.01)
REM  lives in gne_verify.ps1 (batch has no float math); this bat echoes the
REM  raw M1 line so the CVS row parses it from a fresh log. No literal-sig
REM  comparison: the 023 sig embeds wall-clock timings (floats by design).
REM ============================================================================
setlocal EnableDelayedExpansion

set GODOT=C:\Users\opc\Documents\AI_ENGINE\godot-master\bin\godot.windows.editor.dev.x86_64.console.exe
set PROJ=C:\Users\opc\Documents\AI_ENGINE\godot-next-engine\demo\gpu_smoke
set SIGF=C:\Users\opc\AppData\Local\Temp\opencode\gt023_sig.txt
set OUT=C:\Users\opc\AppData\Local\Temp\opencode\gt023_full.txt

if not exist "%GODOT%" (
  echo [FATAL] engine exe missing: %GODOT%
  exit /b 90
)

if exist "%SIGF%" del /q "%SIGF%"
if exist "%OUT%" del /q "%OUT%"

echo === GNE-023 RUN 1 ===
"%GODOT%" --path "%PROJ%" --rendering-method forward_plus res://main_023.tscn -- --sigf="%SIGF%" >> "%OUT%" 2>&1
set RC=%errorlevel%
if exist "%SIGF%" set /p SIG=<"%SIGF%"

echo.
echo --- rc=%RC% ---
echo --- sig: "%SIG%"

set FAILED=0

if not "%RC%"=="0" set FAILED=1

findstr /c:"GNE 023: INTEGRATION PASS" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no INTEGRATION PASS marker in output
  set FAILED=1
)
findstr /c:"GNE 023: FAIL" "%OUT%" >nul 2>&1
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
findstr /c:"GNE 023: M1 hdr lit" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [FAIL] no M1 evidence line in output
  set FAILED=1
)

echo --- M1 evidence (fresh log):
for /f "delims=" %%L in ('findstr /c:"GNE 023: M1 hdr lit" "%OUT%"') do echo --- %%L

echo.
if "%FAILED%"=="0" (
  echo GT_023A: PASS
  exit /b 0
) else (
  echo GT_023A: FAIL
  exit /b 1
)
