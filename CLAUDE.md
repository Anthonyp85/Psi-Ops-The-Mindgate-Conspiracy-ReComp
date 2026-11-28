# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A static recompilation of the Original Xbox (x86, XDK 5849) game *Psi-Ops: The Mindgate Conspiracy* to native Windows x64 (D3D12 + XAudio2) and Linux x86_64 / Steam Deck (Vulkan 1.2), both using SDL3. It is built on the third-party `XboxXBERecomp` SDK, pinned as a git submodule at `cc9edda3a821a4204dc28783f429b88a27ade4f9` and heavily modified through `patches/xboxxberecomp.patch`.

The repo must stay copyright-clean: no `.xbe`, `.iso`, game assets, or vendor DLLs are ever committed (see `.gitignore`). Private material lives in git-ignored `.local/`, `assets/`, `plugins/`.

`PROJECT_MEMORY.md` is the reference for binary-verified guest addresses, struct offsets, ABIs and per-milestone findings; `handoff.md` is the current status log. Read the relevant section of `PROJECT_MEMORY.md` before touching hooks or guest memory, and record new verified addresses there.

## Working alongside Gemini / Antigravity

Primary development happens in Gemini / Antigravity on `main`, in the main checkout. Claude Code is a second agent on the same repo:

- Work only in a worktree on a `claude/*` branch. Never edit the main checkout's working tree, its `XboxXBERecomp/` submodule, or its git config; read from them if needed (`GIT_OPTIONAL_LOCKS=0` for git commands there).
- `AGENTS.md` at the repo root is the Gemini-side agent guide and has more pitfalls than this file. It is currently untracked, so it only exists in the main checkout; read it there when present, and keep this file consistent with it.
- Leave a trail the other agent will see: summarize what changed in `handoff.md`, and put review output in `CODE_REVIEW.md` (open findings from the last read-only review live there).
- Never `git push`, tag, or touch GitHub releases unless the user explicitly says so. Don't bump the version; releases are cut from `main`.
- Audit Gate 1 scans every ref, so commits on any branch must use the owner's GitHub noreply identity (already the repo's `user.email`) and contain no personal names, emails or local paths in messages or files.

## Commands

Windows builds need the MSVC environment (`vcvars64.bat`) plus LLVM at `C:\Program Files\LLVM\bin`; `build.bat` sets this up itself, raw `cmake` calls do not.

```bash
# One-time (and after any submodule reset): init submodule, check out the pinned SHA, apply the patch. Idempotent.
setup_sdk.bat            # Windows
./setup_sdk.sh           # Linux / WSL2

# Full build (needs default.xbe; auto-extracts it from the XISO if missing)
python tools/extract_xbe.py --iso "<path>.xiso.iso"   # -> .local/xbe/default.xbe
build.bat [--preset release|debug|relwithdebinfo]      # -> out/build/<preset>/PsiOps.exe + PsiOps.dll
./build_linux.sh [--preset linux-release|linux-debug]  # -> out/build/<preset>/PsiOps + libPsiOps.so

# Copyright-clean build with no .xbe/.iso (what CI builds)
build.bat --headers-only
cmake --preset release -B out/build/headers-only-win -DPSIOPS_ALLOW_NO_XBE=ON && cmake --build out/build/headers-only-win
cmake --preset linux-release -B out/build/headers-only-linux -DPSIOPS_ALLOW_NO_XBE=ON && cmake --build out/build/headers-only-linux

# Packaging + release audit
python tools/fetch_upscalers.py        # Windows: signed DLSS/FSR3/XeSS DLLs -> plugins/
release.bat [--skip-build]             # -> dist/*-win64-launcher.zip, *-source.zip
./release_linux.sh [--skip-build]      # -> dist/*-linux-x86_64.tar.gz
python tools/audit_release.py --build-dir out/build/release
python tools/audit_release.py --ci --platform win64|linux --build-dir <dir>   # CI subset
```

Build scripts refuse to run while `PsiOps.exe` / `PsiOps` is running.

### Tests

There are no unit tests or linters. Verification is done by Python suites in `tools/` that launch the built game and drive it in-process (never OS-level input). They need a full (non-headers-only) build and the retail XISO; Linux cases run through WSL2 Ubuntu 22.04 (`--wsl-distro` or `PSIOPS_WSL_DISTRO`). All write to `.local/<suite>/`, and most accept `--platform win64|linux|both` and `--dry-run`.

