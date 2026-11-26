# Psi-Ops: The Mindgate Conspiracy — Static Recompilation

[![Build & Verify](https://github.com/AnthonyP85/Psi-Ops-The-Mindgate-Conspiracy-ReComp/actions/workflows/build.yml/badge.svg)](https://github.com/AnthonyP85/Psi-Ops-The-Mindgate-Conspiracy-ReComp/actions/workflows/build.yml)

A native **Windows x64** (D3D12 + XAudio2 + SDL3) and **Native Linux x86_64 / Steam Deck** (Vulkan 1.2 + SDL3 + `GLIBC 2.35`) static recompilation of the Original Xbox (2004, Intel x86, Title ID `0x4D570016` / `MW-022`) edition of *Psi-Ops: The Mindgate Conspiracy*, built on [`XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (`cc9edda3a821a4204dc28783f429b88a27ade4f9`).

> **Legal Notice:** This repository and its binary releases contain **zero** copyrighted game assets, disc images, or Original Xbox executables (`.xbe`/`.iso`). You must supply your own legally owned retail USA Original Xbox disc image (`.xiso.iso` / `.iso`). On first launch, the launcher compiles your local `default.xbe` directly on your machine using the bundled freestanding compiler into `PsiOps.dll` (Windows) or `libPsiOps.so` (Linux) while mounting your `.xiso.iso` strictly read-only.

---

## Features

- **Native Dual-Platform Static Recompilation:** No emulator overhead — lifts all 9 executable sections (Midway ESP engine, Havok physics, Python 2.2.1, AI.implant, DirectSound 5849, and CRI Sofdec `PSGSFD*` video) to native x86-64 C++20.
- **Direct Read-Only XISO Mounting (`>2 GiB` 64-Bit Safe):** Streams directly from your untouched `.xiso.iso` via a sharded `16 MiB` `Disc::BlockCache` (`64 KiB` blocks) with full 64-bit offsets for all 21 `>2 GiB` CRI Sofdec `.sfd` FMVs.
- **6-Tab Pre-Game Launcher & Live `F1` / `BACK+L3` Overlay:** Configure disc image, upscaling, GPU/display, audio, controls, and diagnostics with mouse, touch, or gamepad (`L1`/`R1` tab switching, `Start` to play), including an in-app XISO file browser with Steam Deck microSD Quick-Jump and live disc validation.
- **9-Upscaler Graphics Suite & Native `16:9` / `16:10` Support:**
  - **6 Built-In Zero-DLL Spatial Upscalers (Windows D3D12 & Linux Vulkan 1.2):** `FSR 1.0` (default), `NVIDIA NIS v1.0.3`, `AMD CAS`, `Bicubic 5-Tap` (supports `>100%` SSAA), `Integer`, and `Bilinear`.
  - **3 Retail Vendor-Signed Temporal Upscalers (Windows D3D12, bundled in `plugins/`):** `NVIDIA DLSS 4` (Presets Auto/E/J/K & DLAA + automatic NGX driver-store fallback), `AMD FSR 3.1`, and `Intel XeSS 2` — driven by true `16:9` square-pixel rendering, Halton `(2,3)` camera jitter, `RG16F` motion vectors, and `R8_UNORM` HUD reactive masking (auto-bypassed on 2D menus, loading screens, and FMVs; gracefully falls back to `FSR 1.0` on Linux Vulkan or missing DLLs).
- **Native-PC 1:1 Mouse & Keyboard + Multi-Vendor Controller Support:**
  - `WM_INPUT` Raw Input (Windows) & `SDL_SetWindowRelativeMouseMode` (Linux) injected past the controller deadzone & slew filter with `1px` micro-aim precision, `0.000°` stop-drift, `dt`-independent sensitivity (`cm/360 @ 800 DPI`), decoupled PC Mouse Invert-Y, `Raw 1:1 Aim` auto-steer bypass, normalized `WASD` (`1.000`), engine-level decoupled `InputManager` actions (`E` Interact/Pickup, `Space` Jump/Mantle, `Q`/`Mouse4` Melee, `LCtrl`/`C` Crouch, `LAlt` Wall Cover, `R`/`MMB` instant manual Reload / MC Secondary, `WheelUp`/`WheelDown` bidirectional Next/Prev Weapon & Sniper Zoom / TK Push-Pull, `RMB`/`V` Sniper Scope / Target Lock-On, `LShift`/`Mouse5` + `1–6` Psi powers, mouse-flick TK Throw, `G` TK Camera Control, `X` MC Head Pop), smart OS cursor capture/release, and portable OS-independent `settings.toml` bindings (`Primary` + `Secondary` slots across 25 actions).
  - Full SDL3 controller support (DualSense 5 / Edge with Touchpad-as-Back, Xbox, Switch, Steam Deck) with sustained rumble keepalive and deterministic `.rec` recording/replay.
- **Cross-Hardware Performance & Frame Pacing:**
  - `NvOptimusEnablement` & `AmdPowerXpressRequestHighPerformance` exports, `--list-gpus` / `--gpu <idx>`, adaptive `16 / 32 / 64 MiB` upload rings, `32 MiB` GPU texture slab sub-allocators, per-VID/DID shader/PSO/metadata disk caches with parallel boot pre-warm & background async writes, `0` mid-frame GPU flushes, de-conflicted VRR/VSync pacing (`60/90/120/144/165 Hz`), RAM-bounded first-launch compilation (`~768 MiB`/slot), and binary-verified `60 FPS` (default) & `30 FPS` `1.00x` Havok simulation fidelity.

---

## Quick Start (Pre-Built Release Archives)

### Windows x64 (`Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.1.0-win64-launcher.zip`)
1. Extract the `.zip` archive to any writable folder (e.g. `C:\Games\PsiOps`).
2. Double-click **`PsiOps.exe`** to open the 6-Tab Pre-Game Launcher.
3. In **Play & Game Disc**, browse to your retail USA `Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso` (or place it inside an `assets/` folder beside `PsiOps.exe`).
4. Choose a one-click **Hardware Preset** (`1080p Mainstream`, `1440p High`, `4K Ultra`, `Handheld / iGPU`, or `Steam Deck`) and click **Play**.
   - On first launch, `PsiOps.exe` compiles `PsiOps.dll` locally from your XISO using the bundled `compiler/` toolchain and caches it for instant subsequent launches.

### Native Linux x86_64 & Steam Deck (`Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.1.0-linux-x86_64.tar.gz`)
> **IMPORTANT (Steam Deck / SteamOS):** `PsiOps` is a **native Linux x86_64 ELF binary** (`GLIBC 2.35`, Vulkan 1.2, SDL3). **Do NOT enable Proton / Steam Play compatibility** on the Steam shortcut!

1. Extract the tarball on your Steam Deck or Linux PC:
   ```bash
   mkdir -p ~/Games/PsiOps
   tar -xzf Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.1.0-linux-x86_64.tar.gz -C ~/Games/PsiOps
   cd ~/Games/PsiOps
   ```
2. *(Optional, Steam Deck / Desktop shortcut)* Register the native Linux Non-Steam Game shortcut and icon:
   ```bash
   ./install-steamdeck.sh --install-dir "$PWD"
   # To cleanly remove later:
   ./install-steamdeck.sh --uninstall
   ```
3. Launch **`./PsiOps`** (or from Steam Gaming Mode after restarting Steam). Use the built-in **XISO File Browser** (with one-click microSD `/run/media/...` Quick-Jump) to select your `.xiso.iso` and press **Start** / **Play**.

---

## Controls

| Action | Default M&K (Primary / Secondary) | Engine Action / Xbox (Config 1) |
|---|---|---|
| Move | `W` `A` `S` `D` / Arrow Keys | Left Stick (`0x01..0x04`, RV `0x09..0x0C`) |
| Aim / Look / TK Throw | Raw 1:1 Mouse (flick on TK release throws) | Right Stick (`0x0D` TK Throw) |
| Fire Weapon | `LMB` / `F` | Right Trigger (`RT`, `0x1A`/`0x1B`) |
| Sniper Scope / Lock-On | `RMB` / `V` | `R3` Sniper Scope (`0x1D`) / `Y` Lock-On (`0x2B`/`0x2C`/`0x33`) |
| Reload / MC Secondary | `R` / `MMB` | Instant Manual Reload / MC Secondary (`0x37`) |
| Jump / Mantle | `Space` | Decoupled Jump / Mantle (`0x23`, never melees/interacts) |
| Use / Interact / Pickup | `E` / `Enter` | Decoupled Interact (`0x1F`) & Ground Pickup (`0x1E`, never jumps) |
| Melee Attack | `Q` / `Mouse4` | Decoupled Melee (`0x20`, never jumps) |
| Crouch / Sneak | `LCtrl` / `C` | Crouch Toggle (`B`, `0x26`) |
| Wall Cover / Peek | `LAlt` | Wall Move / Cover (`L3`, `0x36`) |
| Next / Previous Weapon / Zoom | `WheelUp` / `WheelDown` | Next (`0x18`) / Prev (`0x19`), Sniper Zoom (`0x34`/`0x35`), TK Push/Pull (`0x05`/`0x06`) |
| Use Active Psi Power | `LShift` / `Mouse5` | Casts / holds selected Psi power (`0x10`/`0x11` + `0x12..0x17`) |
| Direct Psi Powers (`TK`/`PK`/`MD`/`MC`/`RV`/`AV`) | `1` `2` `3` `4` `5` `6` | Selects active Psi slot & casts/holds directly (`0x12..0x17`) |
| TK Camera Control / MC Head Pop | `G` / Hold `X` (0.5s) | TK Camera (`0x2E`) / MC Suicide (`0x38`) |
| Pause / Back / Skip FMV | `Escape` (context-aware latched) | `Start` (`0x80`) / `B` |
| Mission Objectives | `Tab` | `Back` (`0x21`/`0x22`, DualSense Touchpad) |
| Live Settings Overlay | `F1` | `Back + L3` |

All 25 actions support 4-column Click-to-Rebind (`Primary` + `Secondary`) in both the Pre-Game Launcher and `F1` Overlay, stored portably in `settings.toml` under `[controls]` and `[controls.bindings]`. M&K mappings are decoupled directly inside Psi-Ops's `InputManager` regardless of the in-game Xbox Control Config (`1`/`2`/`3`), while Controller mode and `.rec` replays remain 100% untouched.

---

## Command-Line Reference

| Flag | Description |
|---|---|
| `--iso <path>` | Mount the specified `.xiso.iso` / `.iso` read-only |
| `--launcher` / `--no-launcher` | Force open or bypass the Pre-Game Launcher (holding `Shift` at launch also opens the Launcher) |
| `--gpu <idx>` / `--list-gpus` | Select a specific GPU adapter by index or list available GPUs as JSON |
| `--fps-cap <30\|60>` | Set simulation-safe frame-rate cap (`60` default, `30` fallback; both preserve `1.00x` Havok timing) |
| `--present-mode <auto\|vsync\|vrr\|mailbox>` | Override VRR/VSync pacing mode |
| `--compile-jobs <N>` | Override parallel first-launch `PrepareGameCode` compiler threads |
| `--settings <path.toml>` | Use a custom `settings.toml` path |
| `--validate-iso <path>` | Deep read-only XISO/XBE validation (JSON output) |
| `--create-shortcut [dir]` | Create a desktop shortcut (`.lnk` on Windows, `.desktop` on Linux) |
| `--record-input <file.rec>` / `--replay-input <file.rec>` | Record or replay deterministic controller sessions (`--replay-mode hybrid\|frame\|time`, `--replay-exit`, `--state-log <file.jsonl>`) |
| `--perf-log <file.json>` | Write full 11-metric frame-time, GPU, audio, I/O, and memory telemetry JSON + CSV |

---

## Building from Source

### Prerequisites
- **Windows x64:** Visual Studio 2022/2026 C++ Build Tools (Windows 10/11 SDK), LLVM/Clang (`C:\Program Files\LLVM\bin`), CMake `>= 3.22`, Ninja, Python `>= 3.11`, Git.
- **Linux x86_64 (Ubuntu 22.04 recommended for `GLIBC 2.35` compatibility):** `build-essential`, `cmake`, `ninja-build`, `clang`, `lld`, `pkg-config`, `python3`, `patchelf`, `libvulkan-dev`, `mesa-vulkan-drivers`, `glslang-dev`, `spirv-tools`, and SDL3 dev headers (`libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev`).

### 1. Setup Pinned SDK (`XboxXBERecomp` + Patch)
```bash
# Windows
setup_sdk.bat

# Linux / WSL2
./setup_sdk.sh
```

### 2. Copyright-Clean `--headers-only` Build (No `.xbe` or `.iso` Required)
```bash
# Windows x64
cmake --preset release -B out/build/headers-only-win -DPSIOPS_ALLOW_NO_XBE=ON
cmake --build out/build/headers-only-win

# Linux x86_64
cmake --preset linux-release -B out/build/headers-only-linux -DPSIOPS_ALLOW_NO_XBE=ON
cmake --build out/build/headers-only-linux
```

### 3. Full Local Build (With Your Own `.xiso.iso`)
```bash
# Extract default.xbe privately to git-ignored .local/xbe/default.xbe (read-only)
python tools/extract_xbe.py --iso "/path/to/Psi-Ops - The Mindgate Conspiracy (USA).xiso.iso"

# Windows x64 (also optionally fetch signed retail upscaler DLLs into git-ignored plugins/)
python tools/fetch_upscalers.py
build.bat

# Linux x86_64
./build_linux.sh
```

### 4. Building Distributable Archives & Running the 6-Gate Audit
```bash
# Windows: creates dist/Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.1.0-{win64-launcher,source}.zip
release.bat

# Linux (Ubuntu 22.04): creates dist/Psi-Ops-The-Mindgate-Conspiracy-ReComp-v1.1.0-linux-x86_64.tar.gz
./release_linux.sh

# Run full 6-gate copyright, sanitization, binary ABI, and media integrity audit
python tools/audit_release.py
```

---

## Legal, Clean-Room & Trademark Disclaimer

- **Copyright-Clean Distribution:** This repository and all official release archives contain **zero** copyrighted game code, Original Xbox executables (`default.xbe`), disc images (`.iso`/`.xiso.iso`), audio, video, or artwork. You must supply your own legally obtained retail USA Original Xbox copy of *Psi-Ops: The Mindgate Conspiracy*. On first launch, the bundled freestanding compiler translates your local `default.xbe` on your own machine while mounting your disc image strictly read-only.
- **Independent Preservation Project:** This is an independent, non-commercial technical preservation project and is not affiliated with, endorsed by, or sponsored by Midway Games, Warner Bros. Interactive Entertainment, Microsoft, Valve, NVIDIA, AMD, or Intel.
- **Trademarks:** *Psi-Ops: The Mindgate Conspiracy*, *Xbox*, *Direct3D*, *Steam Deck*, *NVIDIA DLSS*, *AMD FidelityFX*, and *Intel XeSS* are trademarks or registered trademarks of their respective owners.

---

## License

Licensed under the **GNU General Public License v2.0 or later (`GPL-2.0-or-later`)** with the **Runtime Upscaler Plugin Exception** — see [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for complete legal notices.
