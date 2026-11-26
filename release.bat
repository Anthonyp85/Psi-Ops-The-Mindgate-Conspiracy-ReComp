@echo off
setlocal enabledelayedexpansion
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Psi-Ops: The Mindgate Conspiracy - Copyright-clean Win64 launcher & source release packager.
rem Keep %%_PDB%% undefined so cmd.exe-wrapped link steps pass /PDBALTPATH:%%_PDB%% to lld-link verbatim
set "_PDB="

set "REPO_NAME=Psi-Ops-The-Mindgate-Conspiracy-ReComp"
set "VERSION=v1.2.0"
set "SKIP_BUILD=0"

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--version" (
    set "VERSION=%~2"
    shift
    shift
    goto :parse_args
)
if /i "%~1"=="--skip-build" (
    set "SKIP_BUILD=1"
    shift
    goto :parse_args
)
shift
goto :parse_args
:args_done

cd /d "%~dp0"
set "REPO_ROOT=%CD%"
set "DIST_DIR=%REPO_ROOT%\dist"
set "STAGE_DIR=%DIST_DIR%\stage-win64"
set "BUILD_DIR=%REPO_ROOT%\out\build\headers-only-win"
set "WIN_ZIP=%DIST_DIR%\%REPO_NAME%-%VERSION%-win64-launcher.zip"
set "SRC_ZIP=%DIST_DIR%\%REPO_NAME%-%VERSION%-source.zip"

echo.
echo   %REPO_NAME% %VERSION% - Windows x64 Release Packaging
echo   ==============================================================================

rem Safety check: refuse to package while PsiOps.exe is running
powershell -NoProfile -Command "if (Get-Process -Name PsiOps -ErrorAction SilentlyContinue) { exit 1 }"
if errorlevel 1 (
    echo   ERROR: PsiOps.exe is currently running. Close it before packaging.
    exit /b 1
)

rem Ensure Python and Git are available
where python >nul 2>nul || (echo   ERROR: python not found in PATH. & exit /b 1)
where git >nul 2>nul || (echo   ERROR: git not found in PATH. & exit /b 1)

rem Initialize MSVC + LLVM Clang toolchain when building
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if "%SKIP_BUILD%"=="1" goto :skip_win_build
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
    if defined VSDIR (
        if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" (
            call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
        )
    )
)
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang++ >nul 2>nul || (echo   ERROR: clang++ not found. & exit /b 1)
where lld-link >nul 2>nul || (echo   ERROR: lld-link not found. & exit /b 1)
where cmake >nul 2>nul || (echo   ERROR: cmake not found. & exit /b 1)
where ninja >nul 2>nul || (echo   ERROR: ninja not found. & exit /b 1)

echo [1/5] Building copyright-clean Win64 --headers-only target...
cmake -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DPSIOPS_ALLOW_NO_XBE=ON ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG" ^
    "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG" ^
    -DCMAKE_C_COMPILER=clang ^
    -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS=-march=x86-64-v2 ^
    -DCMAKE_CXX_FLAGS=-march=x86-64-v2 || exit /b 1
cmake --build "%BUILD_DIR%" --target PsiOps || exit /b 1
goto :after_win_build

:skip_win_build
echo [1/5] Skipping build [--skip-build]; using existing "%BUILD_DIR%"...

:after_win_build

if not exist "%BUILD_DIR%\PsiOps.exe" (
    echo   ERROR: Missing "%BUILD_DIR%\PsiOps.exe".
    exit /b 1
)

echo [2/5] Verifying retail signed upscaler plugins...
python "%REPO_ROOT%\tools\fetch_upscalers.py" --verify-only || (
    echo   ERROR: Retail upscaler plugins missing or failed verification. Run "python tools\fetch_upscalers.py" first.
    exit /b 1
)

echo [3/5] Staging copyright-clean Win64 launcher tree...
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
mkdir "%STAGE_DIR%"
mkdir "%STAGE_DIR%\config"
mkdir "%STAGE_DIR%\compiler"
mkdir "%STAGE_DIR%\plugins"

copy /y "%BUILD_DIR%\PsiOps.exe" "%STAGE_DIR%\PsiOps.exe" >nul || exit /b 1
copy /y "%REPO_ROOT%\resources\psiops.png" "%STAGE_DIR%\psiops.png" >nul || exit /b 1
copy /y "%REPO_ROOT%\config\game.toml" "%STAGE_DIR%\config\game.toml" >nul || exit /b 1
for %%F in (LICENSE THIRD_PARTY_NOTICES.md README.md) do (
    if not exist "%REPO_ROOT%\%%F" (
        echo   ERROR: Missing required release document "%%F".
        exit /b 1
    )
    copy /y "%REPO_ROOT%\%%F" "%STAGE_DIR%\%%F" >nul || exit /b 1
)

