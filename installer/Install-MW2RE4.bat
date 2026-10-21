@echo off
title MW2-RE4 installer
if not exist "%~dp0installer\Install-MW2RE4.ps1" goto notrelease
if not exist "%~dp0files\MW2RE4.asi" goto notrelease
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Install-MW2RE4.ps1" %*
if errorlevel 1 echo. & echo Installation did not complete.
echo.
pause
exit /b

:notrelease
echo The mod files were not found next to this installer.
echo.
echo This looks like the source code (Code ^> Download ZIP), which does not
echo contain the mod. Download MW2-RE4-^<version^>.zip from the Releases page:
echo   https://github.com/Ermes-x86/MW2-RE4/releases
echo extract it, and run Install-MW2RE4.bat from the zip's main folder.
echo.
pause
exit /b 1
