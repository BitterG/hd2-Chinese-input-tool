@echo off
rem HD2 Chinese-input tool auto-launcher (run by Task Scheduler every 1 min).
rem Starts hd2-chinese-input.exe when HELLDIVERS 2 is running and the tool is not.
rem The tool itself auto-exits 5s after the game closes (no residue).
setlocal

tasklist /FI "IMAGENAME eq helldivers2.exe" | find /I "helldivers2.exe" >nul || exit /b 0
tasklist /FI "IMAGENAME eq hd2-chinese-input.exe" | find /I "hd2-chinese-input.exe" >nul && exit /b 0

start "" "%~dp0hd2-chinese-input.exe"
exit /b 0
