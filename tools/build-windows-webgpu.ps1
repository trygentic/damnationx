<#
.SYNOPSIS
    Builds and packages the Windows x64 native WebGPU (SDL3 + Dawn/D3D12) client with MSVC.

.DESCRIPTION
    The HD art needs the native WebGPU presenter, and Dawn's D3D12 backend needs MSVC (or clang-cl),
    so this build cannot come from the MinGW Docker cross build (tools/build-windows-docker.sh).
    It produces the same CPack ZIP layout as that build (exe, devilutionx.mpq, mods/hf.mpq,
    LICENSE/, README.txt, release-manifest.ini, Discord and screen reader DLLs) with:
      - a statically linked CRT (/MT) and static SDL3, SDL3_image, SDL3_mixer, libsodium and Dawn,
        so the exe needs no Visual C++ redistributable;
      - Dawn at the pinned DEVILUTIONX_DAWN_REVISION (fetched and built once under the build dir);
      - the Windows SDK's redistributable d3dcompiler_47.dll, which Dawn's D3D12 backend loads from
        the exe folder (CMake/Dawn.cmake).

    Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload and a Windows 10/11 SDK,
    Git, Python 3, and internet access for the first configure (Dawn, SDL3, StormLib/smpq sources).
    smpq is built and installed to %LOCALAPPDATA%\Programs\smpq by tools/build_and_install_smpq.ps1
    if it is not found.

.PARAMETER BuildDir
    Build directory (default: build-windows-webgpu under the source root).

.PARAMETER Jobs
    Parallel compile jobs for the game and the one-time Dawn build (default: 6).

.PARAMETER Commit
    Release commit for the package name and release-manifest.ini. Required when the source tree is
    not a Git checkout (e.g. extracted from `git archive`); defaults to the current Git commit.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build-windows-webgpu.ps1 -Jobs 6
#>
param(
    [string]$BuildDir = "",
    [int]$Jobs = 6,
    [string]$Commit = "",
    [string[]]$ExtraCMakeArgs = @()
)

$ErrorActionPreference = "Stop"
$SourceDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir "build-windows-webgpu" }

# --- MSVC x64 developer environment -------------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022 or Build Tools with the C++ workload." }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "No Visual Studio installation with the x64 C++ tools was found." }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    Write-Host "Using MSVC environment from $vcvars"
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
foreach ($tool in @("cl.exe", "cmake.exe", "ninja.exe", "git.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found in PATH." }
}

# --- smpq (packs devilutionx.mpq and mods/hf.mpq) -----------------------------------------------
$smpqBin = Join-Path $env:LOCALAPPDATA "Programs\smpq\bin"
if (Test-Path (Join-Path $smpqBin "smpq.exe")) { $env:Path = "$smpqBin;$env:Path" }
if (-not (Get-Command smpq -ErrorAction SilentlyContinue)) {
    Write-Host "smpq not found; building it with tools/build_and_install_smpq.ps1"
    & (Join-Path $SourceDir "tools\build_and_install_smpq.ps1")
    $env:Path = "$smpqBin;$env:Path"
}

# --- Configure ----------------------------------------------------------------------------------
$env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
$cmakeArgs = @(
    "-S", $SourceDir, "-B", $BuildDir, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
    # Static CRT for the game, its fetched dependencies and Dawn (CMake/Dawn.cmake forwards it).
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DUSE_SDL3=ON", "-DDEVILUTIONX_WEBGPU=ON", "-DDEVILUTIONX_DAWN_JOBS=$Jobs",
    "-DDEVILUTIONX_SYSTEM_SDL3=OFF", "-DDEVILUTIONX_STATIC_SDL3=ON",
    "-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF", "-DDEVILUTIONX_STATIC_SDL_IMAGE=ON",
    "-DDEVILUTIONX_SYSTEM_SDL_MIXER=OFF", "-DDEVILUTIONX_STATIC_SDL_MIXER=ON",
    "-DDEVILUTIONX_SYSTEM_ZLIB=OFF", "-DDEVILUTIONX_SYSTEM_BZIP2=OFF", "-DDEVILUTIONX_SYSTEM_LUA=OFF",
    "-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF", "-DDEVILUTIONX_STATIC_LIBSODIUM=ON",
    "-DDEVILUTIONX_SYSTEM_LIBPNG=OFF",
    "-DBUILD_TESTING=OFF", "-DCPACK=ON", "-DBUILD_ASSETS_MPQ=ON",
    "-DDISCORD_INTEGRATION=ON", "-DSCREEN_READER_INTEGRATION=ON"
)
# Translations (.gmo in devilutionx.mpq) need gettext's msgfmt/msgmerge; Git for Windows ships them.
# Pass them explicitly rather than putting Git's usr\bin (which has its own link.exe) on PATH.
if (-not (Get-Command msgfmt -ErrorAction SilentlyContinue)) {
    $gitExe = (Get-Command git.exe).Source
    $gitUsrBin = Join-Path (Split-Path (Split-Path $gitExe)) "usr\bin"
    if (Test-Path (Join-Path $gitUsrBin "msgfmt.exe")) {
        $cmakeArgs += @("-DGETTEXT_MSGFMT_EXECUTABLE=$gitUsrBin\msgfmt.exe", "-DGETTEXT_MSGMERGE_EXECUTABLE=$gitUsrBin\msgmerge.exe")
    } else {
        Write-Warning "gettext (msgfmt) not found; the package will have no translations."
    }
}
if ($Commit) {
    $cmakeArgs += @("-DDEVILUTIONX_RELEASE_COMMIT=$Commit", "-DGIT_COMMIT_HASH=$Commit")
}
$cmakeArgs += $ExtraCMakeArgs

Write-Host "cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

# --- Build and package --------------------------------------------------------------------------
& cmake --build $BuildDir --target package --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build or packaging failed." }

Get-ChildItem -Path $BuildDir -Filter "*.zip" | Where-Object { $_.Name -notlike "_CPack*" } |
    ForEach-Object { Write-Host "Package: $($_.FullName) ($([math]::Round($_.Length / 1MB, 1)) MB)" }
