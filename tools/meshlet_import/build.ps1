# Builds the GOTOT meshlet import tool.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$ty = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$cl = Join-Path $ty "bin\Hostx64\x64\cl.exe"
$toolset = Join-Path $ty "include"
$testsdk = "C:\Program Files (x86)\Windows Kits\10"
if (!(Test-Path $cl)) { throw "cl.exe not found: $cl" }
$files = @(
    (Join-Path $root "main.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\allocator.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\clusterizer.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\indexgenerator.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\indexanalyzer.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\indexcodec.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\meshletcodec.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\meshletutils.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\overdrawoptimizer.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\simplifier.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\spatialorder.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\vcacheoptimizer.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\vertexcodec.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\vertexfilter.cpp"),
    (Join-Path $root "third_party\meshoptimizer\src\vfetchoptimizer.cpp")
)
$out = Join-Path $root "meshlet_import.exe"
$vcvars = "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`""
$args = "/nologo /O2 /std:c++17 /EHsc /MT /I `"$root`" /I `"$(Join-Path $root "third_party\meshoptimizer\src")`" "
foreach ($fl in $files) { $args += "`"$fl`" " }
$args += "/Fe:`"$out`""
& cmd /c "call $vcvars >nul 2>&1 && cl $args"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Output "built: $out"