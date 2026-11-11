# Compila lua.exe e luac.exe (Lua 5.4.7) a partir do LuaRaw vendorizado no RE-UE4SS (o mesmo Lua que roda os mods
# no jogo), em build\lua54\. Só ferramenta de desenvolvimento: checar sintaxe e rodar o teste simulado do cb-recon
# (tools/test/cb-recon-mock.lua). Precisa do cl.exe: rode dentro do "Developer PowerShell" do Visual Studio
#   & 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$lua = Join-Path $root "client\deps\RE-UE4SS\deps\first\LuaRaw"
$out = Join-Path $root "build\lua54"
if (-not (Test-Path "$lua\src\lapi.c")) { throw "LuaRaw não encontrado em $lua (rode tools\fetch-ue4ss.ps1)" }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe não encontrado: abra o Developer PowerShell do VS" }
New-Item -ItemType Directory -Force "$out\obj" | Out-Null

# Mesmos fontes do LuaRaw/CMakeLists.txt (inclusive luauser.c: o luaconf.h do UE4SS define lua_lock -> LuaLock).
$core = Get-ChildItem "$lua\src\*.c" | Where-Object { $_.Name -notin @("lua.c", "luac.c") } | ForEach-Object { $_.FullName }
Push-Location "$out\obj"
try {
    foreach ($exe in "lua", "luac") {
        & cl.exe /nologo /O2 /W3 /MT /DLUA_COMPAT_5_3 /I "$lua\include" /Fe"$out\$exe.exe" $core "$lua\src\$exe.c"
        if ($LASTEXITCODE) { throw "falhou ao compilar $exe.exe" }
    }
} finally { Pop-Location }
& "$out\lua.exe" -v
