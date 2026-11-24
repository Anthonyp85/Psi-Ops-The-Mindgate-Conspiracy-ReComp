@echo off
setlocal enabledelayedexpansion
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Psi-Ops: The Mindgate Conspiracy (Original Xbox) - Windows x64 build script

set "GAME_TITLE=Psi-Ops: The Mindgate Conspiracy"
set "GAME_EXE=PsiOps"
set "PRESET=release"
set "ALLOW_NO_XBE=OFF"

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--headers-only" (
    set "ALLOW_NO_XBE=ON"
    shift
    goto :parse_args
)
if /i "%~1"=="--preset" (
    set "PRESET=%~2"
    shift
    shift
    goto :parse_args
)
shift
goto :parse_args
:args_done

cd /d "%~dp0"
echo.
echo   %GAME_TITLE% - Windows x64 build (preset=%PRESET%, headers-only=%ALLOW_NO_XBE%)
echo   ==============================================================================

rem Safety check: refuse to overwrite running game executable
tasklist /fi "imagename eq %GAME_EXE%.exe" 2>nul | find /i "%GAME_EXE%.exe" >nul
if not errorlevel 1 (
    echo   ERROR: %GAME_EXE%.exe is currently running. Close it before building.
    exit /b 1
)

rem Locate Visual Studio vcvars64.bat via vswhere (complete installs only)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo   ERROR: vswhere.exe not found at "%VSWHERE%".
    exit /b 1
)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo   ERROR: Visual Studio with C++ x64 build tools was not found.
    exit /b 1
)
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo   ERROR: vcvars64.bat not found at "%VCVARS%".
    exit /b 1
)

echo [1/4] Initializing MSVC + LLVM Clang toolchain...
call "%VCVARS%" >nul || exit /b 1
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang++ >nul 2>nul || (echo   ERROR: clang++ not found. Install LLVM to "%ProgramFiles%\LLVM\bin". & exit /b 1)
where lld-link >nul 2>nul || (echo   ERROR: lld-link not found. & exit /b 1)
where cmake >nul 2>nul || (echo   ERROR: cmake not found. & exit /b 1)
where ninja >nul 2>nul || (echo   ERROR: ninja not found. & exit /b 1)
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v

rem Ensure private default.xbe is extracted when building full image
set "XBE_PATH="
if /i "%ALLOW_NO_XBE%"=="OFF" (
    if exist ".local\xbe\default.xbe" (
        set "XBE_PATH=.local\xbe\default.xbe"
    ) else if exist "assets\default.xbe" (
        set "XBE_PATH=assets\default.xbe"
    ) else (
        echo [2/4] Extracting private .local\xbe\default.xbe from XISO...
        python tools\extract_xbe.py || exit /b 1
        set "XBE_PATH=.local\xbe\default.xbe"
    )
)

set "OUT=%CD%\out\build\%PRESET%"
echo [3/4] Configuring CMake preset %PRESET%...
cmake --preset %PRESET% -DPSIOPS_ALLOW_NO_XBE=%ALLOW_NO_XBE% || exit /b 1

if /i "%ALLOW_NO_XBE%"=="OFF" (
    if not exist "config\game.toml" (
        echo       Generating config\game.toml via xbe_symbols...
        cmake --build "%OUT%" --target xbe_symbols || exit /b 1
        if not exist "%OUT%\sdk_dir.txt" (echo   ERROR: sdk_dir.txt missing. & exit /b 1)
        set /p SDK=<"%OUT%\sdk_dir.txt"
        if not exist config mkdir config
        "%OUT%\xbe_symbols.exe" "%XBE_PATH%" "!SDK!\xdk" --library %GAME_EXE%.dll --out config\game.toml || exit /b 1
    )
)

echo [4/4] Building %GAME_EXE%...
cmake --build "%OUT%" || exit /b 1

echo.
echo   Build complete: %OUT%\%GAME_EXE%.exe
exit /b 0
