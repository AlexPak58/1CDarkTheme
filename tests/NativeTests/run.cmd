@echo off
setlocal
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Run from an x64 Native Tools Command Prompt. 1>&2
    exit /b 2
)
if "%~1"=="" (
    echo Usage: run.cmd ^<new external artifact directory^> 1>&2
    exit /b 2
)
set "OUT=%~f1"
if exist "%OUT%" (
    echo Use a new artifact directory. 1>&2
    exit /b 2
)
pushd "%~dp0..\.."
if errorlevel 1 exit /b 1
mkdir "%OUT%\legacy"
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\normal"
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\trace" search-results-trace
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\color" search-results-color-test
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\background" int3-bg
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\counter" counter-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\grid" grid-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\frame" frame-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\scroller" scroller-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\dock" dock-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\property" property-probe
if errorlevel 1 goto failed
call "%CD%\build.cmd" "%OUT%\property-paint" property-paint-probe
if errorlevel 1 goto failed
for %%T in (profile-test attach-test caption-test install-test counter-probe-test counter-launcher-test grid-probe-test grid-launcher-test devtool-binding-test global-search-test frame-probe-test frame-launcher-test panel-headers-test scroller-probe-test scroller-launcher-test dock-probe-test dock-launcher-test dock-splitters-test property-probe-test property-launcher-test property-paint-probe-test property-paint-launcher-test) do (
    cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 /I. "%~dp0%%T.cpp" /Fo"%OUT%\%%T.obj" /Fe"%OUT%\%%T.exe" /link bcrypt.lib dwmapi.lib user32.lib
    if errorlevel 1 goto failed
)
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 /LD "%~dp0legacy-test.cpp" /Fo"%OUT%\legacy\test.obj" /Fe"%OUT%\legacy\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\profile-test.exe" DESIGNER
if errorlevel 1 goto failed
"%OUT%\profile-test.exe" ENTERPRISE
if errorlevel 1 goto failed
"%OUT%\attach-test.exe" "%OUT%\normal\tree-theme.dll" "%OUT%\legacy\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\caption-test.exe"
if errorlevel 1 goto failed
"%OUT%\install-test.exe"
if errorlevel 1 goto failed
"%OUT%\counter-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\counter-launcher-test.exe" "%OUT%\counter\search-counter-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\grid-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\grid-launcher-test.exe" "%OUT%\grid\grid-identity-probe.dll" "%OUT%\counter\search-counter-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\devtool-binding-test.exe"
if errorlevel 1 goto failed
"%OUT%\global-search-test.exe"
if errorlevel 1 goto failed
"%OUT%\panel-headers-test.exe"
if errorlevel 1 goto failed
"%OUT%\dock-splitters-test.exe"
if errorlevel 1 goto failed
"%OUT%\frame-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\frame-launcher-test.exe" "%OUT%\frame\local-frame-probe.dll" "%OUT%\grid\grid-identity-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\scroller-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\scroller-launcher-test.exe" "%OUT%\scroller\scroller-identity-probe.dll" "%OUT%\frame\local-frame-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\dock-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\dock-launcher-test.exe" "%OUT%\dock\dock-fill-probe.dll" "%OUT%\scroller\scroller-identity-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\property-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\property-launcher-test.exe" "%OUT%\property\property-description-probe.dll" "%OUT%\dock\dock-fill-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\property-paint-probe-test.exe"
if errorlevel 1 goto failed
"%OUT%\property-paint-launcher-test.exe" "%OUT%\property-paint\property-paint-probe.dll" "%OUT%\property\property-description-probe.dll" "%OUT%\normal\tree-theme.dll"
if errorlevel 1 goto failed
"%OUT%\property-paint\property-paint-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\property\property-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\dock\dock-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\frame\frame-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\grid\grid-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\counter\counter-probe-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
"%OUT%\normal\tree-theme-run.exe" --attach bogus 1 --palette impossible
if not "%errorlevel%"=="2" goto failed
popd
exit /b 0
:failed
popd
exit /b 1
