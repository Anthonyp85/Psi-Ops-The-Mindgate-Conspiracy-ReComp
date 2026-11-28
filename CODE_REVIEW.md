# CODE_REVIEW.md — External read-only review (Claude Code)

- **Reviewed at:** `v1.2.2` (`ff3b1d2`), 2026-11-27. First pass was at `v1.2.1` (`882a082`); every "verified" item below was re-read at `v1.2.2`.
- **Reviewer:** Claude Code, working in a separate git worktree on branch `claude/init-b2b832`.
- **Fixes:** `H1`, `H2`, `H4`, `H5` are fixed on branch `claude/fix-review-high` (see §3a). Until that branch is merged, `main` still has them.
- **For:** whoever picks the project up next (Gemini / Antigravity on `main`, or a human).

## 1. What this session changed

| File | Change |
|---|---|
| `CLAUDE.md` | New. Claude Code's counterpart to `AGENTS.md` (commands, architecture, repo rules). |
| `CODE_REVIEW.md` | New. This file. |
| `handoff.md` | One appended section pointing here. |

That documentation commit (`3fd50d1`) touched nothing else. The follow-up fix branch `claude/fix-review-high` changes eight code files (listed in §3a) plus this file and `handoff.md`. **Still untouched by any Claude work: `config/`, `patches/`, the SDK submodule, build scripts, and version strings.** Nothing was tagged or pushed.

## 2. How to read the findings

- **Verified** means "I read the code at `v1.2.2` and it does what the finding says". Nothing was built or executed, so no finding is reproduced at runtime.
- **Reported** means a reviewer sub-agent raised it and I did not confirm it myself. Treat as leads, not facts.
- SDK findings cite `XboxXBERecomp/…` paths in the patched tree. That tree was confirmed byte-identical to `patches/xboxxberecomp.patch` at `v1.2.2`. Fixing any of them means editing the SDK and re-exporting the patch (see `AGENTS.md` §4).
- Line numbers are for `ff3b1d2` and will drift.

## 3. Fixed by `v1.2.2`

| Earlier finding | Status |
|---|---|
| File browser crash on non-ACP filenames; disc path stored in the wrong encoding (`file_browser_ui.cpp`, `launcher_ui.cpp`) | Fixed: `app::PathToUtf8` / `app::Utf8ToPath`, `error_code` iteration, `try/catch`. |
| Temporal motion vectors built from transposed matrices (`upscaler_spatial.cpp` `kTemporalPrepHlsl`) | Fixed: `row_major float4x4 CurVpInv/PrevVp` at `:351-352`. |

Two `.string()` calls on host paths remain and break the new UTF-8 rule: `src/common/ui_common.cpp:792` (`app::ExeDir().string()`) and `:798` (`out_lnk.string()`). On Windows these can still throw for a non-ACP install or Desktop path.

## 3a. Fixed on branch `claude/fix-review-high` (based on `3fd50d1`, not yet on `main`)

| ID | What was wrong | Change | Files |
|---|---|---|---|
| H1 | F1 overlay Controls tab re-locked its own non-recursive mutex: `DrawControlsSettingsPanel` held `st.lock` while calling `app::Save()`, which reaches `mkb::SaveSettings` and locks it again. | The panel now takes a `std::unique_lock` and releases it before `app::Save()`. The F1 overlay passes `auto_save = false` and lets its own `if (dirty) app::Save()` do the single save, so a change no longer writes `settings.toml` twice in one frame (PR review follow-up). | `src/common/mkb_input.cpp`, `src/common/settings_ui.cpp` |
| H2 | Self-update skipped the hash check when the release asset had no digest, then reported "staged and verified". | `StartDownloadAndStageUpdate()` refuses up front when the digest is empty (error state, message points to the release page), and the worker's check is now unconditional. | `src/common/update_checker.cpp` |
| H4 | Win64 CI passed when `release.bat` failed, because cmd ignores a failed `call` and the audit fell back to an unpacked check. | `call release.bat --skip-build || exit /b 1`; new opt-in `--require-archives` audit flag (used by CI) makes a missing zip fail Gate 4. Without the flag the audit behaves as before. | `.github/workflows/build.yml`, `tools/audit_release.py` |
| H5 | Zero-gated perf counters were read with `.get(key, 0)`, so a missing key passed; `--rec` passed `--replay`, which the runtime ignores. | Missing zero-gated counters are recorded and fail strict (`post`) runs; flag renamed to `--replay-input`. | `tools/test_m4_perf.py`, `tools/test_shader_precompile.py`, `tools/test_real_upscalers.py` |

How these were checked (no game launch, no full build):

- `mkb_input.cpp`, `settings_ui.cpp` and `update_checker.cpp` compile cleanly with `-fsyntax-only` using the exact Win64 flags from the existing headers-only build tree. **Not compiled for Linux, not linked, not run.**
- The new perf parser was run over 96 existing `perf.json` files: all 70 from current-era builds have every required key; 26 from pre-M4 builds lack `mid_frame_flushes` / `sync_present_cache_writes`, which only matters in strict `post` mode (pre-baselines are not strict). The `--dry-run` self-test passes and now covers the missing-key case.
- CI was confirmed green and producing the zip at `ff3b1d2` before the `|| exit /b 1` change, so the stricter step should not turn a healthy run red. **The changed workflow itself has not run yet.**

