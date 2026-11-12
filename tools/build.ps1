# Build do mod cb-mp (UE4SS.dll, proxy dwmapi.dll e cb-mp.dll) em build\mod, configuração Game__Shipping__Win64.
# Rodar no "Developer PowerShell" do VS 2026 (Launch-VsDevShell.ps1 -Arch amd64) com $env:VCPKG_ROOT definido.
# Os pacotes do vcpkg (manifesto client/vcpkg.json) ficam em -VcpkgInstalled (padrão C:/cbmp-vi-mod): nunca apontar
# para a pasta do build da raiz (C:/cbmp-vi), porque o vcpkg em modo manifesto removeria de lá os pacotes da raiz.
param(
    [ValidateSet("mod")] [string]$Target = "mod",
    [string]$VcpkgInstalled = "C:/cbmp-vi-mod"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "client"
$build = Join-Path $root "build\mod"

$unrealInclude = Join-Path $src "deps\RE-UE4SS\deps\first\Unreal\include"
if (-not (Test-Path -LiteralPath $unrealInclude)) {
    throw "UE4SS não encontrado ($unrealInclude não existe): rode tools/fetch-ue4ss.ps1"
}
# vcpkg: $env:VCPKG_ROOT; se ele não tiver o toolchain, o vcpkg que vem com o VS (o dev shell define VSINSTALLDIR).
$vcpkgRoots = @($env:VCPKG_ROOT)
if ($env:VSINSTALLDIR) { $vcpkgRoots += (Join-Path $env:VSINSTALLDIR "VC\vcpkg") }
$toolchain = $vcpkgRoots | Where-Object { $_ } | ForEach-Object { Join-Path $_ "scripts\buildsystems\vcpkg.cmake" } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $toolchain) {
    throw "toolchain do vcpkg não encontrado (VCPKG_ROOT='$env:VCPKG_ROOT'): defina VCPKG_ROOT ou rode no dev shell do VS"
}
Write-Host "vcpkg: $toolchain"
$normalized = ($VcpkgInstalled -replace '\\', '/').TrimEnd('/')
if ($normalized -ieq 'C:/cbmp-vi') { throw "-VcpkgInstalled não pode ser C:/cbmp-vi (pasta do build da raiz)" }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

# O toolchain só vale na primeira configuração: um build\mod antigo (spike, sem vcpkg) tem de ser apagado antes.
$cache = Join-Path $build "CMakeCache.txt"
if (Test-Path -LiteralPath $cache) {
    $hasToolchain = Select-String -LiteralPath $cache -Pattern '^CMAKE_TOOLCHAIN_FILE:.*vcpkg\.cmake' -Quiet
    if (-not $hasToolchain) { throw "build\mod foi configurado sem o vcpkg (build antigo): apague a pasta $build e rode de novo" }
}

# Ports locais só do mod (client/vcpkg-overlays: GNS com protobuf estático, ver o README de lá).
# VCPKG_MANIFEST_INSTALL=ON explícito: um cache com OFF pularia o "vcpkg install" e o port local nunca entraria (o
# install não refaz nada quando o manifesto e os ports não mudaram).
$overlays = (Join-Path $src "vcpkg-overlays") -replace '\\', '/'
& $cmake -S $src -B $build -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    "-DVCPKG_OVERLAY_PORTS=$overlays" `
    -DVCPKG_MANIFEST_INSTALL=ON `
    "-DVCPKG_INSTALLED_DIR=$normalized"
if ($LASTEXITCODE) { throw "configure falhou" }
# Compila tudo: UE4SS.dll, o proxy dwmapi.dll e cb-mp.dll.
& $cmake --build $build --config Game__Shipping__Win64
if ($LASTEXITCODE) { throw "build falhou" }
