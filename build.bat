@echo off
REM build.bat — Build and run Shield on Windows.
REM Usage:
REM   build.bat           Debug build
REM   build.bat release   Release build
REM   build.bat run       Build + run
REM   build.bat clean     Clean build directory

setlocal enabledelayedexpansion

set BUILD_TYPE=Debug
set RUN_AFTER=0
set CLEAN=0

if "%1"=="release" set BUILD_TYPE=Release
if "%1"=="run" set RUN_AFTER=1
if "%1"=="clean" set CLEAN=1

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build

if %CLEAN%==1 (
    echo Cleaning build directory...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Done.
    exit /b 0
)

REM Configure
set TOOLCHAIN=
if defined VCPKG_ROOT (
    set TOOLCHAIN=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
)

echo === Shield Build ===
echo Build type: %BUILD_TYPE%
echo Build dir:  %BUILD_DIR%
echo.

cmake -B "%BUILD_DIR%" %TOOLCHAIN% -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DSHIELD_BUILD_TESTS=ON -DSHIELD_BUILD_EXAMPLES=ON
if %errorlevel% neq 0 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
if %errorlevel% neq 0 exit /b %errorlevel%

echo.
echo === Build Complete ===
echo Binary: %BUILD_DIR%\bin\shield.exe
echo.

if %RUN_AFTER%==1 (
    echo === Starting Shield Server ===
    "%BUILD_DIR%\bin\shield.exe" server --config config\app.yaml
)
