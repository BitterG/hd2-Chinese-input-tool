@echo off
rem HD2 Chinese-input tool auto-launcher (run by Task Scheduler every 1 min).
rem Starts proto.exe when HELLDIVERS 2 is running and the tool is not.
rem proto.exe itself auto-exits 5s after the game closes (no residue).
setlocal

tasklist /FI "IMAGENAME eq helldivers2.exe" | find /I "helldivers2.exe" >nul || exit /b 0
tasklist /FI "IMAGENAME eq proto.exe" | find /I "proto.exe" >nul && exit /b 0

start "" "%~dp0proto.exe"
exit /b 0
