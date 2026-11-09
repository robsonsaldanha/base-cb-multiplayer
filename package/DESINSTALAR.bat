@echo off
setlocal
cd /d "%~dp0"
if not exist "cbmp-backup" (
  echo Nada para desinstalar.
  pause
  exit /b 0
)
rem guarda logs e dumps ao lado deste .bat antes de apagar qualquer coisa
rem (ver >nul zera o errorlevel, para nao herdar o do comando anterior quando o arquivo nao existe)
ver >nul
if exist "ue4ss\Mods\cb-mp\cb-mp.log" copy /y "ue4ss\Mods\cb-mp\cb-mp.log" "cb-mp-ultimo.log" >nul
if errorlevel 1 set "FALHOU=salvar o cb-mp.log" & goto :falha0
ver >nul
if exist "ue4ss\Mods\cb-mp\*.dmp" copy /y "ue4ss\Mods\cb-mp\*.dmp" "." >nul
if errorlevel 1 set "FALHOU=salvar os arquivos .dmp do cb-mp" & goto :falha0
ver >nul
if exist "ue4ss\UE4SS.log" copy /y "ue4ss\UE4SS.log" "UE4SS-ultimo.log" >nul
if errorlevel 1 set "FALHOU=salvar o UE4SS.log" & goto :falha0
ver >nul
if exist "ue4ss\*.dmp" copy /y "ue4ss\*.dmp" "." >nul
if errorlevel 1 set "FALHOU=salvar os arquivos .dmp do UE4SS" & goto :falha0

if exist "dwmapi.dll" del /q "dwmapi.dll" >nul 2>&1
if exist "dwmapi.dll" set "FALHOU=remover dwmapi.dll" & goto :falha0
if exist "ue4ss" rmdir /s /q "ue4ss" >nul 2>&1
if exist "ue4ss" set "FALHOU=remover a pasta ue4ss" & goto :falha0

if not exist "cbmp-backup\dwmapi.dll" goto :semdll
move "cbmp-backup\dwmapi.dll" "dwmapi.dll" >nul
if errorlevel 1 goto :falha1
:semdll
if not exist "cbmp-backup\ue4ss" goto :semue
move "cbmp-backup\ue4ss" "ue4ss" >nul
if errorlevel 1 goto :falha1
:semue
rmdir "cbmp-backup" >nul 2>&1
echo Desinstalado. Logs salvos em cb-mp-ultimo.log e UE4SS-ultimo.log (e .dmp, se havia)
echo ao lado deste arquivo.
pause
exit /b 0

:falha0
echo ERRO: falhou ao %FALHOU%. O jogo esta aberto? Rode como administrador se o jogo estiver em Program Files.
echo Seus arquivos originais continuam em cbmp-backup\ (nao foi apagado).
pause
exit /b 1

:falha1
echo ERRO: falhou ao restaurar seus arquivos originais.
echo       Eles continuam em cbmp-backup\ (nao foi apagado). Feche o jogo e tente de novo.
pause
exit /b 1
