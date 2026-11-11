# Kit de reconhecimento do CB Multiplayer: instalar, coletar e desinstalar (chamado por INSTALAR.bat, COLETAR.bat e
# DESINSTALAR.bat). Compatível com o Windows PowerShell 5.1 que vem no Windows 11. Roteiro: LEIA-ME.md.
#
# O que este script mexe na pasta do jogo (Phoenix\Binaries\Win64):
#   * acrescenta dwmapi.dll e a pasta ue4ss\ (UE4SS + mods cb-mp e cb-recon);
#   * se já existir dwmapi.dll ou ue4ss\, MOVE o original para cbmp-recon-backup\ (mesmo disco: bytes intactos) e
#     guarda o hash de cada arquivo em cbmp-recon-backup\estado.json;
#   * a desinstalação apaga só o que o kit pôs, devolve os originais e confere os hashes.
# Nenhum arquivo original do jogo (exe, .pak, etc.) é aberto para escrita.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet("Instalar", "Desinstalar", "Coletar")][string]$Acao,
    [string]$PastaJogo,          # raiz do jogo ou a própria Phoenix\Binaries\Win64 (pula a detecção)
    [string]$Destino,            # COLETAR: pasta onde gravar o zip (padrão: OneDrive\CasteloBruxoRP\cbmp-dumps ou Área de Trabalho)
    [switch]$SemPerguntas,       # testes/automação: não pergunta nada (assume "sim" nas confirmações seguras)
    [switch]$SemPausa,           # não espera Enter no fim
    [string]$SteamRaiz,          # testes: pasta da Steam no lugar da do registro
    [string]$EpicManifestos,     # testes: pasta de manifestos da Epic no lugar da real
    [switch]$SemAdmin            # testes: nunca pede elevação
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
# Saída redirecionada (testes, logs): UTF-8. No console de verdade o Write-Host já usa Unicode.
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false }

$KitRoot = Split-Path -Parent $PSScriptRoot
$Payload = Join-Path $KitRoot "payload"
$KitJson = Join-Path $KitRoot "kit.json"
$MarcaInstalacao = Join-Path $KitRoot "instalado-em.txt"
$MarcaColeta = Join-Path $KitRoot "ultima-coleta.txt"
$BackupNome = "cbmp-recon-backup"
$Itens = @("dwmapi.dll", "ue4ss")       # o que o kit põe na Win64 (nível de cima)
$SteamAppId = "990080"

function Escrever([string]$msg, [string]$cor = "Gray") { Write-Host $msg -ForegroundColor $cor }
function Ok([string]$msg) { Escrever $msg "Green" }
function Aviso([string]$msg) { Escrever $msg "Yellow" }
function Falha([string]$msg) { throw [System.Exception]::new($msg) }

function Pausar {
    if (-not $SemPausa) { Write-Host ""; [void](Read-Host "Aperte Enter para fechar esta janela") }
}

function Perguntar([string]$pergunta, [bool]$padrao = $true) {
    if ($SemPerguntas) { return $padrao }
    $sufixo = if ($padrao) { "[S/n]" } else { "[s/N]" }
    $r = Read-Host "$pergunta $sufixo"
    if ([string]::IsNullOrWhiteSpace($r)) { return $padrao }
    return $r.Trim().ToLower().StartsWith("s")
}

function Hash([string]$arquivo) { (Get-FileHash -LiteralPath $arquivo -Algorithm SHA256).Hash.ToLower() }

function E-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------------------------------------------
# Localizar o jogo (mesma lógica do launcher, launcher/src-tauri/src/game.rs: Steam pelo registro +
# libraryfolders.vdf + appmanifest_990080.acf; Epic pelos .item em ProgramData)
# ---------------------------------------------------------------------------------------------------------------

function Win64-De([string]$raiz) { Join-Path $raiz "Phoenix\Binaries\Win64" }

function Normalizar-Pasta([string]$p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return $null }
    $p = $p.Trim().Trim('"').TrimEnd('\', '/')
    if ($p -match '(?i)\\Phoenix\\Binaries\\Win64$') { $p = $p.Substring(0, $p.Length - "\Phoenix\Binaries\Win64".Length) }
    elseif ($p -match '(?i)\\Phoenix$') { $p = $p.Substring(0, $p.Length - "\Phoenix".Length) }
    return $p
}

function Jogo-Valido([string]$raiz) {
    if (-not $raiz -or -not [IO.Path]::IsPathRooted($raiz)) { return $false }
    return (Test-Path -LiteralPath (Join-Path (Win64-De $raiz) "HogwartsLegacy.exe") -PathType Leaf)
}

function Ler-Registro([string]$chave, [string]$valor) {
    try { return (Get-ItemProperty -LiteralPath $chave -Name $valor -ErrorAction Stop).$valor } catch { return $null }
}

