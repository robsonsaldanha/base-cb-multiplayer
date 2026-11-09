@echo off
setlocal
cd /d "%~dp0"
rem pasta certa = Phoenix\Binaries\Win64: tem o exe e ..\..\Content\Paks
if not exist "HogwartsLegacy.exe" goto :pastaerrada
if not exist "..\..\Content\Paks\" goto :pastaerrada
if exist "cbmp-backup" (
  echo Ja instalado. Rode DESINSTALAR.bat antes de instalar de novo.
  pause
  exit /b 1
)
type nul > "cbmp-write-test.tmp" 2>nul
if errorlevel 1 goto :semescrita
del /q "cbmp-write-test.tmp" >nul 2>&1
mkdir "cbmp-backup"
if errorlevel 1 goto :semescrita

set "DLL_ORIG=0"
set "DLL_MOVED=0"
set "UE_ORIG=0"
set "UE_MOVED=0"
if exist "dwmapi.dll" set "DLL_ORIG=1"
if exist "ue4ss" set "UE_ORIG=1"

if "%DLL_ORIG%"=="0" goto :semdll
move "dwmapi.dll" "cbmp-backup\dwmapi.dll" >nul
if errorlevel 1 set "FALHOU=mover dwmapi.dll" & goto :falha
set "DLL_MOVED=1"
:semdll
if "%UE_ORIG%"=="0" goto :semue
move "ue4ss" "cbmp-backup\ue4ss" >nul
if errorlevel 1 set "FALHOU=mover a pasta ue4ss" & goto :falha
set "UE_MOVED=1"
:semue
xcopy /e /i /y "payload\*" "." >nul
if errorlevel 1 set "FALHOU=copiar os arquivos do mod" & goto :falha

rem o jogo roda sem admin: libera escrita na pasta ue4ss (logs) para o grupo Usuarios
icacls "ue4ss" /grant *S-1-5-32-545:(OI)(CI)M /T >nul
if errorlevel 1 goto :semacl
goto :aclok
:semacl
echo AVISO: nao consegui liberar escrita na pasta ue4ss para o grupo Usuarios.
echo        Os logs podem nao ser gravados ali. Se faltar o cb-mp.log, procure em
echo        %LOCALAPPDATA%\CasteloBruxo\cb-mp\
:aclok

echo Instalado. Abra o jogo, carregue um save, ande 10 s, aperte F8 duas vezes e saia.
echo Depois mande estes arquivos (e qualquer .dmp, veja o LEIA-ME.txt):
echo   ue4ss\Mods\cb-mp\cb-mp.log
echo   ue4ss\UE4SS.log
pause
exit /b 0

:pastaerrada
echo ERRO: extraia este zip DENTRO da pasta Phoenix\Binaries\Win64 do jogo
echo        (onde fica o HogwartsLegacy.exe) e rode de novo.
pause
exit /b 1

:semescrita
echo ERRO: nao consigo gravar nesta pasta.
echo       Se o jogo esta em Program Files, clique com o botao direito neste
echo       arquivo e escolha "Executar como administrador".
if exist "cbmp-write-test.tmp" del /q "cbmp-write-test.tmp" >nul 2>&1
if exist "cbmp-backup" rmdir "cbmp-backup" >nul 2>&1
pause
exit /b 1

:falha
echo ERRO: falhou ao %FALHOU%. O jogo esta aberto? Rode como administrador se o jogo estiver em Program Files.
echo Desfazendo alteracoes...
set "RB=0"
rem remove o que o payload ja copiou (so se o original ja foi tirado do caminho)
if "%DLL_ORIG%"=="1" if "%DLL_MOVED%"=="0" goto :rbue
if exist "dwmapi.dll" del /q "dwmapi.dll" >nul 2>&1
if exist "dwmapi.dll" set "RB=1"
:rbue
if "%UE_ORIG%"=="1" if "%UE_MOVED%"=="0" goto :rbrest
if exist "ue4ss" rmdir /s /q "ue4ss" >nul 2>&1
if exist "ue4ss" set "RB=1"
:rbrest
if "%RB%"=="1" goto :rbfalhou
if "%DLL_MOVED%"=="1" move "cbmp-backup\dwmapi.dll" "dwmapi.dll" >nul
if "%DLL_MOVED%"=="1" if errorlevel 1 set "RB=1"
if "%UE_MOVED%"=="1" move "cbmp-backup\ue4ss" "ue4ss" >nul
if "%UE_MOVED%"=="1" if errorlevel 1 set "RB=1"
if "%RB%"=="1" goto :rbfalhou
rmdir "cbmp-backup" >nul 2>&1
echo Alteracoes desfeitas. Nada foi instalado.
pause
exit /b 1

:rbfalhou
echo ERRO: nao consegui desfazer tudo. Seus arquivos originais estao em cbmp-backup\
echo       Feche o jogo e rode DESINSTALAR.bat.
pause
exit /b 1
