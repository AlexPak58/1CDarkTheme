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
set "THEME_SOURCE=theme.cpp"
set "THEME_LAUNCHER_SOURCE=launcher.cpp"
set "THEME_DLL_NAME=tree-theme.dll"
set "THEME_LAUNCHER_NAME=tree-theme-run.exe"
set "THEME_DLL_FLAGS="
set "THEME_LAUNCHER_FLAGS="
if /i "%~2"=="counter-probe" (
    set "THEME_SOURCE=search_counter_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=counter_probe_launcher.cpp"
    set "THEME_DLL_NAME=search-counter-probe.dll"
    set "THEME_LAUNCHER_NAME=counter-probe-run.exe"
)
if /i "%~2"=="grid-probe" (
    set "THEME_SOURCE=grid_identity_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=grid_probe_launcher.cpp"
    set "THEME_DLL_NAME=grid-identity-probe.dll"
    set "THEME_LAUNCHER_NAME=grid-probe-run.exe"
)
if /i "%~2"=="frame-probe" (
    set "THEME_SOURCE=local_frame_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=frame_probe_launcher.cpp"
    set "THEME_DLL_NAME=local-frame-probe.dll"
    set "THEME_LAUNCHER_NAME=frame-probe-run.exe"
)
if /i "%~2"=="scroller-probe" (
    set "THEME_SOURCE=scroller_identity_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=scroller_probe_launcher.cpp"
    set "THEME_DLL_NAME=scroller-identity-probe.dll"
    set "THEME_LAUNCHER_NAME=scroller-probe-run.exe"
)
if /i "%~2"=="png-surface-probe" (
    set "THEME_SOURCE=png_surface_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=png_surface_probe_launcher.cpp"
    set "THEME_DLL_NAME=scroller-png-surface-probe.dll"
    set "THEME_LAUNCHER_NAME=png-surface-probe-run.exe"
)
if /i "%~2"=="dock-probe" (
    set "THEME_SOURCE=dock_fill_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=dock_probe_launcher.cpp"
    set "THEME_DLL_NAME=dock-fill-probe.dll"
    set "THEME_LAUNCHER_NAME=dock-probe-run.exe"
)
if /i "%~2"=="property-probe" (
    set "THEME_SOURCE=property_description_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=property_probe_launcher.cpp"
    set "THEME_DLL_NAME=property-description-probe.dll"
    set "THEME_LAUNCHER_NAME=property-probe-run.exe"
)
if /i "%~2"=="property-paint-probe" (
    set "THEME_SOURCE=property_paint_probe.cpp"
    set "THEME_LAUNCHER_SOURCE=property_paint_launcher.cpp"
    set "THEME_DLL_NAME=property-paint-probe.dll"
    set "THEME_LAUNCHER_NAME=property-paint-probe-run.exe"
)
if /i "%~2"=="cairo" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_BENCHMARK"
)
if /i "%~2"=="cairo-bg" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_BACKGROUND_ONLY"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_BENCHMARK"
)
if /i "%~2"=="tooltip-trace" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_TOOLTIP_TRACE"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_TOOLTIP_TRACE"
)
if /i "%~2"=="search-results-trace" (
    set "THEME_SOURCE=search_results_trace.cpp"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_SEARCH_RESULTS_TRACE"
)
if /i "%~2"=="search-results-color-test" (
    set "THEME_SOURCE=search_results_trace.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_SEARCH_RESULTS_COLOR_TEST"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_SEARCH_RESULTS_TRACE"
)
if /i "%~2"=="form-line-trace" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_FORM_LINE_TRACE"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_FORM_LINE_TRACE"
)
if /i "%~2"=="spreadsheet-outline-trace" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_OUTLINE_TRACE"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_OUTLINE_TRACE"
)
if /i "%~2"=="spreadsheet-grid-trace" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_GRID_TRACE"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_GRID_TRACE"
)
if /i "%~2"=="spreadsheet-grid-color-test" (
    set "THEME_SOURCE=cairo_experiment.cpp"
    set "THEME_DLL_FLAGS=/DTHEME_GRID_TRACE /DTHEME_GRID_COLOR_TEST"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_GRID_TRACE"
)
if /i "%~2"=="int3" set "THEME_LAUNCHER_FLAGS=/DTHEME_BENCHMARK"
if /i "%~2"=="int3-bg" (
    set "THEME_DLL_FLAGS=/DTHEME_BACKGROUND_ONLY"
    set "THEME_LAUNCHER_FLAGS=/DTHEME_BENCHMARK"
)
if not exist "%THEME_BUILD_DIR%" mkdir "%THEME_BUILD_DIR%"
if not exist "%THEME_BUILD_DIR%\" goto failed
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 %THEME_DLL_FLAGS% /LD "%THEME_SOURCE%" /Fo"%THEME_BUILD_DIR%\theme.obj" /Fe"%THEME_BUILD_DIR%\%THEME_DLL_NAME%" bcrypt.lib dwmapi.lib user32.lib
if errorlevel 1 goto failed
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /utf-8 %THEME_LAUNCHER_FLAGS% "%THEME_LAUNCHER_SOURCE%" /Fo"%THEME_BUILD_DIR%\launcher.obj" /Fe"%THEME_BUILD_DIR%\%THEME_LAUNCHER_NAME%" bcrypt.lib user32.lib
if errorlevel 1 goto failed
if /i "%~2"=="settings" (
    dotnet build settings\ConfiguratorThemeSettings.csproj -c Release --artifacts-path "%THEME_BUILD_DIR%\dotnet" -o "%THEME_BUILD_DIR%"
    if errorlevel 1 goto failed
)
popd
exit /b 0
:failed
popd
exit /b 1