Worth doing before merging: rebind a key and move a slider in the F1 overlay Controls tab on both platforms (H1), run `tools/test_m4_perf.py --mode post` and `tools/test_shader_precompile.py` once (H5), and let CI run on the branch (H4).

Behaviour changes to be aware of: a release whose asset has no `digest` in the GitHub API response can no longer be self-installed (H2), and `test_real_upscalers.py --rec` now really replays the recording and exits when it ends (H5).

## 4. Still open — verified at `v1.2.2`

### High

| ID | Finding | Where | Suggested fix |
|---|---|---|---|
| H3 | **D3D12 window resize skips render-target reallocation and upscaler reset.** `ResizeSwap()` ends by calling `ComputeRenderExtents(render_w_, render_h_, …)`, so the following `ApplySettings()` computes `size_changed == false` and skips `targets_.clear()`, the scale update and the upscaler history reset. | `XboxXBERecomp/runtime/gpu/d3d12.cpp:5639`, `:6122-6135` | Have `ResizeSwap` leave the old extents (or set a `remake` flag) so `ApplySettings` sees the change. Vulkan equivalent was reported, not verified. |

`H1`, `H2`, `H4` and `H5` moved to §3a.

Note on H1 (now fixed): the save call had been there since `v1.0.0`. Re-locking a `std::mutex` on the same thread is undefined behaviour (normally a hang or abort). If rebinding a key in the F1 overlay works in practice, something in that chain is not what it looks like on paper — worth one manual test before changing anything.

### Medium

| ID | Finding | Where |
|---|---|---|
| M1 | RCAS limiter is effectively disabled on D3D12: `rcp(max(4.0 * mn4 - 4.0, -1e-5))` should be `min` (the Vulkan copy at `upscaler_vulkan.cpp:260` is correct). | `src/upscale/upscaler_spatial.cpp:306` |
| M2 | M&K hooks stay live while **recording**: `ShouldBypassMkb()` covers replay and autotest only, so a `.rec` captured while playing on mouse and keyboard will not replay identically. | `src/common/input_recorder.cpp:523` |
| M3 | Cheat "applied" flags are cleared whenever the tracked player pointer changes (including to 0) without undoing what the cheat wrote into guest state. | `src/game/hooks.cpp:567-571` |
| M4 | Updater test hooks are active in release builds: `PSIOPS_UPDATE_MOCK_JSON` / `PSIOPS_UPDATE_MOCK_ARCHIVE` let an environment variable supply the release metadata and the archive. | `src/common/update_checker.cpp:2011`, `:2069`, `:2234` |
| M5 | First-launch compile leaves orphaned compiler jobs on Linux if the parent dies: `setpgid` only, no `prctl(PR_SET_PDEATHSIG)` or signal forwarding. | `XboxXBERecomp/runtime/app/game_code.cpp:1267` |
| M6 | Headers-only codegen does not depend on `src/`: the custom command depends on the tool and the config only, but `xbe_recomp --headers-only` scans `src/` for `X86_FUNC` / `X86_EXTERN_FUNC`. Adding an override does not trigger a regenerate. | `XboxXBERecomp/cmake/XbeRecomp.cmake:190`, `:200` |
| M7 | `FastMemHash64` has no rotation between rounds, so paired top-bit changes within a lane cancel (possible stale-surface cache hits). | `XboxXBERecomp/runtime/gpu/d3d12.cpp:360-378` |
| M8 | Audit silently skips its PE checks when `pefile` cannot be imported. | `tools/audit_release.py:464-467` |
| M9 | Kernel `SetEvent` direct-grant path applies to any non-notification object, not just synchronization events. | `XboxXBERecomp/runtime/kernel/objects.cpp:335` |
| M10 | `HasVisibleUi()` has side effects (polls the F2 hotkey, consumes pending save requests) and D3D12 calls it twice per frame. `AGENTS.md` describes it as a lock-free bypass. | `src/common/settings_ui.cpp:560-572`; `XboxXBERecomp/runtime/gpu/d3d12.cpp:5717`, `:5831` |

### Low

