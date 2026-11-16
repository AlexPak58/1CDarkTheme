@echo off
setlocal
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Run build.cmd from an x64 Native Tools Command Prompt. 1>&2
    exit /b 2
)
pushd "%~dp0"
if errorlevel 1 exit /b 1
set "THEME_BUILD_DIR=%~1"
if not defined THEME_BUILD_DIR set "THEME_BUILD_DIR=build"
if not exist "%THEME_BUILD_DIR%" mkdir "%THEME_BUILD_DIR%"
if not exist "%THEME_BUILD_DIR%\" goto failed
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 /LD theme.cpp /Fo"%THEME_BUILD_DIR%\theme.obj" /Fe"%THEME_BUILD_DIR%\tree-theme.dll" bcrypt.lib dwmapi.lib user32.lib
if errorlevel 1 goto failed
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 launcher.cpp /Fo"%THEME_BUILD_DIR%\launcher.obj" /Fe"%THEME_BUILD_DIR%\tree-theme-run.exe" bcrypt.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
