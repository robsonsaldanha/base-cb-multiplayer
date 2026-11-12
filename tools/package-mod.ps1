# Empacota o mod cb-mp (layout 2) em dist\cbmp-mod.zip a partir do build do mod (tools\build.ps1 -Target mod).
#   tools\package-mod.ps1 [-BuildDir build\mod] [-Out dist\cbmp-mod.zip]
# Confira o resultado com: node tools/check-mod-zip.mjs dist/cbmp-mod.zip --version <client/VERSION> --protocol 1
param(
    [string]$BuildDir = "build\mod",
    [string]$Out = "dist\cbmp-mod.zip"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
function Resolve-Root([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
$build = Resolve-Root $BuildDir
$zip = Resolve-Root $Out
$ue4ssRoot = Join-Path $root "client\deps\RE-UE4SS"
$pkg = Join-Path $root "client\package"
$protocol = 1
$ue4ssId = "3.0.1+4fb9b615"
$minLauncher = "0.2.0"
$utf8 = New-Object Text.UTF8Encoding $false

function Find-One([string]$name) {
    $hits = @(Get-ChildItem $build -Recurse -Filter $name | Where-Object { $_.FullName -match "Game__Shipping__Win64" })
    if ($hits.Count -ne 1) { throw "esperava 1 '$name' em $build, achei $($hits.Count): $($hits.FullName -join '; ')" }
    return $hits[0].FullName
}

$version = (Get-Content (Join-Path $root "client\VERSION") -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "client/VERSION inválido: '$version'" }
$ue4ss = Find-One "UE4SS.dll"
$loader = Find-One "dwmapi.dll"
$mod = Find-One "cb-mp.dll"

# UE4SS-settings.ini: só as cinco chaves abaixo vão para 0; o resto (inclusive [Hooks]) fica como veio.
$ini = [IO.File]::ReadAllText((Join-Path $ue4ssRoot "assets\UE4SS-settings.ini"))
foreach ($k in "ConsoleEnabled", "GuiConsoleEnabled", "GuiConsoleVisible", "EnableHotReloadSystem", "EnableDumping") {
    $re = "(?m)^([ \t]*$k[ \t]*=[ \t]*)[^\r\n;#]*?([ \t]*(?:[;#][^\r\n]*)?)(?=\r?$)"
    if ($ini -notmatch $re) { throw "chave '$k' não encontrada em UE4SS-settings.ini" }
    $ini = [regex]::Replace($ini, $re, '${1}0${2}')
}

$meta = '{"layout":2,"version":"' + $version + '","protocol":' + $protocol + ',"ue4ss":"' + $ue4ssId + '","minLauncher":"' + $minLauncher + '"}'

$stage = Join-Path ([IO.Path]::GetTempPath()) ("cbmp-mod-stage-" + [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force "$stage\loader", "$stage\ue4ss\Mods\cb-mp\dlls", "$stage\licenses" | Out-Null
    [IO.File]::WriteAllText("$stage\cbmp-mod.json", $meta, $utf8)
    Copy-Item $loader "$stage\loader\dwmapi.dll"
    Copy-Item $ue4ss "$stage\ue4ss\UE4SS.dll"
    [IO.File]::WriteAllText("$stage\ue4ss\UE4SS-settings.ini", $ini, $utf8)
    [IO.File]::WriteAllText("$stage\ue4ss\Mods\mods.txt", "cb-mp : 1`r`nKeybinds : 0`r`n", $utf8)
    [IO.File]::WriteAllBytes("$stage\ue4ss\Mods\cb-mp\enabled.txt", [byte[]]@())
    Copy-Item $mod "$stage\ue4ss\Mods\cb-mp\dlls\main.dll"
    Copy-Item (Join-Path $ue4ssRoot "LICENSE") "$stage\licenses\UE4SS-LICENSE.txt"
    Copy-Item (Join-Path $pkg "THIRD-PARTY-NOTICES.txt") "$stage\licenses\THIRD-PARTY-NOTICES.txt"
    Copy-Item (Join-Path $pkg "LEIA-ME.txt") "$stage\LEIA-ME.txt"

    # Zip com caminhos em "/" (o Compress-Archive do Windows PowerShell 5 grava "\"): monta com System.IO.Compression.
    Add-Type -AssemblyName System.IO.Compression
    New-Item -ItemType Directory -Force (Split-Path -Parent $zip) | Out-Null
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $fs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
    try {
        $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
        try {
            $files = Get-ChildItem $stage -Recurse -File | Sort-Object FullName
            foreach ($f in $files) {
                $name = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
                $entry = $za.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(2026, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $es = $entry.Open()
                try { $in = [IO.File]::OpenRead($f.FullName); try { $in.CopyTo($es) } finally { $in.Dispose() } } finally { $es.Dispose() }
            }
        } finally { $za.Dispose() }
    } finally { $fs.Dispose() }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
$sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
Write-Host "Pacote: $zip"
Write-Host "versão $version, protocolo $protocol, sha256 $sha"