function Bibliotecas-Steam([string]$raiz) {
    $libs = New-Object System.Collections.Generic.List[string]
    $libs.Add($raiz)
    $vdf = Join-Path $raiz "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        $texto = [IO.File]::ReadAllText($vdf)
        foreach ($m in [regex]::Matches($texto, '"path"\s+"([^"]+)"')) {
            $p = $m.Groups[1].Value.Replace('\\', '\')
            if (-not ($libs | Where-Object { $_ -ieq $p })) { $libs.Add($p) }
        }
    }
    return $libs
}

function Procurar-Steam {
    $raizes = @()
    if ($SteamRaiz) { $raizes = @($SteamRaiz) } else {
        foreach ($r in @((Ler-Registro "HKCU:\Software\Valve\Steam" "SteamPath"),
                (Ler-Registro "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" "InstallPath"))) {
            if ($r) { $raizes += ($r -replace '/', '\') }
        }
    }
    foreach ($raiz in $raizes) {
        foreach ($lib in (Bibliotecas-Steam $raiz)) {
            $acf = Join-Path $lib "steamapps\appmanifest_$SteamAppId.acf"
            if (-not (Test-Path -LiteralPath $acf)) { continue }
            $m = [regex]::Match([IO.File]::ReadAllText($acf), '"installdir"\s+"([^"]+)"')
            if (-not $m.Success) { continue }
            $dir = Join-Path $lib ("steamapps\common\" + $m.Groups[1].Value.Replace('\\', '\'))
            if (Jogo-Valido $dir) { return @{ pasta = $dir; loja = "Steam"; manifesto = $acf } }
        }
    }
    return $null
}

function Nome-Hogwarts([string]$s) { return (($s -replace '[^A-Za-z0-9]', '').ToLower() -eq "hogwartslegacy") }

function Procurar-Epic {
    $pasta = if ($EpicManifestos) { $EpicManifestos } else { Join-Path $env:ProgramData "Epic\EpicGamesLauncher\Data\Manifests" }
    if (-not (Test-Path -LiteralPath $pasta)) { return $null }
    foreach ($item in (Get-ChildItem -LiteralPath $pasta -Filter *.item -File -ErrorAction SilentlyContinue | Select-Object -First 1000)) {
        try { $j = [IO.File]::ReadAllText($item.FullName) | ConvertFrom-Json } catch { continue }
        $nomes = @()
        foreach ($k in "DisplayName", "AppName") { if ($j.PSObject.Properties[$k]) { $nomes += [string]$j.$k } }
        if (-not ($nomes | Where-Object { Nome-Hogwarts $_ })) { continue }
        if (-not $j.PSObject.Properties["InstallLocation"]) { continue }
        $dir = ([string]$j.InstallLocation) -replace '/', '\'
        if (Jogo-Valido $dir) {
            $ver = if ($j.PSObject.Properties["AppVersionString"]) { [string]$j.AppVersionString } else { $null }
            return @{ pasta = $dir; loja = "Epic"; manifesto = $item.FullName; versao = $ver }
        }
    }
    return $null
}

function Pedir-Pasta {
    if ($SemPerguntas) { Falha "pasta do jogo não encontrada (use -PastaJogo)" }
    Escrever ""
    Escrever "Cole o caminho da pasta do Hogwarts Legacy e aperte Enter."
    Escrever "  Steam: biblioteca > botão direito no jogo > Gerenciar > Procurar arquivos locais"
    Escrever "  Epic:  biblioteca > ... no jogo > Gerenciar > ícone de pasta ao lado de Instalação"
    Escrever "  (pode ser a pasta do jogo ou a Phoenix\Binaries\Win64 dentro dela)"
    for ($i = 0; $i -lt 3; $i++) {
        $p = Normalizar-Pasta (Read-Host "Pasta")
        if (Jogo-Valido $p) { return @{ pasta = $p; loja = "manual" } }
        Aviso "Não achei Phoenix\Binaries\Win64\HogwartsLegacy.exe dentro de '$p'. Tente de novo."
    }
    Falha "pasta do jogo não informada"
}

function Encontrar-Jogo([bool]$confirmar) {
    if ($PastaJogo) {
        $p = Normalizar-Pasta $PastaJogo
        if (-not (Jogo-Valido $p)) { Falha "'$PastaJogo' não é a pasta do Hogwarts Legacy (falta Phoenix\Binaries\Win64\HogwartsLegacy.exe)" }
        $loja = "manual"
        if ($p -match '(?i)\\steamapps\\common\\') { $loja = "Steam" } elseif ($p -match '(?i)\\Epic Games\\') { $loja = "Epic" }
        return @{ pasta = $p; loja = $loja }
    }
    $achado = Procurar-Steam
    if (-not $achado) { $achado = Procurar-Epic }
    if ($achado) {
        Escrever "Jogo encontrado ($($achado.loja)): $($achado.pasta)"
        if (-not $confirmar -or (Perguntar "É esta a pasta do jogo?" $true)) { return $achado }
    } else {
        Aviso "Não achei o jogo na Steam nem na Epic automaticamente."
    }
    return Pedir-Pasta
}

function Jogo-Aberto {
    return [bool](Get-Process -Name "HogwartsLegacy" -ErrorAction SilentlyContinue)
}

function Pode-Gravar([string]$pasta) {
    $t = Join-Path $pasta ("cbmp-teste-escrita-" + [guid]::NewGuid().ToString("N") + ".tmp")
    try { [IO.File]::WriteAllText($t, "x"); Remove-Item -LiteralPath $t -Force; return $true } catch { return $false }
}

# Reabre este script como administrador (o UAC pergunta). Devolve $true se reabriu.
function Elevar([string]$win64) {
    if ($SemAdmin -or (E-Admin)) { return $false }
    Aviso ""
    Aviso "Não tenho permissão para gravar em:"
    Aviso "  $win64"
    Aviso "Isso é normal quando o jogo está em C:\Program Files (comum na Epic)."
    if (-not (Perguntar "Posso abrir de novo como administrador? (o Windows vai pedir confirmação)" $true)) { return $false }
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Acao", $Acao,
        "-PastaJogo", "`"$(Split-Path (Split-Path (Split-Path $win64)))`"")
    if ($Destino) { $argv += @("-Destino", "`"$Destino`"") }
    try {
        $p = Start-Process -FilePath "powershell.exe" -ArgumentList $argv -Verb RunAs -Wait -PassThru
        Escrever "A janela de administrador terminou (código $($p.ExitCode))."
        $script:CodigoElevado = $p.ExitCode
        return $true
    } catch {
        Aviso "A permissão de administrador foi recusada."
        return $false
    }
}

# ---------------------------------------------------------------------------------------------------------------
# Conteúdo do kit
# ---------------------------------------------------------------------------------------------------------------

function Ler-Kit {
    if (-not (Test-Path -LiteralPath $KitJson)) { Falha "kit.json não encontrado em $KitRoot (o zip foi extraído inteiro?)" }
    $kit = [IO.File]::ReadAllText($KitJson) | ConvertFrom-Json
    $faltando = @()
    foreach ($f in $kit.payload) {
        $p = Join-Path $Payload ($f.caminho -replace '/', '\')
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $faltando += $f.caminho; continue }
        if ((Hash $p) -ne $f.sha256) { $faltando += "$($f.caminho) (conteúdo diferente)" }
    }
    if ($faltando.Count) { Falha ("o kit está incompleto ou corrompido; extraia o zip de novo. Problemas: " + ($faltando -join ", ")) }
    return $kit
}

function Listar-Arquivos([string]$base) {
    $out = @()
    if (Test-Path -LiteralPath $base -PathType Leaf) {
        $i = Get-Item -LiteralPath $base -Force
        return @(@{ rel = ""; sha256 = (Hash $i.FullName); tamanho = $i.Length })
    }
    foreach ($i in (Get-ChildItem -LiteralPath $base -Recurse -Force -File)) {
        $out += @{ rel = $i.FullName.Substring($base.Length).TrimStart('\'); sha256 = (Hash $i.FullName); tamanho = $i.Length }
    }
    return $out
}

# $p (arquivo ou pasta) tem exatamente os arquivos e hashes do original registrado na instalação?
function Igual-Ao-Original([string]$p, $orig) {
    $esperado = @($orig.arquivos | Where-Object { $_ })
    if ($orig.tipo -eq "arquivo") {
        if (-not (Test-Path -LiteralPath $p -PathType Leaf) -or $esperado.Count -ne 1) { return $false }
        return (Hash $p) -eq $esperado[0].sha256
    }
    if (-not (Test-Path -LiteralPath $p -PathType Container)) { return $false }
    $atuais = @(Get-ChildItem -LiteralPath $p -Recurse -Force -File)
    if ($atuais.Count -ne $esperado.Count) { return $false }
    foreach ($a in $esperado) {
        $f = Join-Path $p $a.rel
        if (-not (Test-Path -LiteralPath $f -PathType Leaf) -or (Hash $f) -ne $a.sha256) { return $false }
    }
    return $true
}

function Ler-Estado([string]$win64) {
    $e = Join-Path $win64 "$BackupNome\estado.json"
    if (-not (Test-Path -LiteralPath $e)) { return $null }
    return [IO.File]::ReadAllText($e) | ConvertFrom-Json
}

function Win64-Instalado {
    # 1) a marca deixada por este kit; 2) -PastaJogo / detecção
    if (-not $PastaJogo -and (Test-Path -LiteralPath $MarcaInstalacao)) {
        $w = ([IO.File]::ReadAllText($MarcaInstalacao)).Trim()
        if ($w -and (Test-Path -LiteralPath (Join-Path $w "$BackupNome\estado.json"))) { return $w }
    }
    $j = Encontrar-Jogo $false
    return (Win64-De $j.pasta)
}

# ---------------------------------------------------------------------------------------------------------------
# INSTALAR
# ---------------------------------------------------------------------------------------------------------------

function Instalar {
    Escrever "== CB Multiplayer: kit de reconhecimento - INSTALAR ==" "Cyan"
    $kit = Ler-Kit
    $jogo = Encontrar-Jogo $true
    $win64 = Win64-De $jogo.pasta
    if (Jogo-Aberto) { Falha "o jogo está aberto. Feche o Hogwarts Legacy e rode de novo." }
    $backup = Join-Path $win64 $BackupNome
    if (Test-Path -LiteralPath $backup) { Falha "o kit já está instalado em $win64. Rode DESINSTALAR.bat antes de instalar de novo." }
    if (Test-Path -LiteralPath (Join-Path $win64 "cbmp-backup")) {
        Falha "o pacote do spike (cb-mp-spike) está instalado nesta pasta. Rode o DESINSTALAR.bat dele (que fica em Phoenix\Binaries\Win64) antes."
    }
    foreach ($outro in "xinput1_3.dll", "UE4SS.dll", "UE4SS-settings.ini") {
        if (Test-Path -LiteralPath (Join-Path $win64 $outro)) {
            Falha "encontrei '$outro' em $win64 (outro UE4SS/mod instalado). Remova esse mod antes; o kit não mistura com outro loader."
        }
    }
    if (-not (Pode-Gravar $win64)) {
        if (Elevar $win64) { return $script:CodigoElevado }
        Falha "sem permissão para gravar em $win64. Clique com o botão direito em INSTALAR.bat > Executar como administrador."
    }

    New-Item -ItemType Directory -Path $backup | Out-Null
    $originais = @()
    $movidos = @()
    $copiados = $false
    try {
        foreach ($nome in $Itens) {
            $p = Join-Path $win64 $nome
            if (Test-Path -LiteralPath $p) {
                $tipo = if (Test-Path -LiteralPath $p -PathType Leaf) { "arquivo" } else { "pasta" }
                $originais += @{ nome = $nome; tipo = $tipo; arquivos = @(Listar-Arquivos $p) }
            }
        }
        $estado = [ordered]@{
            kit = $kit.versao; instaladoEm = (Get-Date).ToString("s"); pastaJogo = $jogo.pasta; win64 = $win64
            loja = $jogo.loja; originais = $originais; adicionados = $Itens
            topo = @(Get-ChildItem -LiteralPath $win64 -Force | Where-Object { $_.Name -ne $BackupNome } | ForEach-Object { $_.Name })
        }
        [IO.File]::WriteAllText((Join-Path $backup "estado.json"), ($estado | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
        foreach ($o in $originais) {
            Move-Item -LiteralPath (Join-Path $win64 $o.nome) -Destination (Join-Path $backup $o.nome)
            $movidos += $o.nome
            Escrever "Original guardado: $($o.nome) -> $BackupNome\$($o.nome)"
        }
        $copiados = $true
        Copy-Item -LiteralPath (Join-Path $Payload "dwmapi.dll") -Destination (Join-Path $win64 "dwmapi.dll")
        Copy-Item -LiteralPath (Join-Path $Payload "ue4ss") -Destination (Join-Path $win64 "ue4ss") -Recurse
        foreach ($f in $kit.payload) {
            $p = Join-Path $win64 ($f.caminho -replace '/', '\')
            if (-not (Test-Path -LiteralPath $p) -or (Hash $p) -ne $f.sha256) { Falha "cópia conferida falhou em $($f.caminho)" }
        }
    } catch {
        $erro = $_.Exception.Message
        Aviso "Falhou: $erro"
        Aviso "Desfazendo..."
        try {
            if ($copiados) {
                foreach ($nome in $Itens) {
                    $p = Join-Path $win64 $nome
                    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
                }
            }
            foreach ($nome in $movidos) { Move-Item -LiteralPath (Join-Path $backup $nome) -Destination (Join-Path $win64 $nome) }
            Remove-Item -LiteralPath (Join-Path $backup "estado.json") -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $backup -Force
            Aviso "Alterações desfeitas; nada ficou instalado."
        } catch {
            Aviso "NÃO consegui desfazer tudo ($($_.Exception.Message)). Seus originais estão em $backup. Feche o jogo e rode DESINSTALAR.bat."
        }
        Falha $erro
    }

    # O jogo roda sem administrador: libera escrita na pasta ue4ss (logs, relatório, dumps) para o grupo Usuários.
    & icacls.exe (Join-Path $win64 "ue4ss") /grant "*S-1-5-32-545:(OI)(CI)M" /T /Q | Out-Null
    if ($LASTEXITCODE -ne 0) { Aviso "Aviso: não consegui liberar escrita na pasta ue4ss para o grupo Usuários (icacls $LASTEXITCODE)." }
    [IO.File]::WriteAllText($MarcaInstalacao, $win64)

    Ok ""
    Ok "Instalado em: $win64"
    Escrever "Próximo passo (veja o LEIA-ME.md): abra o jogo, carregue um save e espere a janela preta do UE4SS"
    Escrever "mostrar 'TUDO PRONTO'. Depois faça o teste do F8, saia do jogo e rode COLETAR.bat."
    return 0
}

# ---------------------------------------------------------------------------------------------------------------
# COLETAR
# ---------------------------------------------------------------------------------------------------------------

function Dentro-De-Repo([string]$pasta) {
    $d = $pasta
    while ($d) {
        if (Test-Path -LiteralPath (Join-Path $d ".git")) { return $true }
        $pai = Split-Path -Parent $d
        if ($pai -eq $d) { break }
        $d = $pai
    }
    return $false
}

function Pasta-Destino {
    if ($Destino) {
        if (Dentro-De-Repo $Destino) { Falha "o destino $Destino fica dentro de um repositório git; os dados são privados. Escolha outra pasta." }
        return $Destino
    }
    $bases = @()
    if ($env:OneDrive) { $bases += $env:OneDrive }
    if ($env:OneDriveConsumer) { $bases += $env:OneDriveConsumer }
    $bases += (Join-Path $env:USERPROFILE "OneDrive")
    foreach ($b in $bases) {
        $cb = Join-Path $b "CasteloBruxoRP"
        if (Test-Path -LiteralPath $cb -PathType Container) {
            $d = Join-Path $cb "cbmp-dumps"
            if (-not (Dentro-De-Repo $d)) { return $d }
        }
    }
    $desk = [Environment]::GetFolderPath("Desktop")
    if (Dentro-De-Repo $desk) { Falha "a Área de Trabalho fica dentro de um repositório git; use COLETAR com outra pasta." }
    return $desk
}

function Copiar-Se-Existe([string]$origem, [string]$destino) {
    if (-not (Test-Path -LiteralPath $origem)) { return $false }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destino) | Out-Null
    Copy-Item -LiteralPath $origem -Destination $destino -Recurse -Force
    return $true
}

function Info-Jogo([string]$win64, $estado) {
    $raiz = Split-Path (Split-Path (Split-Path $win64))
    $info = [ordered]@{ coletadoEm = (Get-Date).ToString("s"); win64 = $win64 }
    foreach ($par in @(@("exe_win64", (Join-Path $win64 "HogwartsLegacy.exe")), @("exe_raiz", (Join-Path $raiz "HogwartsLegacy.exe")))) {
        $p = $par[1]
        if (Test-Path -LiteralPath $p) {
            $i = Get-Item -LiteralPath $p
            $info[$par[0]] = [ordered]@{
                caminho = $p; tamanho = $i.Length; sha256 = (Hash $p); modificado = $i.LastWriteTime.ToString("s")
                versaoArquivo = $i.VersionInfo.FileVersion; versaoProduto = $i.VersionInfo.ProductVersion
            }
        }
    }
    $info.loja = if ($estado) { $estado.loja } else { "?" }
    $acf = Join-Path (Split-Path (Split-Path $raiz)) "appmanifest_$SteamAppId.acf"
    if (Test-Path -LiteralPath $acf) {
        $t = [IO.File]::ReadAllText($acf)
        $info.loja = "Steam"
        foreach ($k in "buildid", "LastUpdated", "TargetBuildID") {
            $m = [regex]::Match($t, "`"$k`"\s+`"([^`"]*)`"")
            if ($m.Success) { $info["steam_$k"] = $m.Groups[1].Value }
        }
    }
    $epic = Procurar-Epic
    if ($epic -and ($epic.pasta.TrimEnd('\') -ieq $raiz.TrimEnd('\'))) { $info.loja = "Epic"; $info.epic_versao = $epic.versao }
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    if ($os) { $info.windows = "$($os.Caption) $($os.Version)" }
    $info.kit = try { ([IO.File]::ReadAllText($KitJson) | ConvertFrom-Json).versao } catch { "?" }
    return $info
}

function Coletar {
    Escrever "== CB Multiplayer: kit de reconhecimento - COLETAR ==" "Cyan"
    $win64 = Win64-Instalado
    $ue = Join-Path $win64 "ue4ss"
    if (-not (Test-Path -LiteralPath $ue)) { Falha "não achei a pasta ue4ss em $win64 (o kit está instalado?)" }
    if (Jogo-Aberto) { Aviso "O jogo ainda está aberto: feche-o antes para os arquivos ficarem completos." ; if (-not (Perguntar "Coletar mesmo assim?" $false)) { return 1 } }
    $estado = Ler-Estado $win64
    $carimbo = (Get-Date).ToString("yyyyMMdd-HHmmss")
    $staging = Join-Path ([IO.Path]::GetTempPath()) "cbmp-recon-coleta-$carimbo"
    $raizZip = Join-Path $staging "cbmp-recon-$carimbo"
    New-Item -ItemType Directory -Force -Path $raizZip | Out-Null
    $lista = New-Object System.Collections.Generic.List[string]
    try {
        # logs e relatórios
        foreach ($rel in "UE4SS.log", "Mods\cb-mp\cb-mp.log", "Mods\cb-recon\cb-recon.log", "Mods\cb-recon\estado.txt",
                "UE4SS_ObjectDump.txt", "UHTHeaderDump", "CXXHeaderDump", "Mods\shared\types") {
            if (Copiar-Se-Existe (Join-Path $ue $rel) (Join-Path $raizZip "ue4ss\$rel")) { $lista.Add("ue4ss\$rel") }
        }
        foreach ($pad in "Mods\cb-recon\relatorio-*.json") {
            foreach ($f in (Get-ChildItem -Path (Join-Path $ue $pad) -File -ErrorAction SilentlyContinue)) {
                Copiar-Se-Existe $f.FullName (Join-Path $raizZip ("ue4ss\" + $f.FullName.Substring($ue.Length + 1))) | Out-Null
                $lista.Add("ue4ss\" + $f.FullName.Substring($ue.Length + 1))
            }
        }
        foreach ($pad in "*.usmap", "*.jmap", "*-ue4ss_actor_data.csv", "*-ue4ss_static_mesh_data.csv") {
            foreach ($f in (Get-ChildItem -LiteralPath $ue -Filter $pad -File -ErrorAction SilentlyContinue)) {
                Copiar-Se-Existe $f.FullName (Join-Path $raizZip "ue4ss\$($f.Name)") | Out-Null; $lista.Add("ue4ss\$($f.Name)")
            }
        }
        # minidumps (UE4SS: ue4ss\crash_*.dmp; cb-mp: Mods\cb-mp\cb-mp-*.dmp) e .dmp soltos na Win64
        foreach ($f in (Get-ChildItem -LiteralPath $ue -Filter *.dmp -File -Recurse -ErrorAction SilentlyContinue)) {
            $rel = $f.FullName.Substring($ue.Length + 1)
            Copiar-Se-Existe $f.FullName (Join-Path $raizZip "ue4ss\$rel") | Out-Null; $lista.Add("ue4ss\$rel")
        }
        foreach ($f in (Get-ChildItem -LiteralPath $win64 -Filter *.dmp -File -ErrorAction SilentlyContinue)) {
            Copiar-Se-Existe $f.FullName (Join-Path $raizZip "win64\$($f.Name)") | Out-Null; $lista.Add("win64\$($f.Name)")
        }
        # log reserva do cb-mp
        $reserva = Join-Path $env:LOCALAPPDATA "CasteloBruxo\cb-mp"
        if (Copiar-Se-Existe $reserva (Join-Path $raizZip "localappdata-cb-mp")) { $lista.Add("localappdata-cb-mp") }
        # logs e relatórios de crash do próprio jogo (só os desta instalação)
        $desde = if ($estado) { [datetime]$estado.instaladoEm } else { (Get-Date).AddDays(-2) }
        $saved = Join-Path $env:LOCALAPPDATA "Hogwarts Legacy\Saved"
        foreach ($f in (Get-ChildItem -LiteralPath (Join-Path $saved "Logs") -Filter *.log -File -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $desde } | Sort-Object LastWriteTime -Descending | Select-Object -First 3)) {
            Copiar-Se-Existe $f.FullName (Join-Path $raizZip "jogo-logs\$($f.Name)") | Out-Null; $lista.Add("jogo-logs\$($f.Name)")
        }
        foreach ($d in (Get-ChildItem -LiteralPath (Join-Path $saved "Crashes") -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $desde } | Sort-Object LastWriteTime -Descending | Select-Object -First 3)) {
            Copiar-Se-Existe $d.FullName (Join-Path $raizZip "jogo-crashes\$($d.Name)") | Out-Null; $lista.Add("jogo-crashes\$($d.Name)")
        }
        # informações do jogo e da instalação
        $info = Info-Jogo $win64 $estado
        if ($estado) { $info.instalacao = $estado }
        $info.arquivos_coletados = $lista
        $info.conteudo_ue4ss = @(Get-ChildItem -LiteralPath $ue -Recurse -File -Force -ErrorAction SilentlyContinue |
            Select-Object -First 5000 | ForEach-Object { "{0}`t{1}" -f $_.FullName.Substring($ue.Length + 1), $_.Length })
        [IO.File]::WriteAllText((Join-Path $raizZip "info-jogo.json"), ($info | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
        [IO.File]::WriteAllText((Join-Path $raizZip "PRIVADO-LEIA.txt"),
            "Dados extraídos do jogo neste PC para desenvolver o CB Multiplayer. PRIVADO: não publicar, não commitar,`r`nnão anexar em issue/PR. Guardar só na pasta cbmp-dumps (fora do repositório).`r`n")

        $destDir = Pasta-Destino
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        $zip = Join-Path $destDir "cbmp-recon-$carimbo.zip"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        Escrever "Compactando (pode levar alguns minutos)..."
        [IO.Compression.ZipFile]::CreateFromDirectory($raizZip, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
    [IO.File]::WriteAllText($MarcaColeta, (Get-Date).ToString("o"))
    $tam = (Get-Item -LiteralPath $zip).Length
    $temRel = [bool]($lista | Where-Object { $_ -like "*relatorio-*.json" })
    $temDump = $lista -contains "ue4ss\UE4SS_ObjectDump.txt"
    Ok ""
    Ok "Coleta pronta:"
    Ok "  $zip"
    Ok ("  tamanho: {0:N1} MB, {1} itens" -f ($tam / 1MB), $lista.Count)
    if (-not $temRel) { Aviso "  ATENÇÃO: nenhum relatorio-*.json (o cb-recon não chegou a achar o jogador?)" }
    if (-not $temDump) { Aviso "  ATENÇÃO: sem UE4SS_ObjectDump.txt (o dump de objetos não rodou ou travou)" }
    Escrever "Este arquivo é PRIVADO: não publique e não coloque dentro do repositório."
    return 0
}

# ---------------------------------------------------------------------------------------------------------------
# DESINSTALAR
# ---------------------------------------------------------------------------------------------------------------

function Desinstalar {
    Escrever "== CB Multiplayer: kit de reconhecimento - DESINSTALAR ==" "Cyan"
    $win64 = Win64-Instalado
    $backup = Join-Path $win64 $BackupNome
    $estado = Ler-Estado $win64
    if (-not $estado) { Ok "Nada para desinstalar em $win64."; return 0 }
    if (Jogo-Aberto) { Falha "o jogo está aberto. Feche o Hogwarts Legacy e rode de novo." }

    # Plano antes de mexer em qualquer coisa (idempotente: pode rodar de novo depois de uma restauração pela metade).
    # Para cada item que o kit põe (dwmapi.dll, ue4ss):
    #   - havia original e ele NÃO está mais no backup: se o que está na Win64 bate com os hashes do original, já foi
    #     devolvido (não mexe); se não bate, para tudo sem apagar nada;
    #   - senão o que está na Win64 é do kit: apaga (dwmapi.dll só se o hash for o do kit) e devolve o original.
    $kit = $null
    try { $kit = [IO.File]::ReadAllText($KitJson) | ConvertFrom-Json } catch { }
    $plano = @{}
    foreach ($nome in $Itens) {
        $orig = @($estado.originais) | Where-Object { $_ -and $_.nome -eq $nome } | Select-Object -First 1
        $de = Join-Path $backup $nome
        $para = Join-Path $win64 $nome
        if ($orig -and -not (Test-Path -LiteralPath $de)) {
            if ((Test-Path -LiteralPath $para) -and (Igual-Ao-Original $para $orig)) { $plano[$nome] = "devolvido"; continue }
            Falha "o original de $nome não está em $backup e o que está em $para não é ele. Nada foi apagado; mande esta mensagem."
        }
        $plano[$nome] = "do-kit"
    }

    # Garante que os resultados foram coletados antes de apagar a pasta ue4ss (se ela ainda é a do kit).
    $ue = Join-Path $win64 "ue4ss"
    $ultimaColeta = if (Test-Path -LiteralPath $MarcaColeta) { [datetime]::Parse(([IO.File]::ReadAllText($MarcaColeta)).Trim()) } else { [datetime]::MinValue }
    $ultimaMudanca = [datetime]::MinValue
    if ($plano["ue4ss"] -eq "do-kit" -and (Test-Path -LiteralPath $ue)) {
        $m = Get-ChildItem -LiteralPath $ue -Recurse -File -Force -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($m) { $ultimaMudanca = $m.LastWriteTime }
    }
    $instalacao = [datetime]$estado.instaladoEm
    if ($ultimaMudanca -gt $instalacao.AddMinutes(1) -and $ultimaColeta -lt $ultimaMudanca) {
        Aviso "Os resultados ainda não foram coletados. Rodando o COLETAR antes de desinstalar..."
        $r = Coletar
        if ($r -ne 0) { Falha "a coleta não terminou; nada foi desinstalado." }
    }

    if (-not (Pode-Gravar $win64)) {
        if (Elevar $win64) { return $script:CodigoElevado }
        Falha "sem permissão para gravar em $win64. Clique com o botão direito em DESINSTALAR.bat > Executar como administrador."
    }

    $sobrou = $false
    $dll = Join-Path $win64 "dwmapi.dll"
    if ($plano["dwmapi.dll"] -eq "do-kit" -and (Test-Path -LiteralPath $dll)) {
        if (-not $kit) { Falha "não consegui ler o kit.json do kit; não apago o dwmapi.dll sem conferir. Extraia o zip do kit de novo no mesmo lugar e rode outra vez." }
        $h = Hash $dll
        $nosso = @($kit.payload | Where-Object { $_.caminho -eq "dwmapi.dll" -and $_.sha256 -eq $h }).Count -gt 0
        if ($nosso) {
            Remove-Item -LiteralPath $dll -Force
        } else {
            $sobras = Join-Path $win64 "cbmp-recon-sobras"
            New-Item -ItemType Directory -Force -Path $sobras | Out-Null
            Move-Item -LiteralPath $dll -Destination (Join-Path $sobras "dwmapi.dll") -Force
            Aviso "dwmapi.dll foi trocado por outro programa depois da instalação; guardei em $sobras."
            $sobrou = $true
        }
    }
    if ($plano["ue4ss"] -eq "do-kit" -and (Test-Path -LiteralPath $ue)) { Remove-Item -LiteralPath $ue -Recurse -Force }
    foreach ($nome in $Itens) {
        if ($plano[$nome] -eq "do-kit" -and (Test-Path -LiteralPath (Join-Path $win64 $nome))) {
            Falha "não consegui remover $nome (arquivo em uso?). Feche o jogo e rode de novo; seus originais continuam em $backup."
        }
    }

    $problemas = @()
    foreach ($o in @($estado.originais)) {
        if (-not $o) { continue }
        $para = Join-Path $win64 $o.nome
        if ($plano[$o.nome] -eq "devolvido") { Escrever "Original já estava devolvido: $($o.nome)"; continue }
        Move-Item -LiteralPath (Join-Path $backup $o.nome) -Destination $para
        if (-not (Igual-Ao-Original $para $o)) { $problemas += $para }
        Escrever "Original devolvido: $($o.nome)"
    }
    if ($problemas.Count) { Falha ("originais devolvidos com diferença: " + ($problemas -join ", ") + ". O estado foi mantido em $backup.") }
    Remove-Item -LiteralPath (Join-Path $backup "estado.json") -Force
    $resto = @(Get-ChildItem -LiteralPath $backup -Force -ErrorAction SilentlyContinue)
    if ($resto.Count -eq 0) { Remove-Item -LiteralPath $backup -Force } else { Aviso "Sobrou algo em $backup; não apaguei."; $sobrou = $true }
    Remove-Item -LiteralPath $MarcaInstalacao -Force -ErrorAction SilentlyContinue

    # Confere o nível de cima da Win64 contra a lista tirada na instalação (o que a loja não olha: arquivos a mais).
    if ($estado.PSObject.Properties["topo"]) {
        $antes = @($estado.topo)
        $agora = @(Get-ChildItem -LiteralPath $win64 -Force | ForEach-Object { $_.Name })
        $novos = @($agora | Where-Object { $antes -notcontains $_ })
        $sumiram = @($antes | Where-Object { $agora -notcontains $_ })
        if ($novos.Count) { Aviso ("Itens a mais na pasta Win64 (não são do kit, ou sobras dele): " + ($novos -join ", ")); $sobrou = $true }
        if ($sumiram.Count) { Aviso ("Itens que existiam na instalação e sumiram: " + ($sumiram -join ", ")); $sobrou = $true }
    }
    Ok ""
    if ($sobrou) {
        Aviso "Desinstalado, mas com os avisos acima: a pasta $win64 NÃO está exatamente como antes. Anote e mande."
        return 0
    }
    Ok "Desinstalado. A pasta $win64 voltou a ser como antes da instalação."
    Escrever "Para ter certeza, use 'Verificar arquivos' na Steam/Epic (veja o LEIA-ME.md)."
    return 0
}

# ---------------------------------------------------------------------------------------------------------------

$codigo = 1
try {
    switch ($Acao) {
        "Instalar" { $codigo = Instalar }
        "Coletar" { $codigo = Coletar }
        "Desinstalar" { $codigo = Desinstalar }
    }
    $codigo = [int]($codigo | Select-Object -Last 1)
} catch {
    Escrever ""
    Escrever ("ERRO: " + $_.Exception.Message) "Red"
    $codigo = 1
}
Pausar
exit $codigo
