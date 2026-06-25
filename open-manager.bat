@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\open-manager.ps1"
if errorlevel 1 pause
exit /b %errorlevel%