| ID | Finding | Where |
|---|---|---|
| L1 | Embedded Python fallback has a syntax error: `'X-GitHub-Api-Version: 2022-11-28',` inside a dict literal (missing `': '` split), so that fallback can never run. | `src/common/update_checker.cpp:1471` |
| L2 | FD cleanup cap is inverted: a large `sysconf(_SC_OPEN_MAX)` is replaced with 1024, so descriptors above 1024 leak into the child. | `src/common/update_checker.cpp:1283-1284` |
| L3 | `dlerror()` is called twice; the second call returns null. | `XboxXBERecomp/runtime/cpu/dispatch.cpp:84` |
| L4 | Vulkan allocates from the newest extra descriptor pool forever after one overflow. | `XboxXBERecomp/runtime/gpu/vulkan.cpp:5532` |
| L5 | Vulkan timestamp state (`ts_started_`) is left set on the acquire-failure early return. | `XboxXBERecomp/runtime/gpu/vulkan.cpp:7281-7292` |
| L6 | Vulkan submits mid-draw when an upload allocation overflows. | `XboxXBERecomp/runtime/gpu/vulkan.cpp:3769-3771` |
| L7 | D3D12 `upload_reserve_` is assigned in several places and never read. | `XboxXBERecomp/runtime/gpu/d3d12.cpp:909`, `:1402-1549` |
| L8 | Linux shutdown destroys the SDL window before `kernel::Shutdown(0)` stops guest/render threads. | `XboxXBERecomp/runtime/app/main_linux.cpp:1105-1113` |
| L9 | Two host-path `.string()` calls left (see §3). | `src/common/ui_common.cpp:792`, `:798` |

## 5. Reported by reviewers — not verified

The `v1.2.2` diff did not touch the cited code for any of these (only path-encoding lines changed in the same files), so they are neither confirmed nor resolved.

- **Caches:** shader/PSO metadata caches are not keyed on the shader generator, so a self-update can keep using old shaders (both backends, `shader_precompile.cpp`).
- **Replay:** one replay cursor is shared across ports; replay applies one frame late; the `.rec` is only written on clean shutdown, so a crash loses the recording (`input_recorder.cpp`).
- **Updater:** the Windows apply script breaks on non-ASCII or `%` in the install path; no rollback after a partial copy; Linux redirects are not host-pinned; `wget` exit 8 is mapped to 404; hard 120 s download limit.
- **Upscalers:** jitter stays enabled when a temporal backend falls back to FSR 1.0; FSR3/XeSS init is retried every frame after a failure; descriptor ring of 4 sets; `plugins/` DLLs are not Authenticode-verified at load and the signer check is a substring match; PE bounds arithmetic can wrap in `upscaler_common.cpp`; `xess_abi.h` may be missing a `reserved0` field.
- **Kernel / XDK:** completion packets posted for I/O that failed synchronously (`files.cpp`); unprivileged Linux threads left at a raised nice value (`threads.cpp`); vblank event reset after 250 µs (`d3d8.cpp`); fork-then-allocate in Linux `Relaunch`; DSound notifications can fire twice.
- **Vulkan / D3D12:** custom border colour and precise occlusion features used without querying support; R32F compare sampling; swapchain barrier source stage; `CopyByRegion` scale mismatch; slab heaps never released.
- **Input / UI:** NaN sensitivity survives `std::clamp`; click-to-focus fires the weapon; launcher init failure is treated as "Exit"; file-browser quick-jump list rebuilt every frame; settings saved on every frame of a slider drag; pending guest-settings sync can be overwritten; `g_applying_host_guest_settings` is not RAII-guarded.
- **Tests / tooling:** stale artifacts counted as fresh; tests kill `PsiOps` by image name; settings round-trip check in `test_m3_mkb.py` is vacuous; `test_disc_64bit.py` does not exercise the runtime; `fetch_upscalers.py` moves DLLs into `plugins/` before verifying them; patch-parity check is skipped when the SDK is not checked out; GLIBC check in CI passes on empty output; release scripts do not check for a dirty tree; archives are not byte-reproducible; `psiops.res` is not a link dependency.

## 6. Areas the reviewers reported clean

Guest pointer bounds and `GuestCallFrame` usage in the hooks; the hand-written SHA-256 and JSON parser; Linux shell quoting in the updater; the disc cache including 64-bit offsets; kernel memory commit tracking; IOCP lock ordering; the NGX and FidelityFX ABI headers; the CMake path-remapping block; `extract_xbe.py`; version strings (consistent at `1.2.2`).

## 7. Notes on code new in `v1.2.2`

Skimmed, not reviewed in depth:

- `Hook_AdxLock` / `Hook_AdxUnlock` / `Hook_AdxMwidleBorderWait` validate the guest addresses they write. Their comments do not state the guest calling convention or stack cleanup, which the other hooks do.
- The ADX mutex hooks replace the guest functions on every platform, not only Linux. If that is intended, a line in `PROJECT_MEMORY.md` saying Windows replays were re-verified with them would close the question.

## 8. Environment observations (main checkout)

- `core.autocrlf` is `true` at system scope and not overridden for the repo or the SDK submodule. Git prints "LF will be replaced by CRLF" warnings for patched SDK files. The exported patch is currently clean (zero CR, matches the live diff), but this is fragile; `git -C XboxXBERecomp config core.autocrlf false` would make it robust. Not changed by this session.
- `AGENTS.md` is untracked in the main checkout, so it does not exist in other worktrees or clones. `CLAUDE.md` refers to it as optional for that reason.
- `tools/audit_release.py` Gate 1 scans **all refs**, so commits on `claude/*` branches are audited too. Commits from this worktree use the same noreply owner identity, and `CLAUDE.md`, this file and the commit trailer were checked against the audit's text scanner (zero hits).
