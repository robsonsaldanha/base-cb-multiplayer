param(
    [ValidateSet("tests", "mod")] [string]$Target = "tests"
)
$ErrorActionPreference = "Stop"
$CMAKE = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTEST = Join-Path (Split-Path $CMAKE) "ctest.exe"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "client"

if ($Target -eq "tests") {
    $build = Join-Path $root "build\tests"
    & $CMAKE -S $src -B $build -G "Visual Studio 18 2026" -A x64 -DCBMP_WITH_UE4SS=OFF
    if ($LASTEXITCODE) { throw "configure falhou" }
    & $CMAKE --build $build --config Release
    if ($LASTEXITCODE) { throw "build falhou" }
    & $CTEST --test-dir $build -C Release --output-on-failure
    if ($LASTEXITCODE) { throw "testes falharam" }
} else {
    $build = Join-Path $root "build\mod"
    & $CMAKE -S $src -B $build -G "Visual Studio 18 2026" -A x64 -DCBMP_WITH_UE4SS=ON
    if ($LASTEXITCODE) { throw "configure falhou" }
    # Compila tudo: UE4SS.dll, o proxy dwmapi.dll e cb-mp.dll.
    & $CMAKE --build $build --config Game__Shipping__Win64
    if ($LASTEXITCODE) { throw "build falhou" }
}
