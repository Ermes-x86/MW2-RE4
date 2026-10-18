@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /std:c++17 /O2 /MT /EHa /LD /W3 /D_CRT_SECURE_NO_WARNINGS /d1trimfile:"%~dp0\" "%~dp0src\MW2RE4.cpp" /Fo"%~dp0build\MW2RE4.obj" /link /OUT:"%~dp0build\MW2RE4.asi" /PDB:"%~dp0build\MW2RE4.pdb" /DEBUG /INCREMENTAL:NO user32.lib kernel32.lib d3d9.lib gdi32.lib
exit /b %errorlevel%
