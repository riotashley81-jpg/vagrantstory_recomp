@echo off
rem Double-click wrapper for build_release.ps1 (arguments are passed through,
rem e.g. build_release.bat -Clean).
where pwsh >nul 2>nul
if %errorlevel%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release.ps1" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release.ps1" %*
)
set rc=%errorlevel%
if "%~1"=="" pause
exit /b %rc%
