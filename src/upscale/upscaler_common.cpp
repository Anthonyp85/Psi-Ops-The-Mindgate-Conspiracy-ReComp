// SPDX-License-Identifier: GPL-2.0-or-later
// Shared upscaler configuration helpers, Halton (2,3) jitter, camera-cut detection,
// 4x4 matrix utilities, plugin probing, and Authenticode-verified NGX fallback.

#include "upscale/upscaler_common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>
#ifndef CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBEDDED
#define CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBEDDED (1u << 10)
#endif
#ifndef CERT_QUERY_FORMAT_FLAG_BINARY
#define CERT_QUERY_FORMAT_FLAG_BINARY (1u << 1)
#endif
#endif

namespace upscale {

namespace {

namespace fs = std::filesystem;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EnvTruthy(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

bool CheckPe64AndNoDevWatermark(const fs::path& path, std::string* out_reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (out_reason) *out_reason = "unreadable file";
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
    if (buf.size() < 0x100 || buf[0] != 'M' || buf[1] != 'Z') {
        if (out_reason) *out_reason = "not a PE image (missing MZ)";
        return false;
    }
    uint32_t pe_off = 0;
    std::memcpy(&pe_off, buf.data() + 0x3C, 4);
    if (pe_off + 0x1A > buf.size() || std::memcmp(buf.data() + pe_off, "PE\0\0", 4) != 0) {
        if (out_reason) *out_reason = "invalid PE signature";
        return false;
    }
    uint16_t machine = 0, magic = 0;
    std::memcpy(&machine, buf.data() + pe_off + 4, 2);
    std::memcpy(&magic, buf.data() + pe_off + 24, 2);
    if (machine != 0x8664u || magic != 0x020Bu) {
        if (out_reason) *out_reason = "not an x64 PE32+ binary";
        return false;
    }
    static constexpr char kForbidden[] = "DO NOT DISTRIBUTE";
    const size_t flen = sizeof(kForbidden) - 1;
    auto it = std::search(buf.begin(), buf.end(), kForbidden, kForbidden + flen);
    if (it != buf.end()) {
        if (out_reason) *out_reason = "watermarked /dev/ DLL rejected (DO NOT DISTRIBUTE)";
        return false;
    }
    return true;
}

#ifdef _WIN32
bool HasNvidiaAdapter() {
    auto adapters = app::EnumerateGpuAdapters();
    for (const auto& a : adapters) {
        if (!a.is_software && a.vendor_id == 0x10DEu) return true;
    }
    return false;
}

std::vector<fs::path> FindProgramDataDlssCandidates() {
    std::vector<fs::path> out;
    std::error_code ec;
    const wchar_t* pd_env = _wgetenv(L"ProgramData");
    fs::path pd = (pd_env && *pd_env) ? fs::path(pd_env) : fs::path(L"C:\\ProgramData");
    fs::path versions_root = pd / "NVIDIA" / "NGX" / "models" / "dlss" / "versions";
    if (!fs::is_directory(versions_root, ec)) return out;

    struct VerEntry {
        uint64_t ver_num = 0;
        std::string name;
        fs::path dir;
    };
    std::vector<VerEntry> dirs;
    for (auto it = fs::directory_iterator(versions_root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string n = app::PathToUtf8(it->path().filename());
        uint64_t v = std::strtoull(n.c_str(), nullptr, 10);
        dirs.push_back({v, std::move(n), it->path()});
    }
    std::sort(dirs.begin(), dirs.end(), [](const VerEntry& a, const VerEntry& b) {
        if (a.ver_num != b.ver_num) return a.ver_num > b.ver_num;
        return a.name > b.name;
    });

    for (const auto& vd : dirs) {
        fs::path files_dir = vd.dir / "files";
        if (!fs::is_directory(files_dir, ec)) continue;
        std::vector<std::pair<uint64_t, fs::path>> bins;
        for (auto fit = fs::directory_iterator(files_dir, fs::directory_options::skip_permission_denied, ec);
             !ec && fit != fs::directory_iterator(); fit.increment(ec)) {
            if (!fit->is_regular_file(ec)) continue;
            std::string ext = LowerAscii(app::PathToUtf8(fit->path().extension()));
            if (ext == ".bin" || ext == ".dll") {
                uint64_t sz = fit->file_size(ec);
                if (sz >= (1ull << 20)) bins.emplace_back(sz, fit->path());
            }
        }
        std::sort(bins.begin(), bins.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [sz, p] : bins) out.push_back(p);
    }
    return out;
}
#endif

}  // namespace

const char* ModeName(app::UpscalerMode mode) {
    switch (mode) {
        case app::UpscalerMode::Fsr1: return "FSR 1.0 (Spatial)";
        case app::UpscalerMode::Nis: return "NIS v1.0.3 (Spatial)";
        case app::UpscalerMode::Cas: return "CAS (Contrast Adaptive)";
        case app::UpscalerMode::Bicubic: return "Bicubic 5-Tap (incl. SSAA)";
        case app::UpscalerMode::Integer: return "Integer (Nearest Pixel)";
        case app::UpscalerMode::Bilinear: return "Bilinear";
        case app::UpscalerMode::Dlss: return "NVIDIA DLSS 4";
        case app::UpscalerMode::Fsr3: return "AMD FSR 3.1";
        case app::UpscalerMode::Xess: return "Intel XeSS 2";
        default: return "FSR 1.0 (Spatial)";
    }
}

const char* ModeShortName(app::UpscalerMode mode) {
    switch (mode) {
        case app::UpscalerMode::Fsr1: return "FSR 1.0";
        case app::UpscalerMode::Nis: return "NIS";
        case app::UpscalerMode::Cas: return "CAS";
        case app::UpscalerMode::Bicubic: return "Bicubic";
        case app::UpscalerMode::Integer: return "Integer";
        case app::UpscalerMode::Bilinear: return "Bilinear";
        case app::UpscalerMode::Dlss: return "DLSS 4";
        case app::UpscalerMode::Fsr3: return "FSR 3.1";
        case app::UpscalerMode::Xess: return "XeSS 2";
        default: return "FSR 1.0";
    }
}

const char* QualityName(app::UpscaleQuality quality) {
    switch (quality) {
        case app::UpscaleQuality::NativeAA: return "Native AA / DLAA (100%)";
        case app::UpscaleQuality::UltraQuality: return "Ultra Quality (77%)";
        case app::UpscaleQuality::Quality: return "Quality (67%)";
        case app::UpscaleQuality::Balanced: return "Balanced (58%)";
        case app::UpscaleQuality::Performance: return "Performance (50%)";
        case app::UpscaleQuality::UltraPerformance: return "Ultra Performance (33%)";
        case app::UpscaleQuality::Custom: return "Custom Scale (%)";
        default: return "Native AA (100%)";
    }
}

const char* DlssPresetName(app::DlssPreset preset) {
    switch (preset) {
        case app::DlssPreset::Auto: return "Auto (Driver Default)";
        case app::DlssPreset::PresetE: return "Preset E (CNN Legacy)";
        case app::DlssPreset::PresetJ: return "Preset J (Transformer)";
        case app::DlssPreset::PresetK: return "Preset K (Transformer 4)";
        default: return "Preset K (Transformer 4)";
    }
}

const char* HardwarePresetName(app::HardwarePreset preset) {
    switch (preset) {
        case app::HardwarePreset::Custom: return "Custom";
        case app::HardwarePreset::SteamDeck: return "Steam Deck (1280x800)";
        case app::HardwarePreset::HandheldIgpu: return "Handheld / iGPU";
        case app::HardwarePreset::Mainstream1080p: return "1080p Mainstream";
        case app::HardwarePreset::High1440p: return "1440p High";
        case app::HardwarePreset::Ultra4k: return "4K Ultra";
        default: return "Custom";
    }
}

bool IsTemporalMode(app::UpscalerMode mode) {
    return mode == app::UpscalerMode::Dlss || mode == app::UpscalerMode::Fsr3 || mode == app::UpscalerMode::Xess;
}

float QualityScaleFactor(app::UpscaleQuality quality, uint32_t custom_percent) {
    switch (quality) {
        case app::UpscaleQuality::NativeAA: return 1.0f;
        case app::UpscaleQuality::UltraQuality: return 0.77f;
        case app::UpscaleQuality::Quality: return 1.0f / 1.5f;
        case app::UpscaleQuality::Balanced: return 1.0f / 1.7f;
        case app::UpscaleQuality::Performance: return 0.5f;
        case app::UpscaleQuality::UltraPerformance: return 1.0f / 3.0f;
        case app::UpscaleQuality::Custom: return std::clamp(float(custom_percent) / 100.0f, 0.33f, 2.0f);
        default: return 1.0f;
    }
}

void ComputeRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, app::UpscalerMode mode,
                       app::UpscaleQuality quality, uint32_t custom_percent, bool aspect_correct,
                       uint32_t& out_render_w, uint32_t& out_render_h) {
    vp_w = std::max(320u, vp_w);
    vp_h = std::max(240u, vp_h);
    guest_w = std::max(320u, guest_w);
    guest_h = std::max(240u, guest_h);

    float factor = QualityScaleFactor(quality, custom_percent);
    if (IsTemporalMode(mode)) {
        // Temporal neural/motion upscalers require internal render resolution <= display output (DLAA max 100%)
        factor = std::clamp(factor, 1.0f / 3.0f, 1.0f);
    }

    uint32_t rw = 0;
    uint32_t rh = 0;
    if (aspect_correct) {
        rw = uint32_t(std::lround(double(vp_w) * double(factor)));
        rh = uint32_t(std::lround(double(vp_h) * double(factor)));
    } else {
        float sc = std::max((float(vp_h) * factor) / float(guest_h), 0.5f);
        rw = uint32_t(std::lround(double(guest_w) * double(sc)));
        rh = uint32_t(std::lround(double(guest_h) * double(sc)));
    }

    // Clamp cleanly to even dimensions within safe GPU limits
    uint32_t max_w = IsTemporalMode(mode) ? vp_w : 7680u;
    uint32_t max_h = IsTemporalMode(mode) ? vp_h : 4320u;
    rw = std::clamp(rw, 320u, std::max(320u, max_w)) & ~1u;
    rh = std::clamp(rh, 240u, std::max(240u, max_h)) & ~1u;

    out_render_w = std::max(320u, rw);
    out_render_h = std::max(240u, rh);
}

float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    uint32_t i = index;
    while (i > 0) {
        f /= float(base);
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

uint32_t JitterPhaseCount(uint32_t render_w, uint32_t vp_w) {
    float ratio = (render_w > 0 && vp_w > render_w) ? (float(vp_w) / float(render_w)) : 1.0f;
    uint32_t phases = uint32_t(std::lround(8.0f * ratio * ratio));
    return std::clamp(phases, 8u, 64u);
}

void ComputeJitter(uint32_t phase_index, uint32_t render_w, uint32_t vp_w, float& out_jx, float& out_jy) {
    uint32_t n = JitterPhaseCount(render_w, vp_w);
    uint32_t idx = (phase_index % n) + 1u;
    out_jx = Halton(idx, 2u) - 0.5f;
    out_jy = Halton(idx, 3u) - 0.5f;
}

bool IsCameraCut(const gpu::CameraFrameData& prev, const gpu::CameraFrameData& cur) {
    if (!prev.valid || !cur.valid) return true;
    if (!prev.is_3d_gameplay || !cur.is_3d_gameplay) return true;
    if (prev.session != cur.session || prev.level_path != cur.level_path) return true;

    float dx = cur.cam_pos[0] - prev.cam_pos[0];
    float dy = cur.cam_pos[1] - prev.cam_pos[1];
    float dz = cur.cam_pos[2] - prev.cam_pos[2];
    float dist_sq = dx * dx + dy * dy + dz * dz;
    // Psi-Ops verified camera-cut threshold: translation > 1.5 world units (2.25 squared)
    if (!std::isfinite(dist_sq) || dist_sq > 2.25f) return true;

    // Sharp camera orientation cut (> ~45 deg in a single frame: dot < 0.70)
    float dot = prev.cam_fwd[0] * cur.cam_fwd[0] + prev.cam_fwd[1] * cur.cam_fwd[1] +
                prev.cam_fwd[2] * cur.cam_fwd[2];
    float len_a = std::sqrt(prev.cam_fwd[0] * prev.cam_fwd[0] + prev.cam_fwd[1] * prev.cam_fwd[1] +
                            prev.cam_fwd[2] * prev.cam_fwd[2]);
    float len_b = std::sqrt(cur.cam_fwd[0] * cur.cam_fwd[0] + cur.cam_fwd[1] * cur.cam_fwd[1] +
                            cur.cam_fwd[2] * cur.cam_fwd[2]);
    if (len_a > 1e-4f && len_b > 1e-4f) {
        float cos_ang = dot / (len_a * len_b);
        if (cos_ang < 0.70f) return true;
    }

    // FOV jump (> ~5 deg = 0.087 rad, e.g., sniper scope zoom toggle)
    if (std::fabs(cur.fov_y_rad - prev.fov_y_rad) > 0.087f) return true;
    return false;
}

bool InvertMatrix4x4(const float m[16], float out[16]) {
    double inv[16];
    double d[16];
    for (int i = 0; i < 16; ++i) d[i] = double(m[i]);

    inv[0] = d[5] * d[10] * d[15] - d[5] * d[11] * d[14] - d[9] * d[6] * d[15] + d[9] * d[7] * d[14] +
             d[13] * d[6] * d[11] - d[13] * d[7] * d[10];
    inv[4] = -d[4] * d[10] * d[15] + d[4] * d[11] * d[14] + d[8] * d[6] * d[15] - d[8] * d[7] * d[14] -
             d[12] * d[6] * d[11] + d[12] * d[7] * d[10];
    inv[8] = d[4] * d[9] * d[15] - d[4] * d[11] * d[13] - d[8] * d[5] * d[15] + d[8] * d[7] * d[13] +
             d[12] * d[5] * d[11] - d[12] * d[7] * d[9];
    inv[12] = -d[4] * d[9] * d[14] + d[4] * d[10] * d[13] + d[8] * d[5] * d[14] - d[8] * d[6] * d[13] -
              d[12] * d[5] * d[10] + d[12] * d[6] * d[9];

    inv[1] = -d[1] * d[10] * d[15] + d[1] * d[11] * d[14] + d[9] * d[2] * d[15] - d[9] * d[3] * d[14] -
             d[13] * d[2] * d[11] + d[13] * d[3] * d[10];
    inv[5] = d[0] * d[10] * d[15] - d[0] * d[11] * d[14] - d[8] * d[2] * d[15] + d[8] * d[3] * d[14] +
             d[12] * d[2] * d[11] - d[12] * d[3] * d[10];
    inv[9] = -d[0] * d[9] * d[15] + d[0] * d[11] * d[13] + d[8] * d[1] * d[15] - d[8] * d[3] * d[13] -
             d[12] * d[1] * d[11] + d[12] * d[3] * d[9];
    inv[13] = d[0] * d[9] * d[14] - d[0] * d[10] * d[13] - d[8] * d[1] * d[14] + d[8] * d[2] * d[13] +
              d[12] * d[1] * d[10] - d[12] * d[2] * d[9];

    inv[2] = d[1] * d[6] * d[15] - d[1] * d[7] * d[14] - d[5] * d[2] * d[15] + d[5] * d[3] * d[14] +
             d[13] * d[2] * d[7] - d[13] * d[3] * d[6];
    inv[6] = -d[0] * d[6] * d[15] + d[0] * d[7] * d[14] + d[4] * d[2] * d[15] - d[4] * d[3] * d[14] -
             d[12] * d[2] * d[7] + d[12] * d[3] * d[6];
    inv[10] = d[0] * d[5] * d[15] - d[0] * d[7] * d[13] - d[4] * d[1] * d[15] + d[4] * d[3] * d[13] +
              d[12] * d[1] * d[7] - d[12] * d[3] * d[5];
    inv[14] = -d[0] * d[5] * d[14] + d[0] * d[6] * d[13] + d[4] * d[1] * d[14] - d[4] * d[2] * d[13] -
              d[12] * d[1] * d[6] + d[12] * d[2] * d[5];

    inv[3] = -d[1] * d[6] * d[11] + d[1] * d[7] * d[10] + d[5] * d[2] * d[11] - d[5] * d[3] * d[10] -
             d[9] * d[2] * d[7] + d[9] * d[3] * d[6];
    inv[7] = d[0] * d[6] * d[11] - d[0] * d[7] * d[10] - d[4] * d[2] * d[11] + d[4] * d[3] * d[10] +
             d[8] * d[2] * d[7] - d[8] * d[3] * d[6];
    inv[11] = -d[0] * d[5] * d[11] + d[0] * d[7] * d[9] + d[4] * d[1] * d[11] - d[4] * d[3] * d[9] -
              d[8] * d[1] * d[7] + d[8] * d[3] * d[5];
    inv[15] = d[0] * d[5] * d[10] - d[0] * d[6] * d[9] - d[4] * d[1] * d[10] + d[4] * d[2] * d[9] +
              d[8] * d[1] * d[6] - d[8] * d[2] * d[5];

    double det = d[0] * inv[0] + d[1] * inv[4] + d[2] * inv[8] + d[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    double inv_det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = float(inv[i] * inv_det);
    return true;
}

void MulMatrix4x4(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            tmp[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

bool VerifyPe64AndAuthenticode(const fs::path& path, const char* expected_signer_substr, std::string* out_reason) {
    if (!CheckPe64AndNoDevWatermark(path, out_reason)) return false;
#ifdef _WIN32
    std::wstring wpath = path.wstring();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = wpath.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;

    LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (status != ERROR_SUCCESS) {
        if (out_reason) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "WinVerifyTrust failed (0x%08lX)", (unsigned long)status);
            *out_reason = buf;
        }
        return false;
    }

    if (expected_signer_substr && *expected_signer_substr) {
        HCERTSTORE hstore = nullptr;
        HCRYPTMSG hmsg = nullptr;
        DWORD encoding = 0, content_type = 0, format_type = 0;
        BOOL qok = CryptQueryObject(CERT_QUERY_OBJECT_FILE, wpath.c_str(),
                                    CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBEDDED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                                    &encoding, &content_type, &format_type, &hstore, &hmsg, nullptr);
        bool signer_ok = false;
        if (qok && hstore && hmsg) {
            DWORD si_size = 0;
            if (CryptMsgGetParam(hmsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &si_size) && si_size > 0) {
                std::vector<uint8_t> si_buf(si_size);
                if (CryptMsgGetParam(hmsg, CMSG_SIGNER_INFO_PARAM, 0, si_buf.data(), &si_size)) {
                    auto* si = reinterpret_cast<PCMSG_SIGNER_INFO>(si_buf.data());
                    CERT_INFO ci{};
                    ci.Issuer = si->Issuer;
                    ci.SerialNumber = si->SerialNumber;
                    PCCERT_CONTEXT cert = CertFindCertificateInStore(
                        hstore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
                    if (cert) {
                        char name_buf[256] = {};
                        CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name_buf, sizeof(name_buf));
                        std::string lname = LowerAscii(name_buf);
                        std::string lwant = LowerAscii(expected_signer_substr);
                        if (lname.find(lwant) != std::string::npos) {
                            signer_ok = true;
                        } else if (out_reason) {
                            *out_reason = std::string("unexpected Authenticode signer: ") + name_buf;
                        }
                        CertFreeCertificateContext(cert);
                    }
                }
            }
        }
        if (hmsg) CryptMsgClose(hmsg);
        if (hstore) CertCloseStore(hstore, 0);
        if (!signer_ok) {
            if (out_reason && out_reason->empty()) *out_reason = "Authenticode signer could not be matched";
            return false;
        }
    }
    return true;
#else
    (void)expected_signer_substr;
    return true;
#endif
}

fs::path LocateNgxCoreDll() {
#ifdef _WIN32
    std::error_code ec;
    // 1. Official NVIDIA DriverStore registry key written by the display driver installer
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hkey) ==
        ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hkey, L"FullPath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) ==
                ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            fs::path base(buf);
            fs::path cand = fs::is_directory(base, ec) ? (base / "_nvngx.dll") : base;
            if (fs::is_regular_file(cand, ec)) {
                RegCloseKey(hkey);
                return cand;
            }
        }
        RegCloseKey(hkey);
    }
    // 2. System32 fallback
    wchar_t sysdir[MAX_PATH] = {};
    if (GetSystemDirectoryW(sysdir, MAX_PATH) > 0) {
        fs::path cand = fs::path(sysdir) / "_nvngx.dll";
        if (fs::is_regular_file(cand, ec)) return cand;
    }
    return {};
#else
    return {};
#endif
}

fs::path ResolveNgxDlssPath(bool* out_used_programdata, std::string* out_detail) {
    if (out_used_programdata) *out_used_programdata = false;
#ifdef _WIN32
    std::error_code ec;
    fs::path plugin_dlss = app::ExeDir() / "plugins" / "nvngx_dlss.dll";
    if (!EnvTruthy("XBE_DISABLE_PLUGINS_DLSS") && fs::is_regular_file(plugin_dlss, ec)) {
        std::string reason;
        if (CheckPe64AndNoDevWatermark(plugin_dlss, &reason)) {
            if (out_detail) *out_detail = "plugins/nvngx_dlss.dll";
            return plugin_dlss;
        }
        LOGW("upscale: rejected %s: %s", app::PathToUtf8(plugin_dlss).c_str(), reason.c_str());
    }

    // Fallback: verify NVIDIA-signed retail DLSS model in %ProgramData%\NVIDIA\NGX\models\dlss\
    // and stage atomically into cache\ngx\nvngx_dlss.dll.
    fs::path cache_dir = app::ExeDir() / "cache" / "ngx";
    fs::path cached_dll = cache_dir / "nvngx_dlss.dll";
    auto cands = FindProgramDataDlssCandidates();
    for (const fs::path& cand : cands) {
        std::string reason;
        if (!VerifyPe64AndAuthenticode(cand, "NVIDIA", &reason)) {
            LOGW("upscale: skipping ProgramData NGX candidate %s (%s)", app::PathToUtf8(cand).c_str(), reason.c_str());
            continue;
        }
        uint64_t src_sz = fs::file_size(cand, ec);
        bool already_staged = false;
        if (fs::is_regular_file(cached_dll, ec) && fs::file_size(cached_dll, ec) == src_sz) {
            std::string creason;
            if (VerifyPe64AndAuthenticode(cached_dll, "NVIDIA", &creason)) already_staged = true;
        }
        if (!already_staged) {
            fs::create_directories(cache_dir, ec);
            fs::path tmp = cached_dll;
            tmp += ".tmp";
            fs::copy_file(cand, tmp, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                MoveFileExW(tmp.c_str(), cached_dll.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            }
        }
        if (fs::is_regular_file(cached_dll, ec)) {
            if (out_used_programdata) *out_used_programdata = true;
            if (out_detail) *out_detail = "ProgramData NGX fallback -> cache/ngx/nvngx_dlss.dll";
            LOGR("upscale: NGX ProgramData fallback staged verified retail nvngx_dlss.dll (%s -> %s)",
                 app::PathToUtf8(cand).c_str(), app::PathToUtf8(cached_dll).c_str());
            return cached_dll;
        }
    }
    if (out_detail) *out_detail = "missing plugins/nvngx_dlss.dll and no signed ProgramData NGX model";
    return {};
#else
    if (out_detail) *out_detail = "Win64 D3D12 only (Linux Vulkan falls back to FSR 1.0)";
    return {};
#endif
}

PluginProbe ProbePlugins(bool stage_programdata_fallback) {
    PluginProbe p{};
#ifdef _WIN32
    std::error_code ec;
    fs::path pdir = app::ExeDir() / "plugins";

    // DLSS 4
    p.ngx_core_path = LocateNgxCoreDll();
    p.dlss_driver_present = !p.ngx_core_path.empty() && HasNvidiaAdapter();
    fs::path pl_dlss = pdir / "nvngx_dlss.dll";
    if (!EnvTruthy("XBE_DISABLE_PLUGINS_DLSS") && fs::is_regular_file(pl_dlss, ec) &&
        CheckPe64AndNoDevWatermark(pl_dlss, nullptr)) {
        p.dlss_plugin_present = true;
        p.dlss_path = pl_dlss;
    } else if (stage_programdata_fallback) {
        bool used_pd = false;
        std::string det;
        p.dlss_path = ResolveNgxDlssPath(&used_pd, &det);
        p.dlss_programdata_available = used_pd && !p.dlss_path.empty();
    } else {
        auto cands = FindProgramDataDlssCandidates();
        for (const fs::path& cand : cands) {
            if (VerifyPe64AndAuthenticode(cand, "NVIDIA", nullptr)) {
                p.dlss_programdata_available = true;
                p.dlss_path = cand;
                break;
            }
        }
    }
    p.dlss_ready = p.dlss_driver_present && (p.dlss_plugin_present || p.dlss_programdata_available);
    if (p.dlss_ready) {
        p.dlss_detail = p.dlss_plugin_present ? "[Ready] Retail plugins/nvngx_dlss.dll + NVIDIA NGX Driver"
                                              : "[Ready] Signed ProgramData NGX Model + NVIDIA Driver";
    } else if (!p.dlss_driver_present) {
        p.dlss_detail = "Requires NVIDIA RTX GPU & Driver";
    } else {
        p.dlss_detail = "Missing plugins/nvngx_dlss.dll";
    }

    // AMD FSR 3.1
    fs::path pl_ffx_loader = pdir / "amd_fidelityfx_loader_dx12.dll";
    fs::path pl_ffx_up = pdir / "amd_fidelityfx_upscaler_dx12.dll";
    if (fs::is_regular_file(pl_ffx_loader, ec) && fs::is_regular_file(pl_ffx_up, ec) &&
        CheckPe64AndNoDevWatermark(pl_ffx_loader, nullptr) && CheckPe64AndNoDevWatermark(pl_ffx_up, nullptr)) {
        p.fsr3_loader_path = pl_ffx_loader;
        p.fsr3_upscaler_path = pl_ffx_up;
        p.fsr3_ready = true;
        p.fsr3_detail = "[Ready] FidelityFX API DX12 (all DX12 GPUs)";
    } else {
        p.fsr3_detail = "Missing plugins/amd_fidelityfx_{loader,upscaler}_dx12.dll";
    }

    // Intel XeSS 2
    fs::path pl_xess = pdir / "libxess.dll";
    if (fs::is_regular_file(pl_xess, ec) && CheckPe64AndNoDevWatermark(pl_xess, nullptr)) {
        p.xess_path = pl_xess;
        p.xess_ready = true;
        p.xess_detail = "[Ready] plugins/libxess.dll (all SM6.4+ DX12 GPUs)";
    } else {
        p.xess_detail = "Missing plugins/libxess.dll";
    }
#else
    (void)stage_programdata_fallback;
    p.dlss_detail = "Win64 D3D12 only (auto-fallback: FSR 1.0)";
    p.fsr3_detail = "Win64 D3D12 only (auto-fallback: FSR 1.0)";
    p.xess_detail = "Win64 D3D12 only (auto-fallback: FSR 1.0)";
#endif
    return p;
}

void ApplyHardwarePreset(app::Settings& s, app::HardwarePreset preset, const PluginProbe& probe) {
    s.hardware_preset = preset;
    switch (preset) {
        case app::HardwarePreset::SteamDeck:
            s.width = 1280;
            s.height = 800;
            s.display_mode = app::DisplayMode::Borderless;
            s.upscaler = app::UpscalerMode::Fsr1;
            s.upscale_quality = app::UpscaleQuality::Quality;
            s.upscale_sharpness = 35;
            s.aniso = 4;
            s.present_mode = app::PresentMode::Auto;
            s.vsync = true;
            s.fps_cap = 60;
            s.aspect_correct_render = true;
            break;
        case app::HardwarePreset::HandheldIgpu:
            s.width = 1280;
            s.height = 720;
            s.upscaler = app::UpscalerMode::Fsr1;
            s.upscale_quality = app::UpscaleQuality::Balanced;
            s.upscale_sharpness = 35;
            s.aniso = 4;
            s.present_mode = app::PresentMode::Auto;
            s.vsync = true;
            s.fps_cap = 60;
            s.aspect_correct_render = true;
            break;
        case app::HardwarePreset::Mainstream1080p:
            s.upscaler = app::UpscalerMode::Cas;
            s.upscale_quality = app::UpscaleQuality::NativeAA;
            s.upscale_sharpness = 25;
            s.aniso = 8;
            s.present_mode = app::PresentMode::Auto;
            s.vsync = true;
            s.fps_cap = 60;
            s.aspect_correct_render = true;
            break;
        case app::HardwarePreset::High1440p:
            if (probe.dlss_ready) {
                s.upscaler = app::UpscalerMode::Dlss;
                s.upscale_quality = app::UpscaleQuality::Quality;
                s.dlss_preset = app::DlssPreset::PresetK;
            } else if (probe.fsr3_ready) {
                s.upscaler = app::UpscalerMode::Fsr3;
                s.upscale_quality = app::UpscaleQuality::Quality;
            } else {
                s.upscaler = app::UpscalerMode::Cas;
                s.upscale_quality = app::UpscaleQuality::NativeAA;
            }
            s.upscale_sharpness = 20;
            s.aniso = 16;
            s.present_mode = app::PresentMode::Auto;
            s.vsync = true;
            s.fps_cap = 60;
            s.aspect_correct_render = true;
            break;
        case app::HardwarePreset::Ultra4k:
            if (probe.dlss_ready) {
                s.upscaler = app::UpscalerMode::Dlss;
                s.upscale_quality = app::UpscaleQuality::Quality;
                s.dlss_preset = app::DlssPreset::PresetK;
            } else if (probe.fsr3_ready) {
                s.upscaler = app::UpscalerMode::Fsr3;
                s.upscale_quality = app::UpscaleQuality::Quality;
            } else if (probe.xess_ready) {
                s.upscaler = app::UpscalerMode::Xess;
                s.upscale_quality = app::UpscaleQuality::Quality;
            } else {
                s.upscaler = app::UpscalerMode::Fsr1;
                s.upscale_quality = app::UpscaleQuality::Quality;
            }
            s.upscale_sharpness = 15;
            s.aniso = 16;
            s.present_mode = app::PresentMode::Auto;
            s.vsync = true;
            s.fps_cap = 60;
            s.aspect_correct_render = true;
            break;
        case app::HardwarePreset::Custom:
        default:
            break;
    }
}

}  // namespace upscale