| Suite | Covers |
|---|---|
| `test_disc_64bit.py` | >2 GiB XDVDFS offsets (read-only, no game launch) |
| `test_m2_gameplay.py` | boot → 3D gameplay, screenshots, clean logs, 1.00x sim speed |
| `test_m3_mkb.py` | mouse & keyboard aim/bindings/cursor capture |
| `test_m3_replay.py` | controller `.rec` record + deterministic replay |
| `test_m4_perf.py` | perf metrics, 60 vs 30 FPS fidelity, pacing (`--mode pre\|post\|full\|compare`) |
| `test_m5_ux.py` | launcher, F1 overlay, file browser, shortcuts, first-launch compile |
| `test_real_upscalers.py` | all 9 upscalers on real GPU |
| `test_shader_precompile.py` | cold/warm shader + PSO cache, zero mid-gameplay compiles |
| `test_issue2_unicode_and_options.py` | non-ACP/Unicode path safety, live `[game_options]` sync |

Run one suite on one platform, e.g. `python tools/test_m3_mkb.py --platform win64`. The suites share helpers by importing `test_m2_gameplay` as `m2`.

## Architecture

### Three layers

1. **SDK (`XboxXBERecomp/`, submodule + patch).** `tools/recomp` (`xbe_recomp`) lifts the XBE's x86 to C++; `tools/symbols` (`xbe_symbols`) generates `config/game.toml`; `runtime/` is the host: `app` (entry points, settings, first-launch game-code compile), `kernel` (xboxkrnl, disc, memory, threads), `xdk` (D3D8/DSound/XAPI HLE), `gpu` (D3D12 + Vulkan backends, shader/PSO caches), `audio`, `cpu`, `core`. `cmake/XbeRecomp.cmake` provides `add_xbe_image()`, which builds everything.
2. **Host extensions (`src/common/`, `src/upscale/`).** Game-specific host code compiled into the executable: `game_info.cpp` (defines `app::kGame` and registers present/pad/rumble/settings/camera callbacks with the runtime), launcher and F1/F2 overlay (ImGui), M&K input (`mkb_input`), `.rec` input recorder, update checker/self-updater, UI aspect modes, and the upscaler manager (`gpu::Upscaler` implementation: spatial shaders built in, DLSS/FSR3/XeSS loaded from `plugins/` via the hand-written `*_abi.h` headers, Windows only).
3. **Game hooks (`src/game/`).** `hooks.cpp`, `cheats.cpp`, `guest_settings.cpp` — code that reads/writes guest memory and intercepts guest functions.

The root `CMakeLists.txt` globs `src/*/*.cpp` minus `src/game/` as `XBE_HOST_SOURCES`; `add_xbe_image(PsiOps <xbe> game.toml <partitions> game)` adds `src/game/*.cpp` and the SDK runtime. New `.cpp` files in existing `src/` subdirectories are picked up automatically.

### Output shape and headers-only mode

The executable (`PsiOps.exe` / `PsiOps`) holds the runtime and all host/hook code. The recompiled game code is a separate freestanding library (`PsiOps.dll` / `libPsiOps.so`) generated into `generated/game/` and split into `partitions` translation units (from `config/game.toml`).

Release archives ship **without** that library: on first launch the runtime (`runtime/app/game_code.cpp`) recompiles the player's own `default.xbe` using the bundled `compiler/` toolchain. `PSIOPS_ALLOW_NO_XBE=ON` (`XBE_NO_IMAGE`, `xbe_recomp --headers-only`) is the matching build mode: it generates only headers/import stubs from `game.toml`, so the exe builds with no XBE present. It is also selected automatically when no `default.xbe` is found. Anything in `src/` must therefore compile and link in headers-only mode.

### Hooking guest code

Two mechanisms, both keyed by guest virtual address:

- **Address hooks** — a `[[hook]]` entry in `config/game.toml` (`at = 0x...`, `call = "Hook_Name"`, optional `on_true = "ret"`) plus `void|bool Hook_Name(X86Context& ctx, uint8_t* base)` in `src/game/hooks.cpp`. Returning `true` with `on_true = "ret"` skips the original function.
- **Whole-function overrides** — define `X86_FUNC(sub_XXXXXXXX)` in `src/game/`, and call the original via `X86_EXTERN_FUNC(__imp__sub_XXXXXXXX)`. `xbe_recomp --headers-only` scans the sources for these.

`config/game.toml` also carries `[symbols]`/`[data]` (XDK function and data addresses the HLE layer replaces; `0` = not present in this image) and `[functions]` (extra entry points). Changing it reconfigures CMake and regenerates code.

Rules that the existing hooks follow and new ones must too:
- Validate every guest pointer with `ValidGuest(va, span)` before dereferencing; pass the full span touched.
- Wrap any call back into guest code from a hook in `GuestCallFrame` (snapshots/restores the full `X86Context`, including the software x87 state).
- Cheats and guest-settings sync are suppressed during `.rec` record, replay and autotests (`ShouldBypassGuestSettings()`); M&K hooks are bypassed during replay and autotests only (`input_rec::ShouldBypassMkb()`). Controller-mode behavior and replays must stay bit-identical.
- Hook comments cite the guest address, calling convention and stack cleanup; keep that convention.
- Guest threads are pinned to one core. Never mutate guest objects from UI or render threads; queue the change through `psiops::guest_settings` / `psiops::cheats` and apply it on the guest thread.
- The four CRI ADX hooks (`0x3FFBD0`, `0x3FFC20`, `0x3FFC9E`, `0x3FFD00`) replace priority-preemption assumptions that do not hold on a host scheduler; don't remove them.