rem Stage clean self-contained compiler/ (never include compiler\work or linker debug/side files)
robocopy "%BUILD_DIR%\compiler" "%STAGE_DIR%\compiler" /E /XD work /XF *.pdb *.ilk *.exp /NFL /NDL /NJH /NJS /NC /NS >nul
if errorlevel 8 (
    echo   ERROR: Failed to stage compiler directory.
    exit /b 1
)
if exist "%STAGE_DIR%\compiler\work" rmdir /s /q "%STAGE_DIR%\compiler\work"
if exist "%REPO_ROOT%\XboxXBERecomp\xdk\5849\signatures.toml" (
    if not exist "%STAGE_DIR%\compiler\xdk\5849" mkdir "%STAGE_DIR%\compiler\xdk\5849"
    copy /y "%REPO_ROOT%\XboxXBERecomp\xdk\5849\signatures.toml" "%STAGE_DIR%\compiler\xdk\5849\signatures.toml" >nul || exit /b 1
)

rem Bundle required MSVC CRT runtime DLLs beside PsiOps.exe and compiler\xbe_recomp.exe
set "CRT_DIR="
if defined VCToolsRedistDir (
    for /d %%D in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do set "CRT_DIR=%%~fD"
)
if not defined CRT_DIR (
    if exist "%SystemRoot%\System32\vcruntime140.dll" set "CRT_DIR=%SystemRoot%\System32"
)
if not defined CRT_DIR (
    echo   ERROR: Could not locate MSVC CRT redistributable DLLs.
    exit /b 1
)
for %%D in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
    if not exist "!CRT_DIR!\%%D" (
        echo   ERROR: Missing CRT runtime DLL "!CRT_DIR!\%%D".
        exit /b 1
    )
    copy /y "!CRT_DIR!\%%D" "%STAGE_DIR%\%%D" >nul || exit /b 1
    copy /y "!CRT_DIR!\%%D" "%STAGE_DIR%\compiler\%%D" >nul || exit /b 1
)

rem Stage verified retail vendor upscaler DLLs + licenses
for %%P in (nvngx_dlss.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll libxess.dll LICENSE.NVIDIA-DLSS.txt LICENSE.AMD-FidelityFX.md LICENSE.Intel-XeSS.txt) do (
    if not exist "%REPO_ROOT%\plugins\%%P" (
        echo   ERROR: Missing plugin artifact "plugins\%%P".
        exit /b 1
    )
    copy /y "%REPO_ROOT%\plugins\%%P" "%STAGE_DIR%\plugins\%%P" >nul || exit /b 1
)

rem Hard guard: reject any game binary, XBE, ISO, PDB, or settings.toml in staging
for %%B in (PsiOps.dll default.xbe settings.toml) do (
    if exist "%STAGE_DIR%\%%B" (
        echo   ERROR: Forbidden artifact "%%B" detected in staging directory!
        exit /b 1
    )
)

echo [4/5] Packing "%WIN_ZIP%"...
if exist "%WIN_ZIP%" del /f /q "%WIN_ZIP%"
python -c "import pathlib, sys, zipfile; src=pathlib.Path(sys.argv[1]); dst=pathlib.Path(sys.argv[2]); files=sorted(p for p in src.rglob('*') if p.is_file()); zf=zipfile.ZipFile(dst, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6); [zf.write(p, p.relative_to(src).as_posix()) for p in files]; zf.close(); print(f'  Packed {len(files)} files into {dst.name} ({dst.stat().st_size:,} bytes)')" "%STAGE_DIR%" "%WIN_ZIP%" || exit /b 1
rmdir /s /q "%STAGE_DIR%"

echo [5/5] Exporting exact Git HEAD archive "%SRC_ZIP%"...
git -C "%REPO_ROOT%" rev-parse --verify HEAD >nul 2>nul
if not errorlevel 1 (
    if exist "%SRC_ZIP%" del /f /q "%SRC_ZIP%"
    git -C "%REPO_ROOT%" archive --format=zip --output="%SRC_ZIP%" HEAD || exit /b 1
) else (
    echo   WARN: No Git HEAD commit yet; skipped source.zip archive.
)

python -c "import hashlib, pathlib; [print(f'  SHA-256 {p.name}: {hashlib.sha256(p.read_bytes()).hexdigest()} ({p.stat().st_size:,} bytes)') for p in sorted(pathlib.Path('dist').glob('*.zip'))]"
echo.
echo   Win64 release packaging complete.
exit /b 0
