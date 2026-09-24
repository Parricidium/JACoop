@echo off
rem Builds JACoop.exe (native launcher) into dist\launcher\JACoop.exe with the VS Build Tools.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" echo vcvarsall.bat introuvable : "%VCVARS%" && exit /b 1
call "%VCVARS%" x64 >nul
pushd "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS jacoop_launcher.cpp /Fe:..\launcher\JACoop.exe /link /SUBSYSTEM:WINDOWS
set RC=%ERRORLEVEL%
del /q jacoop_launcher.obj 2>nul
popd
exit /b %RC%
