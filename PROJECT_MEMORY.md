# PROJECT_MEMORY.md — Psi-Ops: The Mindgate Conspiracy (Original Xbox Static Recompilation)

## 1. Project Identity & Scope
- **Title:** *Psi-Ops: The Mindgate Conspiracy* (USA Retail, Original Xbox — Intel x86, NOT Xbox 360/PowerPC)
- **Title ID:** `0x4D570016` (`MW-022`, publisher `'MW'`, decimal `22`, cert UTF-16LE title `PsiOps`, Region `0x00000001` NA, Allowed Media `0x00000202` `DVD_X2 | MEDIA_BOARD`, `init_flags = 0x00000005` `MountUtilityDrive | Limit64MB`)
- **Architecture:** Static x86 → C++20 recompilation via pinned **`XboxXBERecomp`** (`cc9edda3a821a4204dc28783f429b88a27ade4f9`) + pure-LF `patches/xboxxberecomp.patch`.
- **Dual Target Platforms:**
  - **Windows x64:** D3D12 + XAudio2 + static SDL3 (`PsiOps.exe` + freestanding `PsiOps.dll`), Clang/LLD (`C:\Program Files\LLVM\bin`, LLVM 23.1.6) + MSVC SDK (`10.0.26100.0`).
  - **Linux x86_64 / SteamOS 3.x:** Native Vulkan 1.2 + static SDL3 + static `glslang`/SPIR-V (`PsiOps` + freestanding `libPsiOps.so`), compiled in WSL2 Ubuntu 22.04.5 LTS (`glibc 2.35`, `GLIBCXX_3.4.30`, `-fuse-ld=lld -rdynamic -shared-libgcc`).
- **Copyright-Clean Mode:** `-DPSIOPS_ALLOW_NO_XBE=ON` (`XBE_NO_IMAGE=ON`, `xbe_recomp --headers-only`) builds host runtime + headers without requiring any `.xbe` or disc image.

---

## 2. Immutable Source Media Ground Truth (READ-ONLY)
- **XISO Path:** `Psi-Ops - The Mindgate Conspiracy (USA)/Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso`
  - Size: `2,801,008,640` bytes (`0xA6F40000`, `2.6086 GiB`)
  - `st_mtime_ns`: `1793646566434966000`
  - **SHA-256:** `fe1b8f418631a3b1d35d7c76d12fd461a10a062746432c9823251ea4f2b96e4e`
  - **SHA-1:** `c46bd84051bc647ce7b62c7a8f4341583e178a9f`
  - **XDVDFS Layout:** Raw XISO game partition (`base = 0x0`), `MICROSOFT*XBOX*MEDIA` at `0x00010000` (sector 32) and footer at `0x107EC`, `root_sector = 264` (`0x00084000`), `root_size = 104` B.
  - **Totals:** `18` subdirectories, `385` files, `2,800,320,499` bytes (`2.6080 GiB`), **single-XBE disc** (`default.xbe` only).
- **Archive Backup:** `Psi-Ops - The Mindgate Conspiracy (USA).7z` (`2,243,057,495` B, `st_mtime_ns = 1795310831123776200`) — untouched.
- **Companion Note:** Companion `.txt` metadata note inside the game media folder (`330` B, `st_mtime_ns = 1793646567091975900`) — verified optical disc SHA-1 `7da2409254ecf3818f28b6b69db8fcec66da29dc`.

### Critical >2 GiB (`INT32_MAX` = `0x7FFFFFFF`) Disc Layout
- **21 `.sfd` files** exceed `0x7FFFFFFF` (up to `0xA6F3D800` / `2,800,998,400` B):
  - **2 GiB boundary straddle:** `data/movies/L5Q10poss.sfd` (`0x07E155800`..`0x080310000`, `35,366,912` B).
  - **Boot FMVs above 2 GiB:** `data/movies/Legal.sfd` (`0x088409800`), `data/movies/LOGO.sfd` (`0x08D166800`), `data/movies/Psi_logo.sfd` (`0x09A034800`).
  - **Highest-offset archive entry:** sector offset `0x0A4EC7800`..`0x0A6F3D800` (`34,037,760` B).
- **Requirement:** All XDVDFS, `Disc::BlockCache` (`16 MiB`, `64 KiB` striped), Win32 `OVERLAPPED`, and POSIX `pread` (`_FILE_OFFSET_BITS=64`) paths must use unsigned 64-bit byte offsets (`uint64_t(sector) * 2048ULL`). Verified via `python tools/test_disc_64bit.py`.

---

## 3. `default.xbe` Binary Ground Truth
- **Extracted Privately To:** `.local/xbe/default.xbe` (gitignored; via `python tools/extract_xbe.py`)
- **Size:** `5,672,960` B (`0x569000`)
- **SHA-256:** `2241d4c515479c525752800556322ce1055464c78198b76e364c27dca30205be`
- **SHA-1:** `42dd95fa444039b82f843ff02a31833da97d41cf`
- **XBE Header (Retail Signed):**
  - `base_address = 0x00010000`, `sizeof_headers = 0x00000AA8`, `sizeof_image = 0x006046C0`
  - **Entry Point VA:** `0x0036DEA7` (`.text`, retail XOR `0xA8FC57AB`)
  - **Kernel Thunk Table VA:** `0x004D0EA0` (`.rdata`, retail XOR `0x5B6D40B6`, 148 unique ordinals)
  - **TLS Directory VA:** `0x004D2B68` (`index_va = 0x0056A078`, `zero_fill = 0xA8`)
  - **Debug Path:** `C:\ESP\Game\output\xbox\Game_dvd.exe` / `Game_dvd.pdb` (age `6`, timestamp `2004-05-14T02:06:24Z`)

### Sections (15 Total — 9 Executable Code Sections Selected by `xbe_symbols`)
| Idx | Name | VA | VirtualSize | RawAddr | RawSize | Recomp Code? |
|---:|---|---:|---:|---:|---:|---|
| 0 | `.text` | `0x00011000` | `0x0046887C` | `0x00001000` | `0x0046887C` | Yes |
| 1 | `D3D` | `0x00479880` | `0x00023220` | `0x0046A000` | `0x0001F948` | Yes |
| 2 | `XGRPH` | `0x0049CAA0` | `0x00002874` | `0x0048A000` | `0x00002874` | Yes |
| 3 | `DSOUND` | `0x0049F320` | `0x00026374` | `0x0048D000` | `0x00026104` | Yes |
| 4 | `PSGSFD00` | `0x004C56A0` | `0x00003F60` | `0x004B4000` | `0x00003F60` | Yes (CRI Sofdec) |
| 5 | `PSGSFD_I` | `0x004C9600` | `0x000003D0` | `0x004B8000` | `0x000003D0` | Yes (CRI Sofdec) |
| 6 | `PSGSFD_B` | `0x004C99E0` | `0x000008A0` | `0x004B9000` | `0x000008A0` | Yes (CRI Sofdec) |
| 7 | `PSGSFD_P` | `0x004CA280` | `0x00000610` | `0x004BA000` | `0x00000610` | Yes (CRI Sofdec) |
| 8 | `XPP` | `0x004CA8A0` | `0x000065E4` | `0x004BB000` | `0x000065E4` | Yes |
| 9 | `.rdata` | `0x004D0EA0` | `0x00076164` | `0x004C2000` | `0x00076154` | No |
| 10 | `.data` | `0x00547020` | `0x000C1F80` | `0x00539000` | `0x00022D30` | No |
| 11 | `DOLBY` | `0x00608FA0` | `0x00007180` | `0x0055C000` | `0x0000716C` | No |
| 12 | `$$XTIMAGE` | `0x00610120` | `0x00002800` | `0x00564000` | `0x00002800` | No |
| 13 | `$$XSIMAGE` | `0x00612920` | `0x00001000` | `0x00567000` | `0x00001000` | No |
| 14 | `.XTLID` | `0x00613920` | `0x00000D90` | `0x00568000` | `0x00000D90` | No (XTLID symbols) |

### XDK Libraries & Engine Middleware
- **XDK Build:** Uniform Retail non-LTCG **`1.0.5849.1`** (`0x4001`) across all 7 libraries: `XAPILIB`, `D3D8`, `XGRAPHC`, `DSOUND`, `XBOXKRNL`, `LIBCPMT`, `LIBCMT`. Matches upstream `XboxXBERecomp/xdk/5849/{layout,signatures}.toml` out-of-the-box. (No `XONLINE`, `XNET`, `XACTENG`, `XMV`, `WMADEC`.)
- **Kernel Thunks (`148` unique ordinals):** `143/148` present in upstream `cc9edda3`; remaining `5` ordinals (`76` `IoQueryVolumeInformation`, `191` `NtCreateIoCompletion`, `223` `NtRemoveIoCompletion`, `227` `NtSetIoCompletion`, `243` `ObOpenObjectByName`) implemented via `patches/xboxxberecomp.patch`.
- **Engine / Middleware:** Midway Chicago proprietary **ESP** engine (`ESPx` v6 `.xbox` archives) + **Havok** physics + **Qhull** + embedded **Python 2.2.1** + **AI.implant** + **CRI Sofdec `.sfd`** (`MWSFD/XBOX 2.65`, 70 `.sfd` files) + **DirectSound `5849`** (`dsstdfx.bin`, Midway Sound Library `.mbx`/`.msx` + CRI ADX).

---

