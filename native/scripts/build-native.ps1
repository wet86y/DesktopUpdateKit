param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$NativeRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path (Split-Path -Parent $NativeRoot) 'build\native'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$VsRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($VsRoot)) { throw 'The MSVC x64 toolchain is not installed.' }
$CMake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$CTest = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $CMake -S $NativeRoot -B $BuildRoot -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMake --build $BuildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CTest --test-dir $BuildRoot -C $Configuration --output-on-failure
exit $LASTEXITCODE
