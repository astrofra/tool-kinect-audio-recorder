@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\rebuild_ffmpeg.ps1"
exit /b %errorlevel%
