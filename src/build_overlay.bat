@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT=%%~fI"
set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
set "OUT_DIR=%ROOT%\build"
set "OUT_EXE=%OUT_DIR%\audio_assist_overlay.exe"

if not exist "%VSDEVCMD%" (
    echo Visual Studio developer command script not found:
    echo   %VSDEVCMD%
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

pushd "%ROOT%"
cl /nologo /std:c++17 /EHsc /W4 ^
  /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /I src ^
  /Fe:"%OUT_EXE%" ^
  src\overlay\DirectionalEventParser.cpp ^
  src\overlay\OverlayService.cpp ^
  src\main.cpp ^
  d2d1.lib dwrite.lib user32.lib gdi32.lib shell32.lib
set "BUILD_EXIT=%errorlevel%"
popd

exit /b %BUILD_EXIT%
