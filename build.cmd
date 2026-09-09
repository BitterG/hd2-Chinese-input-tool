@echo off
rem Build hd2-ocr-input prototype (P0: hotkey placeholder sensing + probe foundation).
rem Usage (cmd):      build.cmd
rem Usage (git bash): cmd //c build.cmd
setlocal

for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [build] vswhere: Visual Studio with VC tools not found
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /O2 src\proto.cpp src\sensing_hotkey.cpp src\fgutil.cpp src\inject.cpp src\carrier.cpp src\floattext.cpp src\capture.cpp src\feature.cpp src\pixel.cpp /link user32.lib gdi32.lib dwmapi.lib imm32.lib /out:proto.exe
if errorlevel 1 (
    echo [build] cl failed
    exit /b 2
)
echo [build] ok: proto.exe