## 4. Verified Host Hardware & Toolchain Notes
- **Host OS & Hardware:** Windows 11 Pro 25H2 (Build `26200.9974`), **AMD Ryzen 7 9800X3D** (8C/16T, single CCD, homogeneous Zen 5, 96 MiB L3 V-Cache), `61.64 GiB` RAM, **NVIDIA GeForce RTX 3090** (`24 GiB`, Driver `616.92`, Vulkan `1.4.351`) + AMD Radeon iGPU (`26.6.1`), signed local NGX DLSS `310.9.16.0` & `310.4.0.0`.
- **Windows Toolchain:** Standalone LLVM/Clang **`23.1.6`** at `C:\Program Files\LLVM\bin` (NOT in default PATH; prepended by `CMakePresets.json` & `build.bat`), VS Community 2026 (`18.8.0`, MSVC `14.51.36231`) & VS Build Tools 2022 (`17.14.13`, MSVC `14.44.35207`), WinSDK `10.0.26100.0` (`rc/mt/fxc/dxc`), `cmake 4.4.4`, `ninja 1.13.0`, `python 3.11.0`, `7-Zip 25.01`.
- **Reverse Engineering Tools:**
  - **Ghidra `12.1.4 PUBLIC`** (`$GHIDRA_INSTALL_DIR`) with **`ghidra-xbe` (`XboxExecutableLoader` v12.1.4)** installed in `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions\`. **Gotcha:** override `JAVA_HOME` to a JDK 21+ installation (e.g. Eclipse Temurin JDK 25) for headless `analyzeHeadless.bat` if system `JAVA_HOME` is Java 8.
  - **IDA Pro `9.4`** with `loaders\xbe.dll` and headless `idat.exe`.
- **WSL2 Ubuntu 22.04.5 LTS Environment:**
  - Use the host Ubuntu 22.04.5 LTS WSL2 distro (`wsl -l -v`; default WSL distro is `docker-desktop`, so always pass `-d <ubuntu2204-distro>`).
  - Kernel `6.6.87.2-microsoft-standard-WSL2`, **`glibc 2.35`**, `g++ 11.4.0` (`GLIBCXX_3.4.30`), `cmake 3.22.1`, `ninja 1.10.1`, `clang 14.0.0` / `clang-15 15.0.7`, `ld.lld 14.0.0` / `lld-15`, `glslang-dev 11.8.0`, `libvulkan-dev 1.3.204`, `mesa-vulkan-drivers 23.2.1` (`lavapipe` `llvmpipe` Vulkan `1.3.255` via `xvfb-run`), `patchelf 0.14.3`, and all 15 SDL3 dev packages pre-installed (`0` missing packages).

---

## 5. Binary-Verified Symbols, Hooks & SDK Fixes (Milestone 1)

### Ghidra 12.1.4 + Capstone Symbol Audit (`config/game.toml`)
- **Headless Ghidra Project:** `.local/ghidra/PsiOps` (created via `analyzeHeadless.bat` with `JAVA_HOME` pointing to JDK 21+, `ghidra-xbe` v12.1.4). Discovered `20,280` functions, `383` switch tables, `2,125` XDK/CRT symbols; **`180/180` exact match (`0` mismatches)** against `xbe_symbols` output.
- **Recompilation Scope:** `partitions = 128`, `9` code sections (`.text`, `D3D`, `XGRPH`, `DSOUND`, `PSGSFD00`, `PSGSFD_I`, `PSGSFD_B`, `PSGSFD_P`, `XPP`), `59,719` recompiled functions (`272` symbols in `--headers-only` mode).
- **Binary-Verified Added `[symbols]` (33 total beyond initial `xbe_symbols` pass):**
  - 4 initially missed/ambiguous: `CDirectSoundStream_AddRef = 0x4A0A5C`, `CDirectSoundStream_Release = 0x4A0AA3`, `D3DDevice_FlushVertexCache = 0x47EFD0` (NV2A `0x41710` vs NOP `0x47F1F0`), `XInitDevices = 0x4CA9D8`.
  - 3 Ghidra-verified D3D push/palette hooks: `D3DDevice_BeginPush = 0x484690`, `D3DDevice_EndPush = 0x4846B0`, `D3DDevice_SetPalette = 0x47EC80`.
  - 1 critical VB/palette sync hook (fixed missing Title Screen quads/text): **`D3D_BlockOnNonSurfaceResource = 0x484CA0`** (called by `D3DVertexBuffer_Lock2 @ 0x4836F0` and `D3DPalette_Lock2 @ 0x4837E0`; prevents CPU from overwriting dynamic VBs before async `render_thread` consumes queued draws).
  - 25 Ghidra+Capstone ABI-verified optional XDK 5849 HLE hooks: `D3DDevice_Reset = 0x47D870` (prevents `XMemFree` on host `m_pAutoDepthStencil`), `D3DDevice_RunPushBuffer = 0x48E2E0`, `D3DDevice_DrawIndexedVerticesUP = 0x4827C0`, `D3DDevice_SetBackBufferScale = 0x47C400`, `XInputGetCapabilities = 0x4CB84B`, `DirectSoundGetSampleTime = 0x49F7F9`, `IDirectSound_CreateSoundStream = 0x4A4F4B`, `IDirectSound_GetCaps = 0x4A19B4`, `IDirectSound_SetAllParameters = 0x4A4F6F`, `IDirectSoundBuffer_AddRef = 0x49F7A1`, `IDirectSoundBuffer_SetNotificationPositions = 0x4A1DAA`, plus 14 additional `IDirectSoundBuffer_*` / `CDirectSoundStream_*` methods.
- **Extra Entry Points (`[functions]`):** `"0x384C3D" = "crt_thread_notify"`, `"0x37ADA1" = "crt_thread_notify_cb"` (indirectly called via `XapiCallThreadNotifyRoutines`).
- **Game Hook (`[[hook]]` in `src/game/hooks.cpp`):** `at = 0x3FFD00`, `call = "Hook_AdxSpinYield"` — CRI ADX (`ADXXB` v1.49) spin-blocker thread (`0x003FFCF0..0x003FFD12`, priority `2`) makes zero kernel calls inside its spin loop; calling `kernel::SuspendPoint(); std::this_thread::yield();` allows `ADXM_Unlock` (`SuspendThread`) to park it cooperatively on both Windows and Linux.

### SDK Fixes Recorded in Pure-LF `patches/xboxxberecomp.patch`
- `runtime/kernel/threads.cpp`:
  - `MarkExited`: writes `W32(t.ethread + 0x120, code)` so `XAPILIB GetExitCodeThread` (`0x0036E198`) reads the true exit status (`0xF0000001`), fixing CRI ADX `adxm_destroy_thrd` (`0x3FFEA0`) stalls after FMV playback.
  - `ApplyThreadPriority` (`#ifdef _WIN32`): full Win32 priority mapping (`>=15 -> TIME_CRITICAL`, `>=2 -> HIGHEST`, `>=1 -> ABOVE_NORMAL`, `<0 -> BELOW_NORMAL`, `<=-2 -> LOWEST`, `<=-15 -> IDLE`).
- `runtime/xdk/dsound.cpp`:
  - Implemented `IDirectSoundBuffer_SetNotificationPositions` HLE (`SYM_IDirectSoundBuffer_SetNotificationPositions`, `Voice::NotifyPos` list, `SignalStopNotifies` on `Stop`/`StopEx`/end-of-playback, `CheckPosNotifies` on wrap & cursor advance) — fixes post-FMV Title Screen transition crash (`0x4A1DAA`).
- **Patch Integrity:** `patches/xboxxberecomp.patch` (`914,815` B, pure LF / `0` CR bytes, `SHA-256 = dc458900261f897a8e4ab11151d17847968b6bb9b6358c2f97f8eab2b6f38942`, verified byte-for-byte against `git -C XboxXBERecomp diff --no-ext-diff --binary cc9edda3a821a4204dc28783f429b88a27ade4f9` and `git apply --check --reverse`).

---

## 6. Milestone 2 — Active 3D Gameplay, `1.00x` Timing & Dual-Platform Parity

### Binary-Verified Main Loop, Timing & Phase Telemetry VAs (`default.xbe`)
- **`PresentationInterval` & Clock Source (`1.00x` Exact Match):** `Direct3D_CreateDevice` (`0x0030F425`) & `D3DDevice_Reset` (`0x002F6B90..0x002F6CF0`) configure `D3DPRESENT_PARAMETERS @ 0x005CC710` (`640x480`, `A8R8G8B8`, `BackBufferCount=2`, `D24S8`, `FullScreen_PresentationInterval = 1` = 60 Hz at `[0x005CC740]`). `QueryPerformanceCounter` (`0x0036B658`) reads `rdtsc` with `QueryPerformanceFrequency` (`0x0036B669`) = `733,333,333 Hz`, matching `XboxXBERecomp`'s `x86_rdtsc()` at exact `1.00x`.
- **`GameApp` Singleton (`app = R32(0x00586A04)`, `vtable = 0x004F4B8C`):**
  - `dt_s = F32(app + 0x38)` (variable `rdtsc/QPF` delta; fallback `0.0333333f` on reset `[app + 0x1C]`).
  - `real_s = double(R64(app + 0x40)) + double(F32(app + 0x48))` (accumulated every frame).
  - `sim_s = double(R64(app + 0x50)) + double(F32(app + 0x58))` (accumulated only when `state == 0`, freezes cleanly in objectives/pause).
  - `state = R32(app + 0x70)` (`0` = active sim, `4` = Mission Objectives popup, `6` = Pause Menu, `8` = level init).
  - `world = R32(app + 0xD4)` (non-null in 3D scene; camera `R32(world + 0x128)`), `g_frame = R32(R32(0x005DEFC8) + 0x1C)`.