### Runtime test hooks

The Python suites drive the game through environment variables read by the runtime and `src/common` (`XBE_AUTOTEST`, `XBE_AUTOTEST_SCRIPT`, `XBE_MKB_AUTOTEST`, `XBE_AUTOTEST_OUT_DIR`, `XBE_INPUT_RECORD`/`XBE_INPUT_REPLAY`, `XBE_STATE_LOG`, `XBE_PERF_LOG`, `XBE_LAUNCHER_AUTOPLAY_MS`, `XBE_CAPTURE_UI_BMP`, `PSIOPS_UPDATE_MOCK_JSON`, …) and CLI flags documented in the README.

## Working rules specific to this repo

**SDK changes go through the patch.** The submodule is `ignore = dirty`, so edits under `XboxXBERecomp/` never show in the parent repo's `git status` and are never committed directly. After editing SDK files, re-export the patch so it is byte-identical to the live diff (audit Gate 2 enforces this, along with zero CR bytes):

```bash
git -C XboxXBERecomp add -N <any new files>
git -C XboxXBERecomp diff --no-ext-diff --binary cc9edda3a821a4204dc28783f429b88a27ade4f9 --output=../patches/xboxxberecomp.patch
```

Do not produce the patch with PowerShell `>` redirection (re-encodes it). The patch must contain zero CR bytes; `core.autocrlf` should be `false` for the submodule (on this machine it is `true` at system scope, so check a fresh SDK checkout before exporting).

**Line endings** are enforced by `.gitattributes`: LF for everything except `*.bat`/`*.cmd` (CRLF).

**SPDX header.** Every `.h`, `.hpp`, `.c`, `.cpp`, `.py`, `.sh`, `.bat`, `.rc`, `.desktop`, `.yml`, `.yaml` and `CMakeLists.txt` needs `SPDX-License-Identifier: GPL-2.0-or-later` in its first 512 bytes (audit Gate 2).

**Host paths are UTF-8.** Never call `.string()` / `.generic_string()` on a `std::filesystem::path` for a host path; use `app::PathToUtf8` / `app::Utf8ToPath` and wide file opens on Windows (`_wfopen`, `_wgetenv`). Iterate directories with the `error_code` overloads.

**Version bumps** must be synchronized across `CMakeLists.txt` (`project(... VERSION)`), `src/common/version.h`, the `static_assert` in `src/common/game_info.cpp`, `resources/psiops.rc`, `resources/app.manifest`, `release.bat`, `release_linux.sh`, `tools/audit_release.py` (`DEFAULT_VERSION`), and the README archive names.

**Path privacy / reproducibility.** `CMakeLists.txt` remaps every host path prefix out of binaries (`-ffile-prefix-map`, `/Brepro`, `/PDBALTPATH:%_PDB%`), and audit Gate 2b scans built binaries for leaked user/workspace paths. Don't embed absolute paths in code or tracked files, and keep `_PDB` unset when invoking the Windows link (the `.bat` scripts do this).

**Source media is read-only.** The XISO is opened read-only everywhere and its mtime is checked by tests and audit Gate 6; never write to or move it. All disc offsets must be unsigned 64-bit (21 FMV files sit above 2 GiB).

**Simulation rate.** Only 60 and 30 FPS caps are valid; Havok skips steps shorter than 1/120 s, so anything higher breaks physics. `SanitizeSafeFpsCap` (SDK `runtime/app/settings.cpp`) and the settings UI clamp to these two values; high-refresh displays are handled by present mode, not by raising the cap.

**Linux ABI ceiling.** The Linux binary must not require anything newer than `GLIBC_2.35` (CI checks with `objdump -T`); build on Ubuntu 22.04.

**Dual platform.** Host features are expected to work on both Win64/D3D12 and Linux/Vulkan (temporal upscalers excepted). Platform-specific SDK sources are filtered in `XbeRecomp.cmake` (`main.cpp`/`xaudio2.cpp`/`d3d12.cpp` vs `main_linux.cpp`/`sdl_audio.cpp`/`vulkan.cpp`).

**Settings persistence.** Optional `settings.toml` tables (`[game_options]`, `[cheats]`, `[ui_aspect]`) are omitted until the user configures them, and an unconfigured launcher exit must perform zero writes — `test_m5_ux.py` asserts this.

## CI

`.github/workflows/build.yml` builds headers-only on `ubuntu-22.04` and `windows-2022`, checks the GLIBC ceiling, captures a headless launcher screenshot under lavapipe, then runs `release_*.sh/.bat --skip-build` and `audit_release.py --ci`.
