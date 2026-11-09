# Baixa RE-UE4SS no commit fixo. Usa git só como ferramenta de download.
$ErrorActionPreference = "Stop"
$COMMIT = "4fb9b615a24c724041ef23e2c4073f2610c4c6d0"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "client\deps\RE-UE4SS"

# Submódulos usam git@github.com (SSH). Redireciona para HTTPS com o login do gh.
gh auth setup-git
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "url.https://github.com/.insteadOf"
$env:GIT_CONFIG_VALUE_0 = "git@github.com:"

if (-not (Test-Path $dest)) {
    git clone https://github.com/UE4SS-RE/RE-UE4SS.git $dest
    if ($LASTEXITCODE) { throw "clone falhou" }
}
git -C $dest fetch origin $COMMIT
git -C $dest checkout --force $COMMIT
if ($LASTEXITCODE) { throw "checkout falhou" }
git -C $dest submodule update --init --recursive
if ($LASTEXITCODE) { throw "submódulos falharam (vínculo GitHub-Epic feito?)" }
$head = git -C $dest rev-parse HEAD
if ($head -ne $COMMIT) { throw "HEAD $head != $COMMIT" }
Write-Host "RE-UE4SS pronto em $dest ($COMMIT)"
