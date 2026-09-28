# Builds the GNE texture import tool (offline only).
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
# Pattern: tools/meshlet_import/build.ps1 (same MSVC toolchain).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$godot = "C:\Users\opc\Documents\AI_ENGINE\godot-master"
$bu = Join-Path $godot "thirdparty\basis_universal"
$ty = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$cl = Join-Path $ty "bin\Hostx64\x64\cl.exe"
if (!(Test-Path $cl)) { throw "cl.exe not found: $cl" }
$files = @(
    (Join-Path $root "main.cpp"),
    (Join-Path $root "stubs.cpp"),
    (Join-Path $bu "transcoder\basisu_transcoder.cpp"),
    (Join-Path $bu "encoder\basisu_comp.cpp"),
    (Join-Path $bu "encoder\basisu_frontend.cpp"),
    (Join-Path $bu "encoder\basisu_backend.cpp"),
    (Join-Path $bu "encoder\basisu_basis_file.cpp"),
    (Join-Path $bu "encoder\basisu_enc.cpp"),
    (Join-Path $bu "encoder\basisu_etc.cpp"),
    (Join-Path $bu "encoder\basisu_gpu_texture.cpp"),
    (Join-Path $bu "encoder\basisu_uastc_enc.cpp"),
    (Join-Path $bu "encoder\basisu_kernels_sse.cpp"),
    (Join-Path $bu "encoder\basisu_resampler.cpp"),
    (Join-Path $bu "encoder\basisu_resample_filters.cpp"),
    (Join-Path $bu "encoder\basisu_ssim.cpp"),
    (Join-Path $bu "encoder\basisu_bc7enc.cpp"),
    (Join-Path $bu "encoder\basisu_pvrtc1_4.cpp"),
    (Join-Path $bu "encoder\basisu_astc_hdr_common.cpp"),
    (Join-Path $bu "encoder\basisu_astc_hdr_6x6_enc.cpp"),
    (Join-Path $bu "encoder\basisu_uastc_hdr_4x4_enc.cpp"),
    (Join-Path $bu "encoder\basisu_opencl.cpp"),
    (Join-Path $bu "encoder\jpgd.cpp"),
    (Join-Path $bu "encoder\pvpngreader.cpp"),
    (Join-Path $bu "encoder\3rdparty\android_astc_decomp.cpp")
)
$out = Join-Path $root "tex_import.exe"
$vcvars = "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`""
$args = "/nologo /O1 /std:c++17 /EHsc /MT /I `"$root`" /I `"$(Join-Path $bu "encoder")`" /I `"$(Join-Path $bu "encoder\3rdparty")`" /I `"$(Join-Path $bu "transcoder")`" /I `"$bu`" /I `"$(Join-Path $godot "thirdparty\zstd")`" /I `"$(Join-Path $godot "thirdparty\tinyexr")`" "
foreach ($fl in $files) { $args += "`"$fl`" " }
$args += "/Fe:`"$out`""
& cmd /c "call $vcvars >nul 2>&1 && cl $args"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Output "built: $out"
