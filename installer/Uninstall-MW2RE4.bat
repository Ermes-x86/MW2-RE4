@echo off
title MW2-RE4 uninstaller
if not exist "%~dp0installer\Uninstall-MW2RE4.ps1" goto notrelease
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Uninstall-MW2RE4.ps1" %*
echo.
pause
exit /b

:notrelease
echo The uninstaller scripts were not found next to this file.
echo.
echo Run Uninstall-MW2RE4.bat from the main folder of the release zip
echo (MW2-RE4-^<version^>.zip from https://github.com/Ermes-x86/MW2-RE4/releases).
echo.
pause
exit /b 1
