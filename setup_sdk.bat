@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Idempotent XboxXBERecomp SDK bootstrap & patch script for Psi-Ops Native Recomp (Windows).
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0"
if "%REPO_ROOT:~-1%"=="\" set "REPO_ROOT=%REPO_ROOT:~0,-1%"
set "SDK_DIR=%REPO_ROOT%\XboxXBERecomp"
set "PATCH_FILE=%REPO_ROOT%\patches\xboxxberecomp.patch"
set "SDK_URL=https://github.com/TGP482/XboxXBERecomp.git"
set "SDK_SHA=cc9edda3a821a4204dc28783f429b88a27ade4f9"

if exist "C:\Program Files\LLVM\bin\clang.exe" (
    set "PATH=C:\Program Files\LLVM\bin;%PATH%"
)

where git >nul 2>nul
if errorlevel 1 (
    echo [setup_sdk] ERROR: git not found in PATH.
    exit /b 1
)

if not exist "%PATCH_FILE%" (
    echo [setup_sdk] ERROR: missing patch file "%PATCH_FILE%".
    exit /b 1
)

if not exist "%SDK_DIR%\.git" (
    echo [setup_sdk] Cloning XboxXBERecomp into "%SDK_DIR%"...
    if exist "%REPO_ROOT%\.gitmodules" (
        git -C "%REPO_ROOT%" submodule update --init XboxXBERecomp
    )
    if not exist "%SDK_DIR%\.git" (
        git clone "%SDK_URL%" "%SDK_DIR%" || exit /b 1
    )
)

for /f "usebackq delims=" %%H in (`git -C "%SDK_DIR%" rev-parse HEAD 2^>nul`) do set "CUR_SHA=%%H"
if /i not "%CUR_SHA%"=="%SDK_SHA%" (
    echo [setup_sdk] Checking out pinned XboxXBERecomp commit %SDK_SHA%...
    git -C "%SDK_DIR%" cat-file -e "%SDK_SHA%^{commit}" 2>nul
    if errorlevel 1 (
        git -C "%SDK_DIR%" fetch origin || exit /b 1
    )
    git -C "%SDK_DIR%" checkout "%SDK_SHA%" || exit /b 1
)

git -C "%SDK_DIR%" apply --reverse --check --whitespace=nowarn "%PATCH_FILE%" >nul 2>nul
if not errorlevel 1 (
    echo [setup_sdk] XboxXBERecomp [%SDK_SHA:~0,8%] is already patched.
    call :mark_new_files
    exit /b 0
)

git -C "%SDK_DIR%" apply --check --whitespace=nowarn "%PATCH_FILE%" >nul 2>nul
if errorlevel 1 (
    echo [setup_sdk] ERROR: patches\xboxxberecomp.patch does not apply cleanly to %SDK_DIR%.
    echo [setup_sdk] Inspect "git -C XboxXBERecomp status" and retry on a clean %SDK_SHA% tree.
    exit /b 1
)

echo [setup_sdk] Applying patches\xboxxberecomp.patch...
git -C "%SDK_DIR%" apply --whitespace=nowarn "%PATCH_FILE%" || exit /b 1
call :mark_new_files
echo [setup_sdk] XboxXBERecomp [%SDK_SHA:~0,8%] patched successfully.
exit /b 0

:mark_new_files
for /f "usebackq delims=" %%F in (`git -C "%SDK_DIR%" ls-files --others --exclude-standard`) do (
    git -C "%SDK_DIR%" add -N -- "%%F" >nul 2>nul
)
exit /b 0
