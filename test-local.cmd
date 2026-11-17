@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 tests\enterprise-palette.cpp /Fo"build\enterprise-test.obj" /Fe"build\enterprise-palette-test.exe" bcrypt.lib user32.lib
if errorlevel 1 exit /b 1
build\enterprise-palette-test.exe
