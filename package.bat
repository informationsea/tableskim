@echo off
rem Usage: package.bat [exe|zip|msix|all]   (default: all, msix excluded)
rem
rem Builds a release binary and creates the NSIS installer (needs NSIS on
rem PATH) and/or a ZIP archive in build\%TABLESKIM_PRESET%. The preset is
rem vs-release (Visual Studio) unless TABLESKIM_PRESET is set, e.g. to
rem msvc-release (Ninja, from a Developer Command Prompt).

setlocal
if "%TABLESKIM_PRESET%"=="" set TABLESKIM_PRESET=vs-release
set KIND=%1
if "%KIND%"=="" set KIND=all

cd /d "%~dp0"

rem Test the cache, not the directory: a restored _deps cache (CI) or an
rem interrupted first configure leaves a directory that was never configured.
if not exist build\%TABLESKIM_PRESET%\CMakeCache.txt cmake --preset %TABLESKIM_PRESET% || exit /b 1
cmake --build --preset %TABLESKIM_PRESET% || exit /b 1

cd build\%TABLESKIM_PRESET%
if /i "%KIND%"=="exe" goto nsis
if /i "%KIND%"=="zip" goto zip
if /i "%KIND%"=="msix" goto msix
if /i "%KIND%"=="all" goto nsis
echo Usage: %0 [exe^|zip^|msix^|all] 1>&2
exit /b 1

:nsis
cpack -G NSIS -C Release || exit /b 1
if /i "%KIND%"=="exe" goto done

:zip
cpack -G ZIP -C Release || exit /b 1
goto done

:msix
rem Microsoft Store package. Requires MSIX_IDENTITY_NAME, MSIX_PUBLISHER and
rem MSIX_PUBLISHER_DISPLAY_NAME from Partner Center (see packaging\windows).
if "%MSIX_IDENTITY_NAME%"=="" (
    echo Set MSIX_IDENTITY_NAME, MSIX_PUBLISHER, MSIX_PUBLISHER_DISPLAY_NAME first.
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0packaging\windows\make-msix.ps1" -IdentityName "%MSIX_IDENTITY_NAME%" -Publisher "%MSIX_PUBLISHER%" -PublisherDisplayName "%MSIX_PUBLISHER_DISPLAY_NAME%" -BuildDir "%CD%" || exit /b 1
goto done

:done
dir *.exe *.zip *.msix
