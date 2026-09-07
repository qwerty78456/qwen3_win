param([string]$Configuration='Release')
$ErrorActionPreference='Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Install the C++ build tools on the development machine.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$redist = Join-Path $vs 'VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT'
if (-not (Test-Path -LiteralPath (Join-Path $redist 'msvcp140.dll'))) { throw 'Pinned x64 VC runtime 14.51.36231 is missing from the development toolchain.' }
& $cmake -S $root -B (Join-Path $root 'build') -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs" "-DASRWIN_REDIST_DIR=$redist"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build (Join-Path $root 'build') --config $Configuration --parallel 4
exit $LASTEXITCODE
