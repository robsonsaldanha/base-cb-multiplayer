@echo off
rem Kit de reconhecimento CB Multiplayer: COLETAR (veja LEIA-ME.md). A logica fica em ferramentas\cbmp-recon.ps1.
setlocal
cd /d "%~dp0"
if not exist "ferramentas\cbmp-recon.ps1" (
  echo ERRO: extraia o zip inteiro antes de rodar este arquivo.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ferramentas\cbmp-recon.ps1" -Acao Coletar
exit /b %ERRORLEVEL%
