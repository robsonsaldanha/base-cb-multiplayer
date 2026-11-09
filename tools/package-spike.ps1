$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\mod"
$assets = Join-Path $root "client\deps\RE-UE4SS\assets"
$pkg = Join-Path $root "package"
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\cb-mp-spike.zip"

function Find-One([string]$name) {
    $hits = Get-ChildItem $build -Recurse -Filter $name | Where-Object { $_.FullName -match "Game__Shipping__Win64" }
    if ($hits.Count -ne 1) { throw "esperava 1 '$name' em $build, achei $($hits.Count): $($hits.FullName -join '; ')" }
    return $hits[0].FullName
}

$ue4ss = Find-One "UE4SS.dll"
$proxy = Find-One "dwmapi.dll"
$mod = Find-One "cb-mp.dll"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$p = Join-Path $stage "payload"
New-Item -ItemType Directory -Force "$p\ue4ss\Mods\cb-mp\dlls" | Out-Null
Copy-Item $proxy "$p\dwmapi.dll"
Copy-Item $ue4ss "$p\ue4ss\UE4SS.dll"
$ini = [IO.File]::ReadAllText((Join-Path $assets "UE4SS-settings.ini"))
foreach ($k in "GuiConsoleEnabled", "GuiConsoleVisible") {
    $re = "(?m)^(\s*$k\s*=\s*)\S+"
    if ($ini -notmatch $re) { throw "chave '$k' nao encontrada em UE4SS-settings.ini" }
    $ini = [regex]::Replace($ini, $re, '${1}0')
}
[IO.File]::WriteAllText("$p\ue4ss\UE4SS-settings.ini", $ini, (New-Object Text.UTF8Encoding $false))
Copy-Item (Join-Path $pkg "mods.txt") "$p\ue4ss\Mods\mods.txt"
Copy-Item $mod "$p\ue4ss\Mods\cb-mp\dlls\main.dll"
New-Item -ItemType File "$p\ue4ss\Mods\cb-mp\enabled.txt" | Out-Null
Copy-Item (Join-Path $pkg "INSTALAR.bat"), (Join-Path $pkg "DESINSTALAR.bat"), (Join-Path $pkg "LEIA-ME.txt") $stage

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Pacote: $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