- **Frontend vs Gameplay & FMV Detection (`src/common/game_info.cpp` `QueryPsiOpsPhase`):**
  - `fmv_active`: `R32(0x005C4198) != 0 && R32(0x005C4250) == 0`.
  - `GameStatus` singleton @ `0x005ECC78` (init guard `G<uint8_t>(0x005ED2B8) & 1`): **`in_shell = G<uint8_t>(0x005ECEA7)`** (`1` in Frontend/Shell `level97`, `0` in all playable levels `level00`/`level01`/`level50`), `in_training = G<uint8_t>(0x005ECEA6)`.
  - Wired via `perf::SetPhaseCallback(QueryPsiOpsPhase)` in `OnPsiOpsInit()`.
- **Save / HDD Check & Thread Concurrency Audit:**
  - `cXBoxSaveScribe` (`0x00121930`) checks the high 32 bits of `GetDiskFreeSpaceExA` `FreeBytesAvailable` first (`files.cpp`'s `8 GiB` `hi=2` passes cleanly without 32-bit overflow).
  - Full `.text -> D3D/XGRPH/DSOUND/XPP` scan confirmed `0` missing critical XDK `5849` hooks and `0` unmitigated spin loops across all 8 `CreateThread` sites (`Hook_AdxSpinYield @ 0x3FFD00` covers the sole spin-blocker; MWSFD FMV loop sleeps on `D3DDevice_BlockUntilVerticalBlank`).

### Cross-Platform Runtime Autotest & Verification Suite
- **SDK Autotest Extension (`patches/xboxxberecomp.patch` — `runtime/core/input.{h,cpp}`, `runtime/gpu/{d3d12,vulkan}.cpp`):**
  - Zero OS-level `SendInput`: reads `XBE_AUTOTEST_SCRIPT` and `XBE_AUTOTEST_OUT_DIR` inside `input::Poll(0)` (forcing Port 0 connected) and `input::AutotestOnPresent()` on both Win64 D3D12 (`WM_CLOSE` clean exit) and Linux Vulkan 1.2 (`SDL_EVENT_QUIT` clean exit).
  - Supports both present-frame-indexed (`pad`, `capture`, `exit`) and wall-clock-ms-indexed (`pad_ms`, `capture_ms`, `exit_ms` from first `Present()`) timelines so 60 FPS Win64 D3D12 and software Linux Vulkan (`llvmpipe`) stay synchronized to Psi-Ops's `1.00x` real-time timeline.
- **Automated Verification Suite (`tools/test_m2_gameplay.py`):**
  - Supports `--platform win64|linux|both --runs <N> --require-speed-ratio`; enforces `0` running `PsiOps` processes before/after every run, hard timeouts (`<= 120s`), multi-checkpoint PNG validation (`title`, `main_menu`, `cutscene`, `gameplay_a`, `gameplay_b`, `pause`, `resume`), frame-diff pixel motion proofs, `0` crash/D3D12/Vulkan/`glslang` errors, audio & `Disc::BlockCache` telemetry, `1.00x` `gameplay_active.g_elapsed_to_wall_ratio` bounds (`[0.85, 1.15]`), and source media `mtime_ns` immutability.

### Verified Milestone 2 Build & Repeatable Runtime Matrix (`--platform both --runs 2 --require-speed-ratio`)
| Target / Run | Binary / Resolution | Wall | `gameplay_active` Frames | Avg FPS / `g_dt_p50` | `g_elapsed/wall` | Disc / Audio | Shaders / Errors |
|---|---|---:|---:|---:|---:|---|---|
| Win64 `--headers-only` | `out/build/headers-only-win/PsiOps.exe` (`5,526,016` B) | — | — | — | — | — | `exit 0` (`-DPSIOPS_ALLOW_NO_XBE=ON`) |
| Linux `--headers-only` | `out/build/headers-only-linux/PsiOps` (`15,707,568` B) | — | — | — | — | — | `exit 0`, max `GLIBC_2.35` |
| Win64 D3D12 Run 1 | `PsiOps.exe` (`5.53 MB`) + `PsiOps.dll` (`23.27 MB`), `2560x1440` | `74.30s` | `1,479` | `59.79` / `16.67 ms` | **`0.9959`** | `327` reads (`50.10 MB`) / `7,373` blocks, `8` voices | `0` compiles, `0` errors, `7/7` PNGs |
| Win64 D3D12 Run 2 | `PsiOps.exe` + `PsiOps.dll`, `2560x1440` | `74.25s` | `1,210` | `59.74` / `16.67 ms` | **`0.9949`** | `335` reads (`51.37 MB`) / `7,372` blocks, `8` voices | `0` compiles, `0` errors, PASS |
| Linux Vulkan 1.2 Run 1 | `PsiOps` (`15.71 MB`, max `GLIBC_2.35`) + freestanding `libPsiOps.so` (`36.47 MB`, `NEEDED=[]`), `640x480` `llvmpipe` | `75.42s` | `760` | `31.30` / `30.61 ms` | **`0.9441`** | `325` reads (`49.76 MB`) / `7,315` blocks, `8` voices | `0` compiles, `0` errors, `7/7` PNGs |
| Linux Vulkan 1.2 Run 2 | `PsiOps` + `libPsiOps.so`, `640x480` `llvmpipe` | `74.94s` | `734` | `30.24` / `30.39 ms` | **`0.9411`** | `325` reads (`49.77 MB`) / `7,295` blocks, `8` voices | `0` compiles, `0` errors, `7/7` PNGs |

---

## 7. Milestone 3 — Controller Polish, Native-PC Mouse & Keyboard Controls & Deterministic Replay

### Binary-Verified Psi-Ops Control Layout, Camera & Telemetry VAs (`default.xbe`, XDK 5849)
- **Control Layout (`InputManager @ 0x002939C0`, active layout `GameStatus+0x23C @ 0x005ECEB4`):**
  - `2` = Config 1 (Default), `3` = Config 2, `4` = Config 3.
  - Config 1 mapping wired in `PsiOpsActionMap` (`src/common/game_info.cpp`, adapts dynamically to Config 2/3): `LS` Move (`WASD` diagonal-normalized `1.000`), `RT` Fire (`LMB`/`F`), `LT` Use Active Psi / Telekinesis (`LShift`/`Mouse5`/`1`), `A` Interact/Use/Climb/Accept (`E`/`Enter`), `B` Crouch/Sneak (`LCtrl`/`C`), `X` Jump/Melee/Vault (`Space`/`Q`/`Mouse4`), `Y` Target Lock-On / Sniper Scope Toggle (`RMB`/`V`), `DPad_Down` Reload (`R`/`MMB`) & Cycle Weapon (`WheelUp`/`WheelDown`; switches to `DPad_Up`/`DPad_Down` Sniper Zoom In/Out when scope active), `Start` Pause (latched `Escape`), `Back` Objectives (`Tab`).
  - **Direct 1-Key Psi Powers (`1..6`, dedicated guest action IDs `0x12..0x17`):** `1`=TK (`LT`), `2`=PK (`White` C1 / `DPad_Up` C2-C3), `3`=MD (`Black` C1 / `White` C2-C3), `4`=MC (`DPad_Up` C1 / `DPad_Down` C2-C3), `5`=RV (`DPad_Left`), `6`=AV (`DPad_Right`).
- **Guest Camera Pipeline & `[[hook]]` VAs (`config/game.toml` + `src/game/hooks.cpp`):**
  - Coordinate system: right-handed Y-up, `+Z` forward, **radians**, pitch clamp `[-1.2217305f, +1.2217305f]` (`±70.0°` @ `0x0051F978`/`0x0051F974`), no native slew/momentum carry-over (`0.000°` stop-drift).
  - Guest options: `GlobalOption[8] @ 0x005ECD5C` (guest Invert-Y, untouched by PC mouse Invert-Y), `GlobalOption[13]/[14] @ 0x005ECD70/0x005ECD74` (yaw/pitch speed dividers @ `0x005CD288`/`0x005CD28C`), `lookSpring @ 0x005ECEB8` (suppressed while M&K active, restored for controller).
  - `[[hook]] at = 0x04EED0` (`Hook_CameraApplyManualAim` — `Camera::ApplyManualAim`, covers 3rd-person, Sniper Scope with `20°..4°` FOV scaling via `[cam+0x37C]/[cam+0x380]`, Mind Control, and TK camera look): divides raw radian mouse delta by `(spd * dt)` to cancel guest `dt` & stick speed algebraically -> `0` deadzone loss (`1px = 0.000800 rad`, `24.94 cm/360 @ 800 DPI`), `0.000000 rad` stop-drift, and `0.000000 rad` 60/30 FPS `dt` invariance; captures exact live `Camera*` (`ctx.edi`) for telemetry (`[cam+0x2D8]` yaw, `[cam+0x2DC]` pitch).
  - `[[hook]] at = 0x115E3C` (`Hook_RemoteViewAim` — `PsiPower::RemoteViewUpdate -> QuatFromEuler`): adds `(d_pitch, d_yaw)` directly to `[esp+0]/[esp+4]`.
  - `[[hook]] at = 0x0FC680` (`Hook_UpdateTKPitch` — `Player::UpdateTKPitch`): updates TK held-object elevation `[player+0x744]` clamped to `±70°` during active TK (`psi_fn in {0x116BE0, 0x117380, 0x1174F0}`).
  - `[[hook]] at = 0x04F4F0` (`Hook_CameraAutoSteerBypass`, `on_true = "ret"` — `Camera::AutoSteerToPoint`): when M&K + `Raw 1:1 Aim` enabled, zeroes `*out_yaw`/`*out_pitch` and returns early (`ret 0x1C`).
- **Smart Cursor Gating & `--state-log` VAs (`src/common/game_info.cpp`):**
  - Modal UI gate: `HUD* @ R32(0x005CCB90)` (`hud+0x321` gameplay active, `hud+0x324` modal lock, `hud+0x3B4` pause open, `hud+0x3B5` objectives open). Cursor captures ONLY in focused, unpaused, non-overlay, non-modal `GameplayActive`.
  - Player/camera/vitals telemetry: `player = R32(app+0xD0)` -> transform `tr = R32(player+0x70)` (`+0x04/+0x08/+0x0C` XYZ), `health @ +0x770`, `psi @ +0x778`, weapon `wpn = R32(R32(player+0x788)+0x04)` (`clip @ +0x48`, `reserve @ R32(wpn+0x44)`), active Psi `psi_fn = R32(R32(player+0x6F4)+0x04)`, live camera `[cam+0x2D8/+0x2DC]`, guest Invert-Y `R32(0x005ECD5C) == 0`.

### Host Layer, Controller Audit & Deterministic Replay Modules
- **SDK Extensions (`patches/xboxxberecomp.patch`, `925,820` B, pure LF / `0` CR bytes, `SHA-256 = af4b255ff4076caddee2c89ead4920834856bd303ec2fc85bc28c5e966d7e97c`, `git apply --reverse --check` rc=0):**
  - `runtime/core/input.{h,cpp}`: `gamecontrollerdb.txt` auto-loading, DualSense 5/Edge/Xbox/Switch/Steam Deck identification, optional radial stick deadzones, sustained `(left||right)?65535u:0u` SDL3 rumble keepalive (3s refresh), `rumble_strength` scaling, `TestRumblePulse()`, `SetPresentCallback()` / `SetShutdownCallback()`, and motor-stop `Shutdown()`.
  - `runtime/app/{app.h,settings.cpp,main.cpp,main_linux.cpp}`: `app::RequestExit()`, `--settings <path>` / `XBE_SETTINGS_PATH`, `--save-settings-exit`, and `XBE_MKB_AUTOTEST`/`--replay-*`/`--state-log` launcher bypass.
- **Deterministic `.rec` Recorder/Replayer (`src/common/input_recorder.{h,cpp}`):**
  - Host-boundary `XBEREC01` recorder/replayer supporting `--record-input`, `--replay-input`, `--replay-mode hybrid|frame|time` (default `hybrid`: stall-clamped `eff_us <= 100 ms/frame` + 2-frame button-edge latch), `--replay-exit`, `--state-log <file.jsonl>`, and `ShouldBypassMkb()` isolation guard.
- **Cross-Platform Raw M&K Host Layer (`src/common/mkb_input.{h,cpp}`):**
  - Win32 `WM_INPUT` Raw Input + Linux SDL3 relative mouse/scancodes, `Auto` hot-switching (`auto`/`mkb`/`controller`) with virtual Port 0 keepalive, Smart OS Cursor Capture/Release (skips `ClipCursor` in headless autotest), portable OS-independent `[controls]` & `[controls.bindings]` (22 actions x `[Primary, Secondary]` string tokens, `0` `VK_*`/`SDL_SCANCODE_*` literals), 4-column Click-to-Rebind ImGui panel (`F1` overlay), and in-process `XBE_MKB_AUTOTEST` driver (`key_ms`, `mouse_ms`, `wheel_ms`, `pad_ms`, `set_ms`, `capture_ms`, `exit_ms`).

### Verified Milestone 3 Build & Dual-Platform Verification Matrix
- **All 5 Targets Built Cleanly (`exit 0`, `0` warnings/errors):**
  - Win64 Release: `out/build/release/PsiOps.exe` (`5,631,488` B) + `PsiOps.dll` (`23,273,984` B)
  - Win64 Debug: `out/build/debug/PsiOps.exe` (`8,712,704` B) + `PsiOps.dll` (`23,275,520` B)
  - Linux Release: `out/build/linux-release/PsiOps` (`15,814,296` B, max `GLIBC_2.35`) + `libPsiOps.so` (`35,511,824` B, `NEEDED = []` freestanding)
  - Win64 `--headers-only`: `out/build/headers-only-win/PsiOps.exe` (`5,631,488` B)
  - Linux `--headers-only`: `out/build/headers-only-linux/PsiOps` (`15,814,296` B, max `GLIBC_2.35`)
- **Native-PC M&K Suite (`python tools/test_m3_mkb.py --platform both --timeout 180` -> `exit 0`, `2/2 PASS`, `0` warnings):**
  - **Win64 D3D12 (`74.53s`, `4,089` frames) & Linux Vulkan 1.2 (`76.80s`, `2,632` frames) — identical metrics:** `1px` micro-aim `d_yaw = -0.000800 rad` / `d_pitch = +0.000800 rad` (`24.9364 cm/360 @ 800 DPI`), `+400px` flick `-0.320000 rad` with `0.000000 rad` next-frame stop-drift, `60 FPS` vs `30 FPS` `+100px` `dt_diff_rad = 0.000000 rad`, decoupled PC Invert-Y `+0.04` / `-0.04` (`guest_invert_y` invariant), `WASD` `1.00000` on all 5 cardinal/diagonal directions, `WheelUp`/`WheelDown`, direct `1..6` Psi, `Auto` hot-switch (`mkb <-> controller`), `11/11` Smart Cursor capture/release states, latched `Escape`, and cross-platform Win64 <-> Linux `settings_portable.toml` `--save-settings-exit` parity.
- **Deterministic Controller Replay Suite (`python tools/test_m3_replay.py --platform both --timeout 210` -> `exit 0`, `9/9 PASS`):**
  - Full lifecycle recorded to `.local/m3_replay/m3_session.rec` (`291,752` B): `Title -> Main Menu -> New Game -> Level 1 Gameplay (move/aim/fire/rumble) -> Pause -> Resume -> Pause -> Restart Checkpoint (`sim_time_resets=1`) -> Gameplay -> Pause -> Go To Main Menu (`Save Before Quitting? -> No`, `returned_to_shell=True`) -> Title/Main Menu -> Exit(0)`.
  - All 9 runs PASS with zero crashes/errors/leftover processes: `record_reference` (`96.31s`), `win64_release_hybrid_run1` (`96.25s`), `win64_release_hybrid_run2` (`99.12s`), `win64_release_frame` (`115.06s`), `win64_release_time` (`96.33s`), `win64_release_cold_cache` (`XBE_NO_SHADER_CACHE=1`, `96.88s`), `win64_release_mkb_isolation` (`106.03s`, `100%` M&K noise bypassed), `win64_debug_hybrid` (`97.97s`), and `linux_release_hybrid` (`102.73s`, WSL2 Ubuntu 22.04 Vulkan 1.2).

## Milestone 4 — Cross-Hardware Performance, 60 FPS Fidelity & Cutscene Stutter Elimination
- **Timing, Havok & CRI Sofdec Ghidra RE:**
  - Native Xbox `D3DDevice_Reset` (`0x002F6CE4`) runs `D3DPRESENT_INTERVAL_ONE` (**60 Hz VSync**), triple-buffered `D3DFMT_A8R8G8B8` + `D3DFMT_D24S8`.
  - `GameApp::UpdateTiming (0x000D9F50)` writes raw QPC `dt` to `[app+0x38]`; `PhysicsWorld::Step (0x00305260)` -> `hkWorld::stepDeltaTime (0x00352330)` runs `numSteps = max(1, (int)(accum * 60.0f + 0.5f))` with a **`1/120 s` (`8.333 ms`) guard (`0x003052CE`)** that skips Havok on alternating frames at `>=120 FPS` and double-stacks unscaled TK impulses (`0x002E0A10`). Particle emitters (`0x002EF1E0`) run fixed `30 Hz` accumulator; Python 2.2.1 (`0x00326E30`) and animations (`0x00288910`) scale continuously by `dt`. **Verdict:** **`60 FPS`** is optimal default (1:1 Havok lockstep), **`30 FPS`** is 100% safe fallback (`numSteps=2 @ 16.67 ms`), **`>60 FPS`** is unsafe.
  - CRI Sofdec (`0x0026C1C0..0x0026D3B9`): **100% `D3DFMT_YUY2 (0x24)`** ping-pong textures (accelerated by SSE2 `DecodeYuv422RowSse2` in `texture.cpp`), triple-caller `D3DDevice_BlockUntilVerticalBlank` (`main`, `vsync`, `fs`) woken simultaneously by shared 60 Hz VBlank CV `notify_all()`, `LockRect(..., Flags=0)` with 0 blocking GPU flushes, and `128 KiB` overlapped `ReadFileEx` chunks for `.msx` audio and `.xbox` (`ESPx`) cutscene streaming (accelerated by `16 MiB` `Disc::BlockCache`).
- **Optimizations & Telemetry (`XboxXBERecomp/` + `tools/test_m4_perf.py`):**
  - **Zero-Stall Dynamic VB Snapshotting (`state.h`, `vertex.cpp`, `render_thread.cpp`):** Dynamic vertex buffers locked via `D3D_BlockOnNonSurfaceResource` / `WaitResource` are snapshotted slice-exact into recycled `Batch::bytes` at `Draw()` time and consumed directly by `gpu::Assemble` on both D3D12 and Vulkan. Eliminated **100% of render-thread resource lock stalls**: Win64 warm **`287,414` -> `0`** (`3,222.84 ms` -> `0.000 ms`), Linux warm **`117,496` -> `0`** (`47,863.10 ms` -> `0.000 ms`), cutting Win64 gameplay guest CPU `-29%` (`1.580 -> 1.122 ms`), `p99` `18.50 -> 17.32 ms`, and Linux gameplay `p50` `31.60 -> 29.40 ms` (`28.84 -> 30.34 FPS`). Visually verified pixel-exact Title Screen, Main Menu, and 3D Gameplay via `tools/test_m2_gameplay.py --platform win64`.
  - **PE High-Performance GPU Exports (`main.cpp`):** Exported `NvOptimusEnablement = 0x00000001` and `AmdPowerXpressRequestHighPerformance = 1` on `PsiOps.exe` (verified via `pefile`).
  - **Multi-GPU, Pacing Policy & Cgroup-Aware `PrepareGameCode` CLIs (`app.h`, `settings.cpp`, `game_code.cpp`, `main.cpp`, `main_linux.cpp`):** Added `--list-gpus`, `--gpu <idx>`, `--simulate-refresh-hz`, `--query-pacing` (shared `ComputePacingPolicy` used by D3D12/Vulkan/CLI), `--query-compile-jobs` (`768 MiB`/slot + Linux cgroup v2 `memory.max`/`memory.current`).
  - **11-Metric Telemetry & Checkpoint-Safe Speed Ratio (`perf.h`, `perf.cpp`, `disc.cpp`, `d3d12.cpp`, `vulkan.cpp`):** Fixed `g_elapsed_to_wall_ratio` across `RESTART CHECKPOINT` resets (`0.2351` -> `1.0023`); instrumented `lock_stalls`, `mid_frame_flushes`, `boot_prewarm_ms`, `sync_present_cache_writes`, `async_cache_writes`, `av_sync_ratio`, `disc_cache_hit_pct`, `disc_os_reads`, `io_avg/p99/max_us`, `gpu_slab_count/mb/allocs`, and `upload_ring_mb`.
- **Verified Post-Optimization Results (`tools/test_m4_perf.py --mode post --platform both` -> `exit 0`, `passed=True` + `--mode compare` -> `.local/m4_perf/pre_vs_post_comparison.md`):**
  - **Win64 Warm 60 FPS (RTX 3090 `10DE:2204`, `64 MiB` ring, `1440p`):** Gameplay `58.47 FPS`, `p50=16.66 ms`, `p99=17.32 ms` (pre `18.50`), `1% low=16.19 FPS`, `guest_cpu=1.12 ms` (pre `1.58`), `gpu=0.33 ms`, `lock_stalls=0` (pre `287,414`), `mid_frame_flushes=0`, `boot_prewarm=2.54 ms`, `compiles=0`, `psos=0`, `sync_present_writes=0`, `audio_underruns=0`, `glitches=0`, `av_sync=0.9956`, `disc_cache_hit=40.07%`, `io_avg=22.86 us`, `io_p99=98.71 us`, `gpu_slabs=1 (32 MiB)`, `sim_ratio=1.0023`. FMV: `58.31 FPS`, `p50=16.66 ms`, `p99=17.20 ms`, `1% low=15.56 FPS` (pre `14.78`), `max=183.87 ms` (pre `200.55`).
  - **Win64 Warm 60 FPS GPU 1 UMA (`AMD Radeon(TM) Graphics` `1002:13C0`, `32 MiB` ring, `1440p`):** Gameplay `58.47 FPS`, `p50=16.67 ms`, `p99=17.71 ms`, `gpu=6.47 ms`, `lock_stalls=0`, `boot_prewarm=1.94 ms`, `compiles=0`, `psos=0`, `gpu_slabs=1 (32 MiB)`, `sim_ratio=1.0023`; per-VID/DID caches `psos_1002_13C0.bin` (`209,636 B`) + `pso_meta_1002_13C0.bin` (`8,152 B`).
  - **Linux Warm 60 FPS (`llvmpipe` Vulkan 1.2 `10005:0000`, `32 MiB` ring, `640x480`):** Gameplay `30.34 FPS` (pre `28.84`), `p50=29.40 ms` (pre `31.60`), `1% low=3.86 FPS` (pre `3.65`), `p99.9=400.55 ms` (pre `444.70`), `lock_stalls=0` (pre `117,496`), `mid_frame_flushes=0`, `boot_prewarm=170.36 ms`, `compiles=0`, `psos=0`, `audio_underruns=0`, `gpu_slabs=1 (32 MiB)`, `sim_ratio=0.9998`; caches `vk_shaders.bin` (`702,148 B`), `vk_psos_10005_0000.bin` (`76 B`), `vk_pso_meta_10005_0000.bin` (`9,760 B`).
  - **60 vs 30 FPS 1.00x Fidelity (`win64_warm_60fps` vs `win64_warm_30fps`):** `ratio_60=1.0023`, `ratio_30=0.9996`, `586` matched timestamps, **`median_world_distance_delta = 0.0205 m`**, **`max_yaw_delta_rad = 0.00034 rad`**, `vitals_match = true`.
  - **Pacing & Compile-Jobs Matrices:** `40/40` Win64+Linux pacing cases PASS (`60/90/120/144/165 Hz` x `auto/vsync/vrr/mailbox`, `double_vblank_wait=false`); `10/10` Win64+Linux RAM-bounded `PrepareGameCode` cases PASS (`4GB->4`, `8GB->8`, `16GB->14`, `64GB->14`, `--compile-jobs 3 -> 3`).
  - **Updated Patch & All 5 Builds:** `patches/xboxxberecomp.patch` (`968,360` B, `0` CR bytes, `SHA-256 = 907acb1e2fab446d0553b5d27b06a6567e3a08b97776d96f6fdf22353522440b`, `git apply --reverse --check` rc=0, `0` foreign hits); `release/PsiOps.exe` (`5,662,720` B) + `PsiOps.dll` (`23,274,496` B), `debug/PsiOps.exe` (`8,744,448` B) + `PsiOps.dll` (`23,275,520` B), `linux-release/PsiOps` (`15,847,384` B, max `GLIBC_2.35`) + `libPsiOps.so` (`36,468,720` B, freestanding `NEEDED = []`), `headers-only-win/PsiOps.exe` (`5,662,720` B), `headers-only-linux/PsiOps` (`15,847,384` B, max `GLIBC_2.35`).

---

## 9. Milestone 5 — Pre-Game Launcher, 9-Upscaler Suite (Bundled Retail DLLs), F1 Overlay & Steam Deck UX

### Binary-Verified Psi-Ops Aspect Fix, Live 3D Camera & 3D/HUD Boundary (`default.xbe`, XDK 5849)
- **True `16:9` Square-Pixel Widescreen Fix (`ApplyPsiOpsAspectFix` in `src/common/game_info.cpp`):**
  - `renderer = R32(0x005DEFC8)` (set at `0x002F6C76..0x002F6C89`): writes `4.0f/3.0f` (`1.333333f`) to `F32(renderer + 0x10)` and `F32(0x005CCBCC)` when `aspect_correct_render` is active, eliminating Xbox `640x480` CRT PAR stretch so circular reticles and `16:9` widescreen render geometrically square.
- **Live 3D Camera Matrices & Temporal Cut Detection (`QueryPsiOpsCamera` in `src/common/game_info.cpp`):**
  - Reads live View matrix (`4x4` float at `renderer + 0x24`), Projection matrix (`renderer + 0xA4`), active camera `g_active_camera = R32(0x005CC6E8)`, and `fovY = F32(cam + 0x80)`.
  - Camera-cut reset thresholds (`src/upscale/upscaler_common.cpp`): translation `> 1.5` world units (`dist_sq > 2.25f`), view forward dot `< 0.70f` (`> ~45 deg`), or FOV jump `> 0.087 rad` (`> ~5 deg`, e.g., Sniper Scope toggle).
  - 3D/HUD reactive-mask boundary (`0x00315480`/`0x003154A0`): `cs_temporal_prep` generates `RG16F` camera motion vectors, `R8_UNORM` HUD/2D reactive mask, and clean `R32_FLOAT` reversed-Z depth; 2D/menu/FMV frames bypass cleanly to `FSR 1.0`.

### 9-Upscaler Suite (`src/upscale/*`) & Bundled Retail Vendor DLLs (`plugins/`, git-ignored)
- **6 Zero-DLL Built-In Spatial Upscalers (Win64 D3D12 + Native Linux Vulkan 1.2 SPIR-V):**
  - `FSR 1.0` (EASU + RCAS), `NIS v1.0.3` (NVIDIA Image Scaling), `CAS` (Contrast Adaptive Sharpening), `Bicubic 5-Tap` (including `>100%` SSAA downsampling up to `200%`), `Integer Nearest`, and `Bilinear`, plus Xbox Gamma LUT compositing.
- **3 Hardware Temporal Backends on Win64 D3D12 (Clean-Room `GPL-2.0-or-later` `ngx_abi.h` / `ffx_abi.h` / `xess_abi.h`, lazy on-demand init):**
  - **NVIDIA DLSS 4** (`upscaler_dlss.cpp`): Presets `Auto`/`E`/`J`/`K` + `DLAA (100%)` via signed DriverStore `_nvngx.dll` (`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`) and retail `/rel/` `plugins/nvngx_dlss.dll` (`v310.9.16`, `58,956,912` B, `WinVerifyTrust OK`, signer `'NVIDIA Corporation'`, `0` dev watermarks) with Authenticode-verified `%ProgramData%\NVIDIA\NGX\models\dlss\` -> `cache\ngx\nvngx_dlss.dll` fallback.
  - **AMD FSR 3.1 / 4** (`upscaler_fsr3.cpp`): FidelityFX API `plugins/amd_fidelityfx_loader_dx12.dll` (`v2.4.0`, `39,184` B) + `plugins/amd_fidelityfx_upscaler_dx12.dll` (`v2.4.0`, `57,664,272` B, `WinVerifyTrust OK`, signer `'Advanced Micro Devices'`).
  - **Intel XeSS 2** (`upscaler_xess.cpp`): `plugins/libxess.dll` (`v2.1.1`, `77,795,704` B, `WinVerifyTrust OK`, signer `'Intel Corporation'`, `xess_abi.h` exact `XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6` and `D3D12ExecuteParams` ABI).
  - **Linux Vulkan 1.2 Graceful Fallback (`upscaler_vulkan.cpp`):** All 6 spatials compiled via `CompileHlslToSpirv`; requesting `dlss`/`fsr3`/`xess` falls back cleanly to `FSR 1.0` with `fallback=true` telemetry and zero errors.
- **5 Hardware Presets:** `Steam Deck (1280x800)`, `Handheld / iGPU`, `1080p Mainstream`, `1440p High`, `4K Ultra` (plus `Custom`).
- **Fetch & Verification Tooling:** `tools/fetch_upscalers.py` (git-ignored `plugins/` + 3 vendor licenses + offline PE64 / Authenticode / dev-watermark gate) and `tools/test_real_upscalers.py` (25-case live GPU verification matrix).

### 6-Tab Pre-Game Launcher, In-App File Browser, F1 Overlay + HUD, Icons & Steam Deck UX
- **Pre-Game Graphical Launcher (`src/common/launcher_ui.{h,cpp}` + `ui_common.{h,cpp}`):**
  - 6 tabs (`Play & Game Disc`, `Upscaling & Graphics`, `Display & GPU`, `Audio`, `Controls`, `Tools & Diagnostics`) with full gamepad (`LB/RB`, D-pad, `A`/`B`, `Start` with release debounce & nav priming), touch, and mouse navigation on Win32+D3D12 (`PerMonitorV2`) and Linux `SDL_Renderer` (`1280x800` auto-detected on `SteamDeck=1` / Gamescope).
  - Cleanly destroyed before game launch (`0` leaks; programmatic `DestroyWindow` suppresses `WM_DESTROY` `PostQuitMessage`, explicitly drains stray `WM_QUIT` on Win32 and `SDL_EVENT_QUIT`/`SDL_EVENT_WINDOW_*` on Linux, drops local DXGI swapchain/factory COM refs before teardown); writes **`0` `settings.toml` files** on unconfigured Exit; supports `--launcher`, `--no-launcher`, `--launcher-tab <0..5>`, `--open-file-browser`, `--capture-launcher-bmp`, and `--launcher-autoplay-ms` / `XBE_LAUNCHER_AUTOPLAY_MS`.
- **In-App XISO File Browser (`src/common/file_browser_ui.{h,cpp}`):**
  - Gamepad/touch/mouse directory navigator with Steam Deck microSD Quick-Jump (`/run/media/mmcblk0p1`, `/home/deck`, etc.) and live `ValidateDiscImage` Title ID (`0x4D570016`) + XBE SHA-1 validation badges.
- **Live `F1` / `BACK+L3` Overlay + Upgraded Live HUD (`src/common/settings_ui.{h,cpp}`):**
  - 5-tab live-synced settings overlay embedding `mkb::DrawControlsSettingsPanel()`, calling `mkb::SetOverlayOpen(bool)` for smart cursor release, plus upgraded HUD (`FPS | ms | Upscaler | Render -> Display | Input | F1 Settings`) and `--open-settings` / `--capture-ui-bmp`.
- **Branded Assets & Win32 PE Resources (`tools/gen_icon.py`, `resources/{psiops.png,psiops.ico,app.manifest,psiops.rc}`):**
  - Compiled via `llvm-rc.exe` with `-Xlinker /MANIFEST:NO` (zero duplicate manifest); embeds `RT_ICON`, `RT_GROUP_ICON` (7 sizes), `RT_MANIFEST` (`PerMonitorV2`), and `RT_VERSION` (`ProductName="Psi-Ops: The Mindgate Conspiracy"`).
- **Steam Deck Native Installer & Scripts (`packaging/steamdeck/{install-steamdeck.sh,psi-ops-recomp.desktop}`, `.local/{send_to_steamdeck,update_steamdeck}.ps1`):**
  - Adds native Linux `PsiOps` to Steam `shortcuts.vdf` (NEVER Proton!) with backup, idempotent re-install, and clean `--uninstall`; `--create-shortcut` sanitizes NTFS illegal `:` (`Psi-Ops - The Mindgate Conspiracy.lnk` / `psiops-recomp.desktop`).

### Verified Milestone 5 Builds & Full Test Matrix
- **All 5 Targets Built Cleanly (`exit 0`, `0` errors) + Pure-LF SDK Patch:**
  - `patches/xboxxberecomp.patch`: `1,102,879` B, `0` CR bytes, `SHA-256 = ada3e41f6f00e913ffa57ada32b9bad2683a1ce5f9cdca8ba5dd54778ae0bff2`, `git -C XboxXBERecomp apply --reverse --check ../patches/xboxxberecomp.patch` rc=0, `0` foreign string hits.
  - Win64 Release: `out/build/release/PsiOps.exe` (`8,556,032` B) + `PsiOps.dll` (`23,273,984` B) + `psiops.png` + `plugins/` (4 signed retail DLLs + 3 licenses)
  - Win64 Debug: `out/build/debug/PsiOps.exe` (`9,342,976` B) + `PsiOps.dll` (`23,275,520` B)
  - Linux Release: `out/build/linux-release/PsiOps` (`16,130,256` B, max `GLIBC_2.35`) + `libPsiOps.so` (`35,512,016` B, freestanding `NEEDED = []`) + `psiops.png`
  - Win64 `--headers-only`: `out/build/headers-only-win/PsiOps.exe` (`6,216,192` B)
  - Linux `--headers-only`: `out/build/headers-only-linux/PsiOps` (`16,130,256` B, max `GLIBC_2.35`)
- **Real-GPU 9-Upscaler Suite (`tools/test_real_upscalers.py` -> `25/25 PASS`, `exit 0`):**
  - **Plugin Integrity Gate:** `PASS` (all 4 retail DLLs PE32+ x64, `WinVerifyTrust OK`, vendor signers verified, `0` dev watermarks, all 3 licenses present).
  - **Win64 RTX 3090 D3D12 (`15/15 PASS`, `.local/m5_upscalers/m5_upscalers_report.json`):** `fsr1`, `nis`, `cas`, `bicubic` (`1280x720->1920x1080`), `bicubic_ssaa_150` (`2880x1620->1920x1080`), `integer`, `bilinear`, `dlss_auto` (`1280x720->1920x1080`), `dlss_preset_e` (`1128x634->1920x1080`), `dlss_preset_j` (`960x540->1920x1080`), `dlss_preset_k` (`1280x720->1920x1080`), `dlss_dlaa_100` (`1920x1080->1920x1080`), `fsr3` (`1280x720->1920x1080`), `xess` (`1280x720->1920x1080`), `dlss_ngx_cache_fallback` (`1280x720->1920x1080`, `%ProgramData%` -> `cache/ngx/nvngx_dlss.dll` PE64 + Authenticode verified).
  - **Linux Vulkan 1.2 `1280x800` (`10/10 PASS`, `.local/m5_upscalers_linux/m5_upscalers_report.json`):** `fsr1`, `nis`, `cas`, `bicubic`, `bicubic_ssaa_150` (`1920x1080->1280x720`), `integer`, `bilinear`, plus graceful `dlss`/`fsr3`/`xess` -> `fsr1` fallback.
- **UX, Packaging & Unpacked `PrepareGameCode` Suite (`tools/test_m5_ux.py` -> `8/8 PASS`, `exit 0`):**
  - Win64 (`.local/m5_ux/m5_ux_report.json`): `win32_pe_resources` (`RT_ICON`/`RT_GROUP_ICON`/`RT_VERSION`/`RT_MANIFEST` `PerMonitorV2`), `win64_validate_iso_and_shortcut` (`Psi-Ops - The Mindgate Conspiracy.lnk`), `win64_launcher_6tabs_and_browser` (`6/6` distinct tabs, `0` `settings.toml` writes on unconfigured Exit, File Browser + `F1` Overlay HUD), `win64_unpacked_prepare_game_code` (Run 1 local compile `51.3s` -> `23,273,984` B `PsiOps.dll`, Run 2 cached reuse `19.3s`).
  - Linux (`.local/m5_ux_linux/m5_ux_report.json`): `steamdeck_installer_vdf_roundtrip` (install + backup + idempotent re-install + `--uninstall` preserving unrelated game), `linux_validate_iso_and_shortcut` (`psiops-recomp.desktop`), `linux_launcher_6tabs_and_browser` (`6/6` distinct `1280x800` Steam Deck tabs, `0` `settings.toml` writes on Exit, File Browser + `F1` Overlay HUD), `linux_unpacked_prepare_game_code` (Run 1 local compile `49.0s` -> `36,468,720` B freestanding `libPsiOps.so`, Run 2 cached reuse `5.3s`).

---

## 10. Milestone 6 — Licensing, GitHub Actions CI, 6-Gate Release Audit & Reproducible Distribution

### Licensing, Documentation & CI Artifacts
- **License (`LICENSE`, `19,781` B, pure LF):** Host recompilation project is licensed under `GPL-2.0-or-later` (upstream `XboxXBERecomp` is `MIT`), plus the Additional Permission (Runtime Upscaler Plugin Exception under GPLv2 / GPLv3 Section 7) covering dynamically loaded vendor upscaler plugins (`nvngx_dlss.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `libxess.dll`).
- **Third-Party Attribution (`THIRD_PARTY_NOTICES.md`, `40,190` B, pure LF):** All 12 notices (XboxXBERecomp, Zydis & Zycore, toml++, Dear ImGui, SDL3, glslang & SPIRV-Tools, zlib, LLVM/Clang/LLD, Microsoft Visual C++ Runtime (MSVC CRT), AMD FidelityFX, NVIDIA RTX SDKs, Intel XeSS 2).
- **Documentation (`README.md`, `11,453` B, pure LF):** End-user and developer guide covering Win64 D3D12, Linux x86_64 / SteamOS 3.x Vulkan 1.2, Steam Deck native installation (`install-steamdeck.sh`), 9-upscaler suite, M&K + Controller + Replay controls, `--headers-only` clean build, and clean-room legal/trademark disclaimers.
- **GitHub Actions CI (`.github/workflows/build.yml`, `4,207` B, pure LF):** Automated Linux (`ubuntu-22.04`, `--headers-only`, `GLIBC <= 2.35`, `python3 tools/audit_release.py --ci --platform linux`) and Windows (`windows-2022`, LLVM/Clang, `--headers-only`, `release.bat --skip-build`, `python tools/audit_release.py --ci --platform win64`) workflows.
- **Git Configuration:** `.gitmodules` pins `XboxXBERecomp` at `cc9edda3a821a4204dc28783f429b88a27ade4f9` (`ignore = dirty`), `.gitattributes` enforces `* text=auto eol=lf` (CRLF only on `*.bat`/`*.cmd`), `.gitignore` excludes `/dist/`, `.local/`, `out/`, `plugins/`, and all disc/XBE/compiled game artifacts.

### Release Packaging & 6-Gate Audit (`release.bat`, `release_linux.sh`, `tools/audit_release.py`)
- **Win64 Release Packager (`release.bat`, CRLF):** Builds Win64 `--headers-only` (`PsiOps.exe`, `6,344,192` B), verifies signed retail `plugins/`, bundles `compiler/{xbe_recomp,clang,lld-link}.exe`, `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, `xdk/5849/{layout,signatures}.toml`, `config/game.toml`, `psiops.png`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `README.md` (strictly excluding `compiler/work` and `PsiOps.dll`).
  - `dist/Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.0.0-win64-launcher.zip`: `199,692,381` B (`36` files), `SHA-256 = 623fa742f1d9fcdbaadec5b80193c19a259f2095594f924a3c5adc5fbd0ca48b`.
- **Linux x86_64 / Steam Deck Packager (`release_linux.sh`, pure LF, `100755`):** Builds Linux `--headers-only` (`PsiOps`, `16,146,824` B, max `GLIBC_2.35`), dereferences `compiler/{xbe_recomp,clang,ld.lld}` + 12 non-core shared libraries into `compiler/lib/`, patches RPATHs (`$ORIGIN:$ORIGIN/lib` and `$ORIGIN`) via `patchelf`, enforces `0` symlinks, and includes `install-steamdeck.sh` + `psi-ops-recomp.desktop`.
  - `dist/Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.0.0-linux-x86_64.tar.gz`: `71,852,215` B (`0` symlinks, `12` isolated `compiler/lib/*.so*`), `SHA-256 = bf400325b462816b4e595ed27ef82c4d34b79263b0499b94d142757e4d751561`.
- **Clean Source Archive (`git archive --format=zip HEAD`):**
  - `dist/Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.0.0-source.zip` (zero binaries, zero `.xbe`/`.iso`, zero `plugins/`, exact `HEAD` snapshot).
- **6-Gate Audit Script (`tools/audit_release.py`, `GPL-2.0-or-later`, pure LF, `--ci --platform linux|win64`):**
  - Gate 1 (Repo & Git History Cleanliness + Pure-LF + Submodule SHA + Executable Bits + Zero Personal/Foreign/Companion Leaks)
  - Gate 2 (SDK Patch Fidelity — byte-exact against `XboxXBERecomp` diff, `0` CR bytes, `git apply --reverse --check` rc=0)
  - Gate 3 (`source.zip` Integrity — byte-for-byte match against `git ls-tree HEAD` honoring `.gitattributes` CRLF on `*.bat`/`*.cmd`)
  - Gate 4 (Win64 Launcher Zip — required layout, PE32+ x64 + Authenticode `WinVerifyTrust OK` + `0` dev watermarks on all 4 retail DLLs, `0` `PsiOps.dll`/`compiler/work`)
  - Gate 5 (Linux Tarball — required layout, `0` symlinks, pure-Python ELF64 `GLIBC <= 2.35` & `$ORIGIN:$ORIGIN/lib` / `$ORIGIN` RPATH resolution across all 12 `compiler/lib/*.so*`, `0` `libPsiOps.so`)
  - Gate 6 (Zero Leaked Copyrighted Assets & Untouched Source Media `mtime_ns`)

### Verified Unpacked Archives & Clean-Environment Rebuild
- **Unpacked Win64 Launcher Zip (`.local/m6_unpacked_win64_report.json`, LLVM stripped from `PATH`):** `--validate-iso` `rc=0` (`0x4D570016`, `XDK 5849`), headless Launcher capture `rc=0` + `zero_settings_write=true`, bundled `compiler/` Run 1 compiled `23,273,984` B `PsiOps.dll` in `29.58s` + `8,294,454` B UI BMP, cached Run 2 reused `PsiOps.dll` (`dll_mtime_unchanged=true`) in `4.03s`.
- **Unpacked Linux Tarball (`.local/m6_unpacked_linux_report.json`, `PATH=/usr/bin:/bin`):** `$ORIGIN/lib` RPATH isolated `100%` of `compiler/` deps, `--validate-iso` `rc=0`, Steam Deck `1280x800` Launcher capture `rc=0` + `zero_settings_write=true`, bundled `compiler/` Run 1 compiled `36,468,720` B freestanding `libPsiOps.so` (`NEEDED=[]`) in `40.54s`, cached Run 2 reused `libPsiOps.so` (`so_mtime_unchanged=true`) in `3.90s`.
- **Clean-Environment `source.zip` Rebuild (`.local/m6_clean_source_report.json`, zero `.xbe`/`.iso`, fresh `git clone` + `patches/xboxxberecomp.patch` + FetchContent):**
  - Win64 `--headers-only`: `rc=0`, `76.80s`, `6,017,536` B `PsiOps.exe`.
  - Linux `--headers-only`: `rc=0`, `169.02s`, `15,980,480` B `PsiOps`, max `GLIBC_2.35`.
- **Launcher -> PLAY PSI-OPS -> Live Game E2E Verification (`XBE_LAUNCHER_AUTOPLAY_MS=400`, `tools/test_m5_ux.py --skip-unpacked-compile`, `--no-launcher` NOT passed):**
  - **Win64 + Linux Full UX Suite (`8/8 PASS`, `exit 0`):** `win32_pe_resources`, `win64_validate_iso_and_shortcut`, `win64_launcher_6tabs_and_browser`, `win64_launcher_to_game_autoplay`, `steamdeck_installer_vdf_roundtrip`, `linux_validate_iso_and_shortcut`, `linux_launcher_6tabs_and_browser`, `linux_launcher_to_game_autoplay`.
- **PC Mouse Menu Navigation, Invisible In-Game OS Cursor & DualSense Edge `Auto` Noise Immunity (`tools/test_m3_mkb.py --platform both` -> `PASS` Win64 + Linux, `exit 0`):**
  - **Mouse Menus (`Hook_HudUpdate @ 0x0C1040` in `src/game/hooks.cpp` + `config/game.toml`):** Walks single embedded `UIManager` (`hud + 0xB0`) in binary-verified `512x465` (`y_off=+10`) Layer `0x17` ortho space with ancestor `TransformNode` (`vtable 0x004FEF84`) propagation, `POLY`/`TEXT`/`SLDP`/`LIST`/Options toggle-row hit-testing, hover highlight (`SetFocusedChild @ 0x0024CAD0`, `0` SFX spam), `LMB` select/drag/FMV skip, `RMB` back/resume (`SendUiEvent @ 0x0024DDC0`), `MouseWheel` up/down, and `BindActivePort @ 0x00294500` Port 0 tie-break heal (`0x0055673C`).
  - **Invisible GUI-Thread OS Cursor & DualSense Noise Immunity (`src/common/mkb_input.{h,cpp}`, `XboxXBERecomp/runtime/app/{main,main_linux}.cpp`, `runtime/core/input.cpp`):** Marshals `ShowCursor`/`SetCursor`/`ClipCursor` (`WM_APP+3`) and `SDL_HideCursor`/`SDL_SetWindowRelativeMouseMode` (`SDL_EVENT_USER`) onto the owning window thread, intercepts `WM_SETCURSOR` + sets `ImGuiConfigFlags_NoMouseCursorChange` when overlay is closed, queries real `GetCursorInfo()` / `SDL_CursorVisible()` into `os_cursor_visible`, enforces minimum physical actuation (`>0.25f` stick / `>45` trigger / press-edge) in `Auto` mode (`15%` default deadzones), and clears physical `Pad` state when `Mkb` owns Port 0 (`0` flip-flops across 11s continuous noisy DualSense Edge stress at `deadzone=0`).
- **Cold-Start AOT Shader, PSO & Upscaler Pre-Compilation (`XboxXBERecomp/runtime/gpu/shader_precompile.{h,cpp}`, `kAotSchemaVersion=4`, `tools/test_shader_precompile.py --platform both` -> `4/4 PASS`, `exit 0`):**
  - Scans all `236` disc `.xbox` archives + full NV2A FFP/programmable shader & PSO state matrix (including `bl_subtractive`, `ps_ui_mod[rect][quad][atest]`, `ps_ui_p13_mod_da`, untextured `shadow.psh` / `genericmesh.psh`, and `rect=1`/`kTexWide` post-process targets) in parallel at boot (`d3d12.cpp` / `vulkan.cpp`) and eagerly prewarms temporal upscalers (`DLSS 4` Preset `K` / `FSR 3.1` / `XeSS 2`) including a 1-frame dummy `Execute` + `ResetHistory` JIT warmup before frame 0.
  - **Win64 Cold (`1440p` RTX 3090, `DLSS 4` Preset `K`, empty `cache/`):** `391` shaders + `2,079` PSOs in `1,280.8 ms`, `dlss_boot_prewarm=True`, `mid_game_compiles=0`, **`runtime_psos=0`**, `mid_game_dlss_creates=0`, `sync_present_cache_writes=0` (`1,480` gameplay frames); **Win64 Warm:** `0.2 ms` cached skip, `0` compiles, **`0` runtime PSOs**.
  - **Linux Vulkan 1.2 Cold (`1280x800`, empty `cache/`):** `391` shaders + `4,579` PSOs in `19.5 s` (`llvmpipe`), `mid_game_compiles=0`, **`runtime_psos=0`**, `sync_present_cache_writes=0` (`271` gameplay frames); **Linux Warm:** `1.6 ms` cached skip, `0` compiles, **`0` runtime PSOs**.

---

## 11. Post-Release Cursor Lock & End-to-End Hitch Elimination Pass

### Track 1: Zero Visible/Black Cursor & Multi-Monitor Cursor Lock (`Win64` + `Linux`)
- **Persistent Transparent Cursor & Multi-Monitor Lock (`src/common/mkb_input.{h,cpp}`, `XboxXBERecomp/runtime/app/{main,main_linux}.cpp`):**
  - Uses a persistent 32x32 100%-transparent BGRA + 1bpp mask `HCURSOR` (`CreateIconIndirect` + `CreateCursor` fallback) + `GCLP_HCURSOR` override + `ShowCursor(FALSE)` + full `WM_SETCURSOR` interception across all hit-test codes (eliminates virtual-display `SetCursor(nullptr)` black-box fallback).
  - Separates cursor hide (`ComputeWantHideCursor` hides across all focused `GameplayActive`, `state==4` `Tip`, `Fmv`, and `LoadingTransition` even in `Controller` mode) from cursor lock (`ComputeWantCursorCapture` locks `ClipCursor` with `MonitorFromWindow` clamp + `2px` inset + relative-mouse center-warp in `Auto`/`Mkb` mode even if `ActiveDevice` began as `Controller`), re-asserted across mouse button/move/input/focus/display/size events and coalesced per-present ticks; Linux SDL3 enforces `SDL_SetWindowMouseRect`, `SDL_SetWindowMouseGrab`, `SDL_SetWindowRelativeMouseMode`, and `SDL_HideCursor`.
  - Strictly gates mouse/cursor messages away from ImGui (`ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange`) when F1 overlay is closed (prevents `::ReleaseCapture()` clearing `ClipCursor` on `WM_LBUTTONUP`), and stabilizes `QueryPsiOpsCamera().is_3d_gameplay` across `GameplayActive` + `state==4` `Tip` (`!pause_open`) in `src/common/game_info.cpp` + `src/game/hooks.cpp` so `DLSS 4` (`temporal=1`) never drops to `FSR 1.0` on in-game tutorial popups.
- **Verified (`tools/test_m3_mkb.py --platform both` -> `2/2 PASS`, `exit 0`):** `818/818` Win64 and `395/395` Linux continuous gameplay+Tip frames checked with `0` unlock leaks, `0` OS cursor visible leaks, and `15/15` cursor windows exact on both platforms.

### Track 2: Stutter, Hitch & Hot-Path Overhead Elimination (`Win64 D3D12` + `Linux Vulkan 1.2`)
- **Zero Mid-Gameplay Cache / Shader / PSO Stalls (`d3d12.cpp`, `vulkan.cpp`, `shader_precompile.{h,cpp}`, `perf.{h,cpp}`):**
  - Added lock-free `perf::CurrentPhase()`, deferred periodic `SaveCaches(false)` in `Present()` whenever `GameplayActive`, moved `std::filesystem::file_size` cache stat queries to the background save thread, expanded AOT PSO seeds (`kAotSchemaVersion=4`), and added 1-frame dummy `DLSS 4` `Execute` + `ResetHistory` boot warmup in `src/upscale/upscaler_manager.cpp` (eliminating first-frame `4.73 ms` DLSS JIT spike).
- **Zero-Overhead Closed-Overlay UI & Hot-Path Logging (`backend.h`, `d3d12.cpp`, `vulkan.cpp`, `settings_ui.{h,cpp}`, `log.cpp`, `d3d8.cpp`, `upscaler_manager.cpp`):**
  - Added lock-free `gpu::HasVisibleUi()` (`mkb::IsOverlayOpen() || show_hud`, defaulting `show_hud=false`) so `RenderUi()` skips `app::UiLock()`, `ImGui_ImplWin32_NewFrame()` (Win32/XInput syscalls on render thread), `ImGui_ImplVulkan_NewFrame()`, and descriptor heap switching whenever UI is hidden.
  - Removed synchronous `std::fflush` on `Level::Release` in `log.cpp` (kept on `Warn`/`Error` + explicit `logx::Flush()` checkpoints), rate-limited `MakeSpace`, demoted `CreateVertexShader` & unchanged `D3DDevice_Reset` to `LOGD`, and silenced transient `temporal=1 <-> 0` `LOGR` chatter in `upscaler_manager.cpp`.
- **Texture Binding, Swizzle Decode & XISO I/O (`d3d12.cpp`, `vulkan.cpp`, `texture.cpp`, `disc.cpp`):**
  - Eliminated spurious `DirtyEpoch` re-uploads for `<= 64 KiB` textures (where `MemSig` hashes 100% of bytes), hoisted `DecodeLevel` format dispatch outside swizzled pixel loops (fixing an 8-bit 1-byte overread in `Texel`), added contiguous bulk `memcpy` row fast-path in texture upload, gated `PerfClock::now()` behind `diag::Perf() || perf::Enabled()`, and upgraded `Disc::BlockCache` to `32 MiB` (`32 x 16 x 64 KiB`, `512 KiB` bypass) with `128 KiB` sequential 2-block read-ahead outside stripe locks.
- **Pre vs Post 89s Deterministic Gameplay Benchmark (`1440p` RTX 3090, `DLSS 4` Preset `K` Quality, `borderless`, `60 FPS`, `gameplay_active` `2,409` frames):**
  - `texup_ms` avg: `0.440 ms` -> **`0.001 ms`** (`-99.9%`)
  - `bind_ms` avg: `0.583 ms` -> **`0.151 ms`** (`-74.1%`)
  - `draw_ms` avg: `1.252 ms` -> **`0.858 ms`** (`-31.4%`)
  - `render_host` avg / p99: `1.640 / 2.487 ms` -> **`1.243 / 2.205 ms`** (`-24.2%` / `-11.3%`)
  - `render_thread_cpu_pct`: `8.01%` -> **`4.32%`** (`-46.1%`)
  - `present_ms` max (1st-frame DLSS): `4.726 ms` -> **`1.972 ms`** (`-58.3%`; frame-0 `0.873 ms`)
  - `disc_cache_hit_pct`: `35.94%` -> **`65.78%`** (`+29.84 pp`; `disc_os_reads` `595 -> 331`, `-44.4%`)
  - `runtime_compiles=0`, `runtime_psos=0`, `sync_present_cache_writes=0`, `audio_underruns=0`, `sim_ratio=0.9998`.
- **Unified Regression Suites & 5-Target Build (`exit 0`, pure-LF `patches/xboxxberecomp.patch` `1,132,116` B, `SHA-256 = f6838de11b38c05f3f56ef49899b79f00fc0febdc25a0d5ab1fcda26614088a2`):**
  - `tools/test_m3_mkb.py --platform both`: `2/2 PASS`
  - `tools/test_shader_precompile.py --platform both`: `4/4 PASS` (cold + warm Win64 & Linux, `runtime_compiles=0`, `runtime_psos=0`)
  - `tools/test_m4_perf.py --mode post --platform both`: `PASS` (`6/6` benchmarks including RTX 3090, AMD Radeon UMA iGPU, 60 vs 30 FPS fidelity, and Linux Vulkan 1.2; `disc_cache_hit_pct` `77.19%` Win64 / `77.36%` Linux; `40/40` pacing; `10/10` compile jobs)
  - `tools/test_m5_ux.py --skip-unpacked-compile`: `8/8 PASS` (Win64 + Linux)
  - `tools/test_disc_64bit.py`: `PASS`
  - All 5 targets built cleanly: `release` (`8,649,216` B), `debug` (`9,435,136` B), `headers-only-win` (`6,344,192` B), `linux-release` (`16,146,824` B, `GLIBC <= 2.35`), `headers-only-linux` (`16,146,824` B, `GLIBC <= 2.35`).
