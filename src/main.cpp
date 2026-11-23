// OLEDTextFix (formerly QDTextFix) — colour fringing reducer for OLED subpixel layouts:
// tuned for QD-OLED triangle (triad) panels, with presets for LG WOLED (RWBG / RGWB)
//
// How it works:
//   1) The selected monitor's desktop image is captured with DXGI Desktop Duplication.
//   2) A pixel shader resamples each colour channel at the physical position of that
//      channel's subpixel on the panel (e.g. G on top, R/B at the bottom), which reduces
//      the green/magenta fringes along glyph edges.
//   3) The result is shown in a click-through, capture-excluded, topmost
//      DirectComposition window.
//
// SDR only. Pauses automatically while HDR is on and for fullscreen applications.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

// ---------------------------------------------------------------- helpers

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) { p->Release(); p = nullptr; }
    }
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    template <class U>
    HRESULT as(Com<U>& out) const { return p->QueryInterface(__uuidof(U), (void**)out.put()); }
};

static wchar_t g_exeDir[MAX_PATH];
static wchar_t g_exePath[MAX_PATH];
static wchar_t g_iniPath[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];
static SRWLOCK g_logLock = SRWLOCK_INIT;

static void Log(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_logLock);
    FILE* f = _wfopen(g_logPath, L"a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

#include "core.h"

// ---------------------------------------------------------------- config

// Filters (all bicubic). Values are stored in the INI file: 0 and 1 were Bilinear and Mitchell in
// earlier builds and are converted on load (see NormalizeKernel). The three that remain are the best trade-offs
// between fringing and sharpness found in tools/sim_filters.py; no other 4-6 tap filter tested did better.
static const int kKernelList[] = {K_CATMULL, K_BALANCED, K_SMOOTH};  // order in the drop-down list
static const int kKernelListN = 3;
static const wchar_t* KernelName(int k) {
    return k == K_BALANCED ? L"Balanced (less fringing, still sharp)"
         : k == K_SMOOTH   ? L"Smooth (least fringing, no added blur)"
                           : L"Sharp — Catmull-Rom (recommended)";
}
static int NormalizeKernel(int k) {
    if (k == K_OLD_MITCHELL) return K_SMOOTH;  // closest remaining filter, sharper than Mitchell was
    if (k < K_CATMULL || k >= K_COUNT) return K_CATMULL;  // old Bilinear or unknown
    return k;
}

#ifndef OTF_TEST_DEFAULT
#define OTF_TEST_DEFAULT 0
#endif
#ifndef OTF_DIAG_DEFAULT
#define OTF_DIAG_DEFAULT 0  // the diagnostic build (build.sh diag) turns the timing log on by default
#endif

struct Config {
    int monitor = -1;  // -1 = automatic (the primary display)
    // Physical position of each channel's subpixel relative to the pixel centre (in pixels)
    // [channel R,G,B][x,y]   (y positive downwards)
    float off[3][2] = {{-0.22f, 0.50f}, {0.0f, 0.0f}, {0.27f, 0.52f}};
    float strength = 0.90f;
    // Experimental: shifts all three channels together (the luminance), in pixels, for panels whose white subpixel
    // moves the brightness away from the colour centroid. 0 = off. Not part of presets.
    float lumX = 0.0f, lumY = 0.0f;
    // Areas that are flat or only a smooth gradient (window shadows, backgrounds) are left unprocessed: the real
    // desktop shows through there, so switching between processed and unprocessed tiles cannot show. In 1/255 steps
    // of the colour range inside the 4x4 filter window; 0 = process everything.
    int flatSkip = 5;
    int kernel = K_CATMULL;
    int linear = 1;          // process in linear light (physically correct)
    int cleartype = 0;       // ClearType-compatible mode: account for ClearType's own horizontal shift
    int enabled = 1;
    int autoSuspend = 1;     // pause for fullscreen applications
    int overlayAlpha = 255;  // 255: 1:1 pixel composition (sharper). 254: translucent path, fallback
    wchar_t preset[64] = L"27\" 1440p QD-OLED";
    wchar_t gpu[128] = L"";  // empty = the GPU the monitor is connected to
    int fpsCap = 0;          // 0 = unlimited
    int skipUnchanged = 1;   // do not redraw when the desktop reports a change but the pixels are identical
    int motionBypass = 1;    // show moving regions directly (no added latency)
    int motionFrames = 3;    // this many changed frames...
    int motionWindowMs = 120;  // ...within this window count as "motion"
    int motionSettleMs = 250;  // re-apply the correction this long after motion stops
    int updateCheck = 1;       // look for a newer release on GitHub and install it
    int hdr = 0;               // experimental: process HDR (scRGB) desktops instead of pausing
    int overlayTest = OTF_TEST_DEFAULT;      // [Debug] OverlayTest=1: paint the left third of the overlay magenta (is the overlay visible at all?)
    int diag = OTF_DIAG_DEFAULT;  // write latency / timing statistics to OLEDTextFix.log once per second
    // Experimental (WOLED only): remove the colour ClearType adds to grey text before the subpixel correction.
    int ctCancel = 1;          // 1 = on (acts only with a WOLED preset and 8-bit capture)
    bool woled = false;        // the active preset is a WOLED layout (set by ApplyPreset / LoadConfig)
    float ctQ0 = 0.50f, ctQ1 = 0.80f;      // colour-cancellation gate: |mean chroma| / mean |chroma| ramp
    float ctLuma = 0.25f;                  // minimum luma contrast in the 7-px window (text-like edge)
    float ctRough0 = 1.00f, ctRough1 = 1.30f;  // subpixel roughness ramp (ClearType is smooth, UI/photos are not)
    float ctAmount = 1.0f;
};

struct Preset {
    std::wstring name;
    float off[3][2];
    float strength;
    int kernel;
    bool builtin;
    bool woled = false;
};


// QD-OLED subpixel layouts measured from macro photos with tools/measure_subpixels.js. Red/blue positions relative
// to green, in pixel pitch; green stays fixed so luminance is not resampled (sharpest). Each value is the median
// of all photos of that class (see README for the list): RTINGS, Notebookcheck, PC Monitors, TFTCentral.
#define QD_27QHD {{-0.22f, 0.50f}, {0.0f, 0.0f}, {0.27f, 0.52f}}  // 27" 1440p: 4 photos
#define QD_27_4K {{-0.17f, 0.50f}, {0.0f, 0.0f}, {0.32f, 0.50f}}  // 27" 4K: 3 photos
#define QD_32_4K {{-0.13f, 0.50f}, {0.0f, 0.0f}, {0.37f, 0.50f}}  // 32" 4K: 5 photos
#define QD_UW34  {{-0.31f, 0.49f}, {0.0f, 0.0f}, {0.22f, 0.49f}}  // 34" 3440x1440: 5 photos
#define QD_SUW49 {{-0.18f, 0.50f}, {0.0f, 0.0f}, {0.32f, 0.50f}}  // 49" 5120x1440: 2 photos
// LG WOLED column layouts relative to the white column, measured from RTINGS macro photos (median; see README).
// The white column is wider than the others, so the colour columns are not exactly 1/4 pixel apart.
#define WO_RWBG {{-0.32f, 0.0f}, {0.47f, 0.0f}, {0.20f, 0.0f}}   // R W B G: 4 photos
#define WO_RGWB {{-0.50f, 0.0f}, {-0.25f, 0.0f}, {0.22f, 0.0f}}  // R G W B: 6 photos

static const Preset kBuiltinPresets[] = {
    {L"27\" 1440p QD-OLED", QD_27QHD, 0.90f, K_CATMULL, true},
    {L"27\" 4K QD-OLED", QD_27_4K, 0.90f, K_CATMULL, true},
    {L"32\" 4K QD-OLED", QD_32_4K, 0.90f, K_CATMULL, true},
    {L"34\" 3440x1440 QD-OLED", QD_UW34, 0.90f, K_CATMULL, true},
    {L"49\" 5120x1440 QD-OLED", QD_SUW49, 0.90f, K_CATMULL, true},
    {L"Any QD-OLED triad — vertical only", {{0.0f, 0.50f}, {0.0f, 0.0f}, {0.0f, 0.50f}}, 0.90f, K_CATMULL, true},
    {L"QD-OLED legacy estimate (early QDTextFix default)", {{-0.20f, 0.20f}, {0.0f, -0.20f}, {0.20f, 0.20f}}, 0.90f, K_CATMULL, true},
    // LG WOLED: each pixel is four vertical columns, R W B G (older panels) or R G W B (2024+). Positions
    // are taken relative to the white column: the panel derives white from the input itself, so keeping it
    // as the reference preserves sharpness (see tools/sim_woled.py). Above ~0.25 strength fringing can get
    // worse on bright content, hence 0.20.
    {L"27\" 1440p 240 Hz WOLED — RWBG (2023-2024)", WO_RWBG, 0.20f, K_CATMULL, true},
    {L"27\" 1440p 480 Hz WOLED — RGWB (2024-2025)", WO_RGWB, 0.20f, K_CATMULL, true},
    {L"32\" 4K 240 Hz WOLED — RGWB (2024-2025)", WO_RGWB, 0.20f, K_CATMULL, true},
    {L"34\" 3440x1440 240 Hz WOLED — RWBG (2024)", WO_RWBG, 0.20f, K_CATMULL, true},
    {L"39\" 3440x1440 240 Hz WOLED — RWBG (2024)", WO_RWBG, 0.20f, K_CATMULL, true},
    {L"45\" 3440x1440 240 Hz WOLED — RWBG (2023-2024)", WO_RWBG, 0.20f, K_CATMULL, true},
    {L"45\" 5120x2160 165 Hz WOLED — RGWB (2025)", WO_RGWB, 0.20f, K_CATMULL, true},
    {L"39\" 5120x2160 Tandem WOLED — RGWB (2026)", WO_RGWB, 0.20f, K_CATMULL, true},
    {L"Any WOLED — RWBG", WO_RWBG, 0.20f, K_CATMULL, true},
    {L"Any WOLED — RGWB", WO_RGWB, 0.20f, K_CATMULL, true},
    // RGB-stripe panels (Tandem RGB WOLED, QD-OLED V-Stripe) already match ClearType: nothing to correct.
    {L"RGB stripe (Tandem RGB WOLED / QD-OLED V-Stripe) — no correction", {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}}, 0.0f, K_CATMULL, true},
};





static std::vector<Preset> g_presets;  // UI thread only

// Presets that shared identical values were merged; settings saved under an old name move to the merged one
static const wchar_t* CanonicalPresetName(const wchar_t* n) {
    static const wchar_t* const kOld[][2] = {
        {L"27\" 1440p 240/280 Hz QD-OLED", L"27\" 1440p QD-OLED"},
        {L"27\" 1440p 360 Hz QD-OLED (Gen 3, 2024)", L"27\" 1440p QD-OLED"},
        {L"27\" 1440p 500 Hz QD-OLED (Gen 3 update, 2025)", L"27\" 1440p QD-OLED"},
        {L"27\" 4K 240 Hz QD-OLED (Gen 4, 2025)", L"27\" 4K QD-OLED"},
        {L"32\" 4K 165 Hz QD-OLED", L"32\" 4K QD-OLED"},
        {L"32\" 4K 240 Hz QD-OLED (Gen 3, 2024)", L"32\" 4K QD-OLED"},
        {L"34\" 3440x1440 165/175 Hz QD-OLED (Gen 1, 2022)", L"34\" 3440x1440 QD-OLED"},
        {L"34\" 3440x1440 240 Hz QD-OLED (Gen 2, 2024)", L"34\" 3440x1440 QD-OLED"},
        {L"49\" 5120x1440 144/240 Hz QD-OLED (Gen 2, 2023)", L"49\" 5120x1440 QD-OLED"},
    };
    for (const auto& e : kOld)
        if (wcscmp(n, e[0]) == 0) return e[1];
    return n;
}

static Config g_cfg;
static SRWLOCK g_cfgLock = SRWLOCK_INIT;
static std::atomic<bool> g_ctBGR{false};  // is the Windows ClearType orientation BGR

static Config GetCfg() {
    AcquireSRWLockShared(&g_cfgLock);
    Config c = g_cfg;
    ReleaseSRWLockShared(&g_cfgLock);
    return c;
}

template <class F>
static void EditCfg(F f) {
    AcquireSRWLockExclusive(&g_cfgLock);
    f(g_cfg);
    ReleaseSRWLockExclusive(&g_cfgLock);
}

static float IniFloat(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_iniPath);
    if (!buf[0]) return def;
    wchar_t* end = nullptr;
    double v = wcstod(buf, &end);
    return end != buf ? (float)v : def;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, g_iniPath);
}
static void IniPutF(const wchar_t* sec, const wchar_t* key, float v) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f", v);
    WritePrivateProfileStringW(sec, key, buf, g_iniPath);
}
static void IniPutI(const wchar_t* sec, const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, g_iniPath);
}

static const wchar_t* kChKeys[3][2] = {{L"RedX", L"RedY"}, {L"GreenX", L"GreenY"}, {L"BlueX", L"BlueY"}};

#ifndef SPI_GETFONTSMOOTHINGORIENTATION
#define SPI_GETFONTSMOOTHINGORIENTATION 0x2012
#endif
#ifndef FE_FONTSMOOTHINGORIENTATIONBGR
#define FE_FONTSMOOTHINGORIENTATIONBGR 0x0000
#endif

// Read the Windows ClearType setting
static bool DetectClearType(bool* bgr = nullptr) {
    BOOL smooth = FALSE;
    UINT type = 0, orient = 1;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smooth, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &orient, 0);
    bool isBgr = orient == FE_FONTSMOOTHINGORIENTATIONBGR;
    g_ctBGR = isBgr;
    if (bgr) *bgr = isBgr;
    return smooth && type == FE_FONTSMOOTHINGCLEARTYPE;
}

static void LoadConfig() {
    Config c;
    bool firstRun = GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES;
    c.monitor = IniInt(L"General", L"Monitor", c.monitor);
    c.enabled = IniInt(L"General", L"Enabled", c.enabled);
    c.autoSuspend = IniInt(L"General", L"AutoSuspendFullscreen", c.autoSuspend);
    c.overlayAlpha = IniInt(L"General", L"OverlayAlpha", c.overlayAlpha);
    c.updateCheck = IniInt(L"General", L"CheckForUpdates", c.updateCheck);
    c.hdr = IniInt(L"General", L"ExperimentalHDR", c.hdr);
    c.diag = IniInt(L"Debug", L"Diagnostics", c.diag);
    c.overlayTest = std::max(0, std::min(2, IniInt(L"Debug", L"OverlayTest", c.overlayTest)));
    GetPrivateProfileStringW(L"General", L"Preset", c.preset, c.preset, 64, g_iniPath);
    wcsncpy(c.preset, CanonicalPresetName(c.preset), 63);
    c.preset[63] = 0;
    GetPrivateProfileStringW(L"Performance", L"Gpu", c.gpu, c.gpu, 128, g_iniPath);
    c.fpsCap = IniInt(L"Performance", L"FpsCap", c.fpsCap);
    c.skipUnchanged = IniInt(L"Performance", L"SkipUnchanged", c.skipUnchanged) ? 1 : 0;
    c.motionBypass = IniInt(L"Performance", L"MotionBypass", c.motionBypass);
    c.motionFrames = std::max(2, IniInt(L"Performance", L"MotionFrames", c.motionFrames));
    c.motionWindowMs = std::max(30, IniInt(L"Performance", L"MotionWindowMs", c.motionWindowMs));
    c.motionSettleMs = std::max(50, IniInt(L"Performance", L"MotionSettleMs", c.motionSettleMs));
    for (int ch = 0; ch < 3; ch++)
        for (int a = 0; a < 2; a++) c.off[ch][a] = IniFloat(L"Subpixel", kChKeys[ch][a], c.off[ch][a]);
    c.strength = IniFloat(L"Filter", L"Strength", c.strength);
    c.flatSkip = std::max(0, std::min(32, IniInt(L"Filter", L"FlatSkip", c.flatSkip)));
    c.lumX = std::max(-1.0f, std::min(1.0f, IniFloat(L"Filter", L"LumShiftX", c.lumX)));
    c.lumY = std::max(-1.0f, std::min(1.0f, IniFloat(L"Filter", L"LumShiftY", c.lumY)));
    c.kernel = IniInt(L"Filter", L"Kernel", c.kernel);
    c.linear = IniInt(L"Filter", L"LinearLight", c.linear);
    c.ctCancel = IniInt(L"Filter", L"ClearTypeCancel", c.ctCancel) ? 1 : 0;
    c.ctQ0 = IniFloat(L"Filter", L"CTCancelQ0", c.ctQ0);
    c.ctQ1 = IniFloat(L"Filter", L"CTCancelQ1", c.ctQ1);
    c.ctLuma = IniFloat(L"Filter", L"CTCancelLumaRange", c.ctLuma);
    c.ctRough0 = IniFloat(L"Filter", L"CTCancelRough0", c.ctRough0);
    c.ctRough1 = IniFloat(L"Filter", L"CTCancelRough1", c.ctRough1);
    c.ctAmount = IniFloat(L"Filter", L"CTCancelAmount", c.ctAmount);
    c.ctQ0 = std::min(std::max(c.ctQ0, 0.0f), 1.9f);
    c.ctQ1 = std::min(std::max(c.ctQ1, c.ctQ0 + 0.01f), 2.0f);
    c.ctLuma = std::min(std::max(c.ctLuma, 0.05f), 1.0f);
    c.ctRough0 = std::min(std::max(c.ctRough0, 0.3f), 2.9f);
    c.ctRough1 = std::min(std::max(c.ctRough1, c.ctRough0 + 0.01f), 3.0f);
    c.ctAmount = std::min(std::max(c.ctAmount, 0.0f), 1.0f);
    c.woled = IniInt(L"Subpixel", L"Woled", NameIsWoled(c.preset) ? 1 : 0) != 0;
    // On first run, pick ClearType mode from the Windows setting
    c.cleartype = IniInt(L"Filter", L"ClearTypeMode", firstRun ? (DetectClearType() ? 1 : 0) : 0);
    c.kernel = NormalizeKernel(c.kernel);
    if (c.overlayAlpha < 200) c.overlayAlpha = 200;
    if (c.overlayAlpha > 255) c.overlayAlpha = 255;
    EditCfg([&](Config& g) { g = c; });
}

static void SaveConfig() {
    Config c = GetCfg();
    IniPutI(L"General", L"Monitor", c.monitor);
    IniPutI(L"General", L"Enabled", c.enabled);
    IniPutI(L"General", L"AutoSuspendFullscreen", c.autoSuspend);
    IniPutI(L"General", L"OverlayAlpha", c.overlayAlpha);
    IniPutI(L"General", L"CheckForUpdates", c.updateCheck);
    IniPutI(L"General", L"ExperimentalHDR", c.hdr);
    WritePrivateProfileStringW(L"General", L"Preset", c.preset, g_iniPath);
    WritePrivateProfileStringW(L"Performance", L"Gpu", c.gpu, g_iniPath);
    IniPutI(L"Performance", L"FpsCap", c.fpsCap);
    IniPutI(L"Performance", L"MotionBypass", c.motionBypass);
    IniPutI(L"Performance", L"MotionFrames", c.motionFrames);
    IniPutI(L"Performance", L"MotionWindowMs", c.motionWindowMs);
    IniPutI(L"Performance", L"MotionSettleMs", c.motionSettleMs);
    for (int ch = 0; ch < 3; ch++)
        for (int a = 0; a < 2; a++) IniPutF(L"Subpixel", kChKeys[ch][a], c.off[ch][a]);
    IniPutF(L"Filter", L"Strength", c.strength);
    IniPutI(L"Filter", L"FlatSkip", c.flatSkip);
    IniPutF(L"Filter", L"LumShiftX", c.lumX);
    IniPutF(L"Filter", L"LumShiftY", c.lumY);
    IniPutI(L"Filter", L"Kernel", c.kernel);
    IniPutI(L"Filter", L"LinearLight", c.linear);
    IniPutI(L"Filter", L"ClearTypeMode", c.cleartype);
    IniPutI(L"Filter", L"ClearTypeCancel", c.ctCancel);
    IniPutF(L"Filter", L"CTCancelQ0", c.ctQ0);
    IniPutF(L"Filter", L"CTCancelQ1", c.ctQ1);
    IniPutF(L"Filter", L"CTCancelLumaRange", c.ctLuma);
    IniPutF(L"Filter", L"CTCancelRough0", c.ctRough0);
    IniPutF(L"Filter", L"CTCancelRough1", c.ctRough1);
    IniPutF(L"Filter", L"CTCancelAmount", c.ctAmount);
    IniPutI(L"Subpixel", L"Woled", c.woled ? 1 : 0);
}

// ---- presets: built-in ones + [Preset:Name] sections in the INI

static void LoadPresets() {
    g_presets.clear();
    for (const auto& p : kBuiltinPresets) {
        g_presets.push_back(p);
        g_presets.back().woled = NameIsWoled(p.name.c_str());
    }
    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), g_iniPath);
    for (wchar_t* s = buf.data(); n && *s; s += wcslen(s) + 1) {
        if (wcsncmp(s, L"Preset:", 7) != 0 || !s[7]) continue;
        Preset p{};
        p.name = s + 7;
        p.builtin = false;
        for (int ch = 0; ch < 3; ch++)
            for (int a = 0; a < 2; a++) p.off[ch][a] = IniFloat(s, kChKeys[ch][a], 0);
        p.strength = IniFloat(s, L"Strength", 1.0f);
        p.kernel = IniInt(s, L"Kernel", K_CATMULL);
        p.kernel = NormalizeKernel(p.kernel);
        p.woled = IniInt(s, L"Woled", NameIsWoled(p.name.c_str()) ? 1 : 0) != 0;
        g_presets.push_back(p);
    }
}

static int FindPreset(const wchar_t* name) {
    for (size_t i = 0; i < g_presets.size(); i++)
        if (g_presets[i].name == name) return (int)i;
    return -1;
}

static void WriteUserPreset(const std::wstring& name, const Config& c) {
    std::wstring sec = L"Preset:" + name;
    for (int ch = 0; ch < 3; ch++)
        for (int a = 0; a < 2; a++) IniPutF(sec.c_str(), kChKeys[ch][a], c.off[ch][a]);
    IniPutF(sec.c_str(), L"Strength", c.strength);
    IniPutI(sec.c_str(), L"Kernel", c.kernel);
    IniPutI(sec.c_str(), L"Woled", c.woled ? 1 : 0);
}

static void DeleteUserPreset(const std::wstring& name) {
    std::wstring sec = L"Preset:" + name;
    WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, g_iniPath);
}

// ---------------------------------------------------------------- global state

enum Status { ST_STARTING, ST_ACTIVE, ST_DISABLED, ST_FULLSCREEN, ST_HDR, ST_ACM, ST_WAITING, ST_ERROR, ST_FORMAT };
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_forceRedraw{true};
static std::atomic<bool> g_bypass{false};
static std::atomic<bool> g_fullscreenSuspend{false};
static std::atomic<bool> g_restart{false};
static std::atomic<int> g_status{ST_STARTING};
static std::atomic<HMONITOR> g_targetMon{nullptr};
static std::atomic<bool> g_overlayVisible{false};
static std::atomic<int> g_fps{0};
static wchar_t g_gpuName[128] = L"?";

static HINSTANCE g_inst;
static HWND g_msgWnd, g_overlay, g_setWnd;
static HICON g_icon;
static RECT g_monRect;  // UI thread uses after render thread sets (via message)
static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_APP_SETRECT = WM_APP + 2;
static const UINT WM_APP_UPDATE = WM_APP + 3;
static UINT WM_TASKBARCREATED;

// ---------------------------------------------------------------- monitor enumeration

struct OutInfo {
    int adapter = 0, output = 0;
    std::wstring name;
    RECT rc{};
    HMONITOR mon = nullptr;
    bool primary = false;
    std::wstring device;  // GDI device name, e.g. \\.\DISPLAY1
};

static std::vector<OutInfo> EnumOutputs() {
    std::vector<OutInfo> v;
    Com<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)f.put()))) return v;
    for (UINT a = 0;; a++) {
        Com<IDXGIAdapter1> ad;
        if (f->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad1{};
        ad->GetDesc1(&ad1);
        if (ad1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        for (UINT o = 0;; o++) {
            Com<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            out->GetDesc(&d);
            if (!d.AttachedToDesktop) continue;
            OutInfo oi;
            oi.adapter = (int)a;
            oi.output = (int)o;
            oi.rc = d.DesktopCoordinates;
            oi.device = d.DeviceName;
            oi.mon = d.Monitor;
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(d.Monitor, &mi)) oi.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            std::wstring friendly = L"Monitor";
            if (EnumDisplayDevicesW(d.DeviceName, 0, &dd, 0)) {
                friendly = dd.DeviceString;
            }
            wchar_t buf[256];
            swprintf(buf, 256, L"%ls — %ldx%ld%ls", friendly.c_str(), d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                     d.DesktopCoordinates.bottom - d.DesktopCoordinates.top, oi.primary ? L" (primary)" : L"");
            oi.name = buf;
            v.push_back(oi);
        }
    }
    return v;
}

static int PickOutput(const std::vector<OutInfo>& v, int want) {
    if (v.empty()) return -1;
    if (want >= 0 && want < (int)v.size()) return want;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].primary) return (int)i;
    return 0;
}

// ---------------------------------------------------------------- filter weights

struct alignas(16) CBData {
    int base[3][4];
    float wx[3][4];
    float wy[3][4];
    int size[4];
    int tile[4];
};

struct alignas(16) CTData {
    float gate[4];   // q0, q1, luma range, amount
    float rough[4];  // r0, r1, invWhite, unused
    int mode[4];     // decode sRGB input, encode sRGB output, BGR ClearType, scRGB
    int size[4];     // width - 1, height - 1
};

// ---------------------------------------------------------------- shaders

static const char kShader[] = R"HLSL(
Texture2D<float4> tex : register(t0);
cbuffer CB : register(b0) {
    int4 baseR, baseG, baseB;
    float4 wxR, wxG, wxB;
    float4 wyR, wyG, wyB;
    int4 sz;
    int4 tileInfo;  // x = tile size, y = scRGB, z = test mode, w = flat-area threshold (1/255 steps; < 0: raw values, > 0: linear light)
};
Texture2D<float> motionMask : register(t1);  // 1 = this tile is currently moving

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    if (tileInfo.z == 1 && p.x < sz.x / 3) return float4(1, 0, 1, 1);  // [Debug] OverlayTest=1: solid magenta band
    if (tileInfo.z == 2) {
        // OverlayTest=2: left third = inverted capture, right third = fixed 6 px red/blue split, independent of all
        // settings (shows whether the capture reaches the screen and whether this shader can change pixels at all)
        int3 q = int3(p, 0);
        if (p.x < sz.x / 3) return float4(1.0 - tex.Load(q).rgb, 1.0);
        if (p.x > (2 * sz.x) / 3) {
            float rr = tex.Load(int3(clamp(p + int2(6, 0), int2(0, 0), sz.xy), 0)).r;
            float bb = tex.Load(int3(clamp(p - int2(6, 0), int2(0, 0), sz.xy), 0)).b;
            return float4(rr, tex.Load(q).g, bb, 1.0);
        }
    }
    // Moving region: leave fully transparent so the real desktop shows through without latency
    if (motionMask.Load(int3(p / tileInfo.x, 0)) > 0.5) return float4(0, 0, 0, 0);
    float r = 0, g = 0, b = 0;
    float3 lo = float3(1e9, 1e9, 1e9), hi = float3(-1e9, -1e9, -1e9);
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int i = 0; i < 4; i++) {
            int2 o = int2(i, j);
            float sr = tex.Load(int3(clamp(p + baseR.xy + o, int2(0, 0), sz.xy), 0)).r;
            float sg = tex.Load(int3(clamp(p + baseG.xy + o, int2(0, 0), sz.xy), 0)).g;
            float sb = tex.Load(int3(clamp(p + baseB.xy + o, int2(0, 0), sz.xy), 0)).b;
            r += wxR[i] * wyR[j] * sr;
            g += wxG[i] * wyG[j] * sg;
            b += wxB[i] * wyB[j] * sb;
            float3 sv = float3(sr, sg, sb);
            lo = min(lo, sv);
            hi = max(hi, sv);
        }
    }
    // Flat or smoothly graded area (window shadow, background): nothing to correct, show the real desktop
    if (tileInfo.w != 0) {
        float3 a = lo, bb = hi;
        if (tileInfo.w > 0) { a = sqrt(max(a, 0.0)); bb = sqrt(max(bb, 0.0)); }  // linear light -> roughly perceptual
        float3 rg = bb - a;
        if (max(rg.x, max(rg.y, rg.z)) < abs(tileInfo.w) / 255.0) return float4(0, 0, 0, 0);
    }
    // HDR / Advanced Color desktops are scRGB (linear, FP16): values above 1 and below 0 are valid
    if (tileInfo.y != 0) return float4(r, g, b, 1.0);
    return float4(saturate(float3(r, g, b)), 1.0);
}

// Change detector: draws a pixel only where the new capture differs from the stored one. An occlusion predicate
// over this draw tells whether anything changed at all (the desktop often reports "everything changed" for frames
// that are pixel-identical, e.g. when only our own overlay was presented).
Texture2D<float4> diffNew : register(t2);
Texture2D<float4> diffOld : register(t3);
float4 PSDiff(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(int2(pos.xy), 0);
    if (all(diffNew.Load(p) == diffOld.Load(p))) discard;
    return float4(1, 1, 1, 1);
}

// Optional first pass (WOLED, experimental): removes the colour ClearType adds to grey/white/black text so that
// the channel shifts of PSMain act on a (nearly) greyscale-antialiased image. A pixel is only touched when all
// three gates agree it is ClearType text: the colour cancels along the row, the window has text-like luma
// contrast, and the 3x-rate subpixel signal is smooth. The result keeps each pixel's exact luminance.
cbuffer CTCB : register(b1) {
    float4 ctGate;   // x = q0 (0.50), y = q1 (0.80), z = lumaRange lr0 (0.25), w = amount (1.0)
    float4 ctRough;  // x = r0 (1.00), y = r1 (1.30), z = invWhite (always 1: 8-bit capture only), w = unused
    int4   ctMode;   // x = input is sRGB-encoded (decode it), y = encode output to sRGB, z = BGR ClearType, w = scRGB (no saturate)
    int4   ctSize;   // x = width - 1, y = height - 1
};

static const float3 LW = float3(0.2126, 0.7152, 0.0722);

float3 SrgbToLin(float3 e) { return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4); }
float3 LinToSrgb(float3 l) { l = max(l, 0); return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }
float  Ramp(float e0, float e1, float x) { return smoothstep(e0, e1, x); }

float4 PSCancel(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float3 c[7];
    float  Y[7];
    float3 Cm = 0;
    float  A = 0, Lmin = 1e9, Lmax = -1e9;
    [unroll] for (int i = 0; i < 7; i++) {
        int2 q = int2(clamp(p.x + i - 3, 0, ctSize.x), p.y);           // edge clamp = replicate border pixel
        float3 v = tex.Load(int3(q, 0)).rgb;
        if (ctMode.x != 0) v = SrgbToLin(v);                             // 10-bit / gamma path: decode first
        c[i] = v;
        Y[i] = dot(v, LW);
        float3 C = v - Y[i];
        Cm += C;
        A += length(C);
        float L = pow(saturate(Y[i] * ctRough.z), 1.0 / 2.2);            // perceptual luma, SDR-white normalised
        Lmin = min(Lmin, L);
        Lmax = max(Lmax, L);
    }
    Cm /= 7.0;
    A /= 7.0;
    // 1) ClearType colour of grey text cancels along the row: |mean C| << mean |C|
    float g = 1.0 - Ramp(ctGate.x, ctGate.y, length(Cm) / (A + 1e-4 / ctRough.z));
    // 2) text-like luma contrast in the window
    g *= Ramp(ctGate.z, 2.0 * ctGate.z, Lmax - Lmin);
    // 3) the 3x-rate subpixel signal (R0 G0 B0 R1 ...) is smooth for ClearType, jagged for coloured UI / photos
    if (g > 0.0) {
        float3 e0 = LinToSrgb(saturate(c[1] * ctRough.z)), e1 = LinToSrgb(saturate(c[2] * ctRough.z)),
               e2 = LinToSrgb(saturate(c[3] * ctRough.z)), e3 = LinToSrgb(saturate(c[4] * ctRough.z)),
               e4 = LinToSrgb(saturate(c[5] * ctRough.z));
        if (ctMode.z != 0) { e0 = e0.bgr; e1 = e1.bgr; e2 = e2.bgr; e3 = e3.bgr; e4 = e4.bgr; }
        float s[11] = {e0.b, e1.r, e1.g, e1.b, e2.r, e2.g, e2.b, e3.r, e3.g, e3.b, e4.r};
        float D1 = 0, D2 = 0;
        [unroll] for (int k = 1; k <= 9; k++) {
            D1 += abs(s[k] - s[k - 1]);
            D2 += abs(s[k + 1] - 2.0 * s[k] + s[k - 1]);
        }
        g *= 1.0 - Ramp(ctRough.x, ctRough.y, D2 / (D1 + 3e-3));
    }
    g *= ctGate.w;
    // Desaturate toward the pixel's own luminance: Y is exact and the result is a convex mix of the pixel and
    // a grey of the same luminance, so it never leaves [0,1] (SDR) and never adds negative scRGB values.
    float3 o = lerp(c[3], Y[3].xxx, g);
    if (ctMode.y != 0) o = LinToSrgb(saturate(o));
    return float4(o, 1.0);
}
)HLSL";

// ---------------------------------------------------------------- GPU list

struct GpuInfo {
    std::wstring name;
    LUID luid;
};

static std::vector<GpuInfo> EnumGpus() {
    std::vector<GpuInfo> v;
    Com<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)f.put()))) return v;
    for (UINT a = 0;; a++) {
        Com<IDXGIAdapter1> ad;
        if (f->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        bool dupe = false;
        for (auto& g : v) dupe |= g.luid.LowPart == d.AdapterLuid.LowPart && g.luid.HighPart == d.AdapterLuid.HighPart;
        if (!dupe) v.push_back({d.Description, d.AdapterLuid});
    }
    return v;
}

// ---------------------------------------------------------------- renderer (render thread)

typedef HRESULT(WINAPI* PFN_DCompCreate)(IDXGIDevice*, REFIID, void**);

static std::atomic<int> g_ddaFps{0};
static std::atomic<bool> g_cross{false};
static std::atomic<int> g_motionTiles{0};
static wchar_t g_capGpuName[128] = L"?";

static bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

static D3D11_BOX BoxOf(const RECT& r) { return D3D11_BOX{(UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1}; }

// Desktop Duplication returns the desktop in its native format: 8-bit normally, FP16 scRGB when HDR or
// Auto Color Management (Advanced Color) is on, and possibly 10-bit.
static DXGI_FORMAT TypelessFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        default: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }
}
static const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "FP16 scRGB (HDR / Auto Color Management)";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "10-bit";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "8-bit";
        default: return "unsupported";
    }
}

// Timing statistics for the diagnostic log (all values in milliseconds)
struct DiagStat {
    double sum = 0, mx = 0;
    int n = 0;
    void add(double v) { sum += v; if (v > mx) mx = v; n++; }
    double avg() const { return n ? sum / n : 0; }
    void reset() { *this = DiagStat(); }
};

struct Renderer {
    // diagnostics (see Config::diag)
    DiagStat dAcq, dIngest, dRender, dPresent, dGpu, dLat, dArea;
    int dBuckets[4] = {0, 0, 0, 0}, dMaxRects = 0, dMaxTiles = 0, dTimeouts = 0, dSlowLogs = 0;
    double dLastPresentMs = 0;
    int dPresentLogs = 0, dOvTick = 0;
    Com<ID3D11Query> dQDisjoint[3], dQStart[3], dQEnd[3];
    int dQWrite = 0, dQPending = 0;
    bool dQReady[3] = {false, false, false};
    bool dHeader = false;
    // capture side (always the GPU the monitor is connected to)
    Com<ID3D11Device> cdev;
    Com<ID3D11DeviceContext> cctx;
    Com<IDXGIOutput1> out1;
    Com<IDXGIOutputDuplication> dup;
    Com<ID3D11Texture2D> stage;  // cross-adapter mode only: CPU readback
    std::vector<uint8_t> shadow;  // CPU copy of the frame in cross-adapter mode
    // processing side (selected GPU)
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    bool cross = false;
    bool forceSame = false;  // advanced-colour capture only works on the monitor's GPU
    DXGI_FORMAT capFmt = DXGI_FORMAT_B8G8R8A8_UNORM;  // format of the captured desktop (and of the overlay swap chain)
    Com<IDXGISwapChain1> sc;
    Com<IDCompositionDevice> dc;
    Com<IDCompositionTarget> tgt;
    Com<IDCompositionVisual> vis;
    Com<ID3D11Texture2D> src[2];
    Com<ID3D11ShaderResourceView> srv[2][2];  // [texture][0 = UNORM, 1 = sRGB]
    int cur = 0;
    Com<ID3D11RenderTargetView> rtv[2];
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11Buffer> cb;
    // optional ClearType colour-removal pre-pass (WOLED, experimental); resources are created on first use
    Com<ID3D11PixelShader> psCt;
    Com<ID3D11Buffer> cbCt;
    Com<ID3D11Texture2D> ctTex;
    Com<ID3D11ShaderResourceView> ctSrv;
    Com<ID3D11RenderTargetView> ctRtv;
    bool ctFailed = false;   // creating the pre-pass resources failed: stay off until the size or device changes
    bool lastCtOn = false;   // the last drawn frame used the pre-pass (widens the cross-adapter copy margin)
    // change detector for same-GPU capture (see PSDiff); resources are created on first use
    Com<ID3D11PixelShader> psDiff;
    Com<ID3D11Texture2D> cmpTex, cmpRt;
    Com<ID3D11ShaderResourceView> cmpSrv;
    Com<ID3D11RenderTargetView> cmpRtv;
    Com<ID3D11RasterizerState> rsScissor;
    Com<ID3D11Query> qDiff;
    bool diffFailed = false;
    int dSame = 0;           // diagnostics: desktop updates whose pixels were identical
    UINT w = 0, h = 0;
    bool haveFrame = false;
    std::vector<uint8_t> meta;
    std::vector<RECT> rects;
    // cross-adapter mode: the result drawn on the processing GPU is moved to the display GPU via the CPU;
    // presentation (swap chain + DirectComposition) always happens on the monitor's GPU.
    Com<ID3D11Texture2D> outProc, outStage, outDisp;
    Com<ID3D11RenderTargetView> outRtv[2];
    std::vector<RECT> acc;  // regions changed since the last draw
    bool accFull = true;
    int hideLogs = 0;
    // per-region motion detection: the screen is split into TILE x TILE tiles
    static const int TILE = 64;
    int tw = 0, th = 0;
    std::vector<LONGLONG> tFirst, tUntil;
    std::vector<uint16_t> tCount;
    std::vector<uint8_t> mask, lastMask;
    Com<ID3D11Texture2D> maskTex;
    Com<ID3D11ShaderResourceView> maskSrv;

    void Hide(const char* why = "") {
        if (g_overlayVisible.exchange(false)) {
            SetWindowPos(g_overlay, nullptr, 0, 0, 0, 0,
                         SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            if (why && *why && hideLogs < 300) { hideLogs++; Log("Overlay hidden: %s", why); }
        }
    }
    void Show() {
        // z-order is only set when showing (periodic "bring to top" could cause flicker)
        if (!g_overlayVisible.exchange(true))
            SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }

    void ReleaseSized() {
        for (auto& r : rtv) r.reset();
        for (auto& t : srv)
            for (auto& s : t) s.reset();
        for (auto& s : src) s.reset();
        stage.reset();
        shadow.clear();
        for (auto& r : outRtv) r.reset();
        outProc.reset();
        outStage.reset();
        outDisp.reset();
        acc.clear();
        accFull = true;
        maskSrv.reset();
        maskTex.reset();
        ctRtv.reset();
        ctSrv.reset();
        ctTex.reset();
        ctFailed = false;
        cmpRtv.reset();
        cmpSrv.reset();
        cmpTex.reset();
        cmpRt.reset();
        diffFailed = false;
        tw = th = 0;
    }

    void Teardown() {
        Hide();
        dup.reset();
        ReleaseSized();
        if (dc) {
            if (tgt) tgt->SetRoot(nullptr);
            dc->Commit();
        }
        vis.reset();
        tgt.reset();
        dc.reset();
        sc.reset();
        cb.reset();
        cbCt.reset();
        psCt.reset();
        psDiff.reset();
        rsScissor.reset();
        qDiff.reset();
        ps.reset();
        vs.reset();
        out1.reset();
        for (int k = 0; k < 3; k++) { dQDisjoint[k].reset(); dQStart[k].reset(); dQEnd[k].reset(); dQReady[k] = false; }
        dQPending = -1;
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        if (cctx) { cctx->ClearState(); cctx->Flush(); }
        ctx.reset();
        dev.reset();
        cctx.reset();
        cdev.reset();
        w = h = 0;
        haveFrame = false;
    }

    bool CompileShaders() {
        static HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!comp) { Log("Could not load d3dcompiler_47.dll"); return false; }
        auto D3DCompileFn = (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile");
        if (!D3DCompileFn) return false;
        auto compile = [&](const char* srcText, size_t len, const char* entry, const char* target, Com<ID3DBlob>& out) {
            Com<ID3DBlob> err;
            HRESULT hr = D3DCompileFn(srcText, len, "oledtextfix", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      out.put(), err.put());
            if (FAILED(hr)) Log("%s compile error: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
            return SUCCEEDED(hr);
        };
        Com<ID3DBlob> vsb, psb;
        if (!compile(kShader, sizeof(kShader) - 1, "VSMain", "vs_5_0", vsb)) return false;
        if (!compile(kShader, sizeof(kShader) - 1, "PSMain", "ps_5_0", psb)) return false;
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.put()))) return false;
        if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.put()))) return false;
        // The ClearType pre-pass is optional: if it does not compile, the filter still works without it
        Com<ID3DBlob> ctb;
        if (compile(kShader, sizeof(kShader) - 1, "PSCancel", "ps_5_0", ctb) &&
            SUCCEEDED(dev->CreatePixelShader(ctb->GetBufferPointer(), ctb->GetBufferSize(), nullptr, psCt.put())))
            ;
        else
            psCt.reset();
        Com<ID3DBlob> dfb;
        if (compile(kShader, sizeof(kShader) - 1, "PSDiff", "ps_5_0", dfb) &&
            SUCCEEDED(dev->CreatePixelShader(dfb->GetBufferPointer(), dfb->GetBufferSize(), nullptr, psDiff.put())))
            ;
        else
            psDiff.reset();
        return true;
    }

    static bool CreateDevice(IDXGIAdapter1* ad, Com<ID3D11Device>& d, Com<ID3D11DeviceContext>& c) {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                       D3D11_SDK_VERSION, d.put(), nullptr, c.put());
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                                   D3D11_SDK_VERSION, d.put(), nullptr, c.put());
        if (FAILED(hr)) Log("D3D11CreateDevice error 0x%08lx", hr);
        return SUCCEEDED(hr);
    }

    bool Init() {
        Config cfg = GetCfg();
        auto outs = EnumOutputs();
        int idx = PickOutput(outs, cfg.monitor);
        if (idx < 0) { Log("No monitor attached to the desktop"); return false; }
        const OutInfo& oi = outs[idx];
        Log("Target monitor: #%d (adapter %d, output %d)", idx, oi.adapter, oi.output);

        Com<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)f.put()))) return false;
        Com<IDXGIAdapter1> capAd;
        if (FAILED(f->EnumAdapters1(oi.adapter, capAd.put()))) return false;
        DXGI_ADAPTER_DESC1 capDesc{};
        capAd->GetDesc1(&capDesc);
        Com<IDXGIOutput> out;
        if (FAILED(capAd->EnumOutputs(oi.output, out.put()))) return false;
        if (FAILED(out.as(out1))) { Log("IDXGIOutput1 not available"); return false; }

        DXGI_OUTPUT_DESC od{};
        out->GetDesc(&od);
        if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            Log("Rotated displays are not supported");
            return false;
        }
        g_targetMon = od.Monitor;

        // Processing GPU: the monitor's GPU if empty, otherwise the GPU chosen by name
        Com<IDXGIAdapter1> procAd;
        if (cfg.gpu[0]) {
            for (UINT a = 0;; a++) {
                Com<IDXGIAdapter1> ad;
                if (f->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 d{};
                ad->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !wcscmp(d.Description, cfg.gpu)) {
                    procAd.p = ad.p;
                    ad.p = nullptr;
                    break;
                }
            }
            if (!procAd) Log("Selected GPU not found, using the monitor's GPU");
        }
        DXGI_ADAPTER_DESC1 procDesc = capDesc;
        if (procAd) procAd->GetDesc1(&procDesc);
        cross = procAd && !SameLuid(procDesc.AdapterLuid, capDesc.AdapterLuid) && !forceSame;
        g_cross = cross;
        swprintf(g_capGpuName, 128, L"%ls", capDesc.Description);
        swprintf(g_gpuName, 128, L"%ls", procDesc.Description);
        Log("Capture GPU: %ls | Processing GPU: %ls%s", capDesc.Description, procDesc.Description, cross ? " (cross-adapter)" : "");

        if (!CreateDevice(capAd.get(), cdev, cctx)) return false;
        if (cross) {
            if (!CreateDevice(procAd.get(), dev, ctx)) return false;
        } else {
            cdev.as(dev);
            cctx.as(ctx);
        }

        if (!CompileShaders()) return false;
        {
            Com<IDXGIDevice1> d1;
            if (SUCCEEDED(cdev.as(d1))) d1->SetMaximumFrameLatency(1);  // keep the present queue as short as possible
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CBData);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.put()))) return false;
        bd.ByteWidth = sizeof(CTData);
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cbCt.put()))) cbCt.reset();

        // DirectComposition: always on the monitor's GPU (cross-adapter presentation flickers)
        static HMODULE dcompDll = LoadLibraryW(L"dcomp.dll");
        auto create = dcompDll ? (PFN_DCompCreate)(void*)GetProcAddress(dcompDll, "DCompositionCreateDevice") : nullptr;
        if (!create) { Log("dcomp.dll not available"); return false; }
        Com<IDXGIDevice> dxgiDev;
        cdev.as(dxgiDev);
        if (FAILED(create(dxgiDev.get(), __uuidof(IDCompositionDevice), (void**)dc.put()))) { Log("Could not create DComp device"); return false; }
        if (FAILED(dc->CreateTargetForHwnd(g_overlay, TRUE, tgt.put()))) { Log("Could not create DComp target"); return false; }
        if (FAILED(dc->CreateVisual(vis.put()))) return false;

        PostMessageW(g_msgWnd, WM_APP_SETRECT, 0, 0);
        return true;
    }

    bool MakeSrc(int i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = TypelessFor(capFmt);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, src[i].put()))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = capFmt;
        if (FAILED(dev->CreateShaderResourceView(src[i].get(), &sd, srv[i][0].put()))) return false;
        srv[i][1].reset();
        if (capFmt == DXGI_FORMAT_B8G8R8A8_UNORM) {  // the sRGB view (linear-light option) only exists for 8-bit
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (FAILED(dev->CreateShaderResourceView(src[i].get(), &sd, srv[i][1].put()))) srv[i][1].reset();
        }
        return true;
    }

    bool EnsureSize(UINT nw, UINT nh) {
        if (nw == w && nh == h && sc) return true;
        ReleaseSized();
        w = nw;
        h = nh;
        if (!sc) {
            Com<IDXGIDevice> dxgiDev;
            cdev.as(dxgiDev);
            Com<IDXGIAdapter> ad;
            dxgiDev->GetAdapter(ad.put());
            Com<IDXGIFactory2> f2;
            if (FAILED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)f2.put()))) return false;
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = w;
            d.Height = h;
            d.Format = capFmt;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.Scaling = DXGI_SCALING_STRETCH;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;  // moving tiles are transparent
            HRESULT hr = f2->CreateSwapChainForComposition(cdev.get(), &d, nullptr, sc.put());
            if (FAILED(hr)) { Log("Swap chain error 0x%08lx", hr); return false; }
            if (capFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                Com<IDXGISwapChain3> sc3;
                if (SUCCEEDED(sc.as(sc3))) {
                    HRESULT cs = sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);  // scRGB: linear, sRGB primaries
                    if (FAILED(cs)) Log("SetColorSpace1(scRGB) failed 0x%08lx", cs);
                }
            }
            vis->SetContent(sc.get());
            tgt->SetRoot(vis.get());
            dc->Commit();
        } else {
            HRESULT hr = sc->ResizeBuffers(2, w, h, capFmt, 0);
            if (FAILED(hr)) { Log("ResizeBuffers error 0x%08lx", hr); return false; }
        }
        // Render target: the back buffer directly on the same GPU; an intermediate texture on the processing GPU in cross mode
        Com<ID3D11Texture2D> target;
        if (!cross) {
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)target.put()))) return false;
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, outProc.put()))) return false;
            target.p = outProc.p;
            target.p->AddRef();
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, outStage.put()))) return false;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.CPUAccessFlags = 0;
            if (FAILED(cdev->CreateTexture2D(&td, nullptr, outDisp.put()))) return false;
        }
        Com<ID3D11RenderTargetView>* views = cross ? outRtv : rtv;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Format = capFmt;
        if (FAILED(dev->CreateRenderTargetView(target.get(), &rd, views[0].put()))) return false;
        views[1].reset();
        if (capFmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
            rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (FAILED(dev->CreateRenderTargetView(target.get(), &rd, views[1].put()))) {
                Log("sRGB RTV not supported — linear mode will run in gamma space");
                views[1].reset();
            }
        }
        accFull = true;
        tw = (int)(w + TILE - 1) / TILE;
        th = (int)(h + TILE - 1) / TILE;
        tFirst.assign((size_t)tw * th, 0);
        tUntil.assign((size_t)tw * th, 0);
        tCount.assign((size_t)tw * th, 0);
        mask.assign((size_t)tw * th, 0);
        lastMask.assign((size_t)tw * th, 0);
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)tw;
            td.Height = (UINT)th;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{mask.data(), (UINT)tw, 0};
            if (FAILED(dev->CreateTexture2D(&td, &init, maskTex.put()))) return false;
            if (FAILED(dev->CreateShaderResourceView(maskTex.get(), nullptr, maskSrv.put()))) return false;
        }
        if (!MakeSrc(0)) return false;
        cur = 0;
        if (cross) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(cdev->CreateTexture2D(&td, nullptr, stage.put()))) return false;
            shadow.assign((size_t)w * h * 4, 0);
        }
        haveFrame = false;
        return true;
    }

    bool IsHdr() {
        Com<IDXGIOutput6> o6;
        if (FAILED(out1.as(o6))) return false;
        DXGI_OUTPUT_DESC1 d1{};
        if (FAILED(o6->GetDesc1(&d1))) return false;
        return d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }

    bool StartDuplication() {
        const bool hdrOn = IsHdr();
        if (hdrOn && !GetCfg().hdr) {
            g_status = ST_HDR;
            return false;
        }
        HRESULT hr;
        if (hdrOn) {
            // HDR: the legacy call would hand out a tone-mapped copy; DuplicateOutput1 returns the real scRGB desktop
            Com<IDXGIOutput5> o5;
            if (FAILED(out1.as(o5))) {
                Log("HDR capture needs IDXGIOutput5 (Windows 10 1709 or later)");
                g_status = ST_HDR;
                return false;
            }
            const DXGI_FORMAT want[] = {DXGI_FORMAT_R16G16B16A16_FLOAT};
            hr = o5->DuplicateOutput1(cdev.get(), 0, 1, want, dup.put());
        } else {
            hr = out1->DuplicateOutput(cdev.get(), dup.put());
        }
        if (FAILED(hr)) {
            static HRESULT last = 0;
            if (hr != last) Log("DuplicateOutput error 0x%08lx", hr);
            last = hr;
            g_status = ST_WAITING;
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        const DXGI_FORMAT f = dd.ModeDesc.Format;
        if (!hdrOn && f == DXGI_FORMAT_R16G16B16A16_FLOAT && !GetCfg().hdr) {
            // Auto Color Management (SDR output, 16-bit desktop): the overlay comes out black or far too dark here, so
            // leave the desktop alone rather than show a wrong picture
            static bool logged = false;
            if (!logged) { logged = true; Log("Auto Color Management is on: paused (the 16-bit desktop is not supported yet)"); }
            dup.reset();
            g_status = ST_ACM;
            return false;
        }
        if (f != DXGI_FORMAT_B8G8R8A8_UNORM && f != DXGI_FORMAT_R10G10B10A2_UNORM && f != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            Log("Unsupported desktop format %d", (int)f);
            dup.reset();
            g_status = ST_FORMAT;
            return false;
        }
        if (f != DXGI_FORMAT_B8G8R8A8_UNORM && cross) {
            Log("Advanced-colour capture (%s) needs the monitor's GPU: ignoring the Processing GPU setting", FormatName(f));
            dup.reset();
            forceSame = true;
            g_restart = true;
            return false;
        }
        if (f != capFmt || !sc) {
            Log("Desktop format: %s%s", FormatName(f), hdrOn ? " (HDR on)" : "");
            ReleaseSized();
            sc.reset();  // the overlay swap chain must use the same format as the capture
            w = h = 0;
            capFmt = f;
        }
        if (!EnsureSize(dd.ModeDesc.Width, dd.ModeDesc.Height)) {
            dup.reset();
            g_status = ST_ERROR;
            return false;
        }
        haveFrame = false;  // never show a stale frame when restarting
        accFull = true;
        Log("Capture started %ux%u @ %.2f Hz", w, h,
            dd.ModeDesc.RefreshRate.Denominator ? (double)dd.ModeDesc.RefreshRate.Numerator / dd.ModeDesc.RefreshRate.Denominator : 0.0);
        return true;
    }

    // Regions changed in this frame (dirty rects + move-rect destinations)
    void GatherRects(const DXGI_OUTDUPL_FRAME_INFO& fi) {
        rects.clear();
        if (!haveFrame || fi.TotalMetadataBufferSize == 0) {
            if (!haveFrame) rects.push_back(RECT{0, 0, (LONG)w, (LONG)h});
            return;
        }
        meta.resize(fi.TotalMetadataBufferSize);
        UINT size = 0;
        if (SUCCEEDED(dup->GetFrameMoveRects((UINT)meta.size(), (DXGI_OUTDUPL_MOVE_RECT*)meta.data(), &size))) {
            auto mv = (DXGI_OUTDUPL_MOVE_RECT*)meta.data();
            for (UINT i = 0; i < size / sizeof(DXGI_OUTDUPL_MOVE_RECT); i++) rects.push_back(mv[i].DestinationRect);
        }
        if (SUCCEEDED(dup->GetFrameDirtyRects((UINT)meta.size(), (RECT*)meta.data(), &size))) {
            auto dr = (RECT*)meta.data();
            for (UINT i = 0; i < size / sizeof(RECT); i++) rects.push_back(dr[i]);
        }
        RECT u{(LONG)w, (LONG)h, 0, 0};
        std::vector<RECT> ok;
        for (RECT r : rects) {
            r.left = std::max(0L, r.left);
            r.top = std::max(0L, r.top);
            r.right = std::min((LONG)w, r.right);
            r.bottom = std::min((LONG)h, r.bottom);
            if (r.right <= r.left || r.bottom <= r.top) continue;
            ok.push_back(r);
            u.left = std::min(u.left, r.left);
            u.top = std::min(u.top, r.top);
            u.right = std::max(u.right, r.right);
            u.bottom = std::max(u.bottom, r.bottom);
        }
        rects.swap(ok);
        if (rects.size() > 48) rects.assign(1, u);  // too fragmented: use one bounding box
    }

    bool EnsureDiff() {
        if (diffFailed) return false;
        if (cmpTex && qDiff && rsScissor) return true;
        if (!psDiff) { diffFailed = true; return false; }
        if (!rsScissor) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.ScissorEnable = TRUE;
            rd.DepthClipEnable = TRUE;
            if (FAILED(dev->CreateRasterizerState(&rd, rsScissor.put()))) { diffFailed = true; return false; }
        }
        if (!qDiff) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
            if (FAILED(dev->CreateQuery(&qd, qDiff.put()))) { diffFailed = true; return false; }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = TypelessFor(capFmt);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = capFmt;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, cmpTex.put())) &&
                  SUCCEEDED(dev->CreateShaderResourceView(cmpTex.get(), &sd, cmpSrv.put()));
        if (ok) {
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, cmpRt.put())) &&
                 SUCCEEDED(dev->CreateRenderTargetView(cmpRt.get(), nullptr, cmpRtv.put()));
        }
        if (!ok) {
            cmpRtv.reset(); cmpSrv.reset(); cmpTex.reset(); cmpRt.reset();
            diffFailed = true;
            Log("Change detector: could not create resources, every desktop update will be redrawn");
            return false;
        }
        return true;
    }

    // True if any pixel inside the current rects differs between cmpTex (new) and src[0] (stored).
    // On any problem it answers true, which is the old behaviour (redraw).
    bool RectsDiffer() {
        D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rsScissor.get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->PSSetShader(psDiff.get(), nullptr, 0);
        ID3D11ShaderResourceView* in[2] = {cmpSrv.get(), srv[0][0].get()};
        ctx->PSSetShaderResources(2, 2, in);
        ID3D11RenderTargetView* rt = cmpRtv.get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->Begin(qDiff.get());
        for (const RECT& r : rects) {
            ctx->RSSetScissorRects(1, &r);
            ctx->Draw(3, 0);
        }
        ctx->End(qDiff.get());
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(2, 2, none);
        ctx->RSSetState(nullptr);
        ctx->Flush();
        BOOL any = TRUE;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        HRESULT hr;
        while ((hr = ctx->GetData(qDiff.get(), &any, sizeof any, 0)) == S_FALSE) {
            QueryPerformanceCounter(&t1);
            if ((t1.QuadPart - t0.QuadPart) * 1000 > f.QuadPart * 4) return true;  // GPU busy: do not wait, just redraw
            YieldProcessor();
        }
        return hr != S_OK || any != FALSE;
    }

    // Same-GPU path: copy only the changed regions of the new frame into our persistent copy of the desktop
    // (the first frame after a (re)start reports the whole screen as one region).
    // Returns false when the pixels turned out to be identical to what we already have.
    bool IngestSameGpu(ID3D11Texture2D* frame) {
        double area = 0;
        for (const RECT& r : rects) area += (double)(r.right - r.left) * (r.bottom - r.top);
        // Only large updates are checked: small ones are real in practice and not worth the extra pass
        if (haveFrame && GetCfg().skipUnchanged && area >= 0.25 * w * h && EnsureDiff()) {
            for (const RECT& r : rects) {
                D3D11_BOX box = BoxOf(r);
                ctx->CopySubresourceRegion(cmpTex.get(), 0, r.left, r.top, 0, frame, 0, &box);
            }
            if (!RectsDiffer()) { dSame++; return false; }
            for (const RECT& r : rects) {
                D3D11_BOX box = BoxOf(r);
                ctx->CopySubresourceRegion(src[0].get(), 0, r.left, r.top, 0, cmpTex.get(), 0, &box);
            }
            cur = 0;
            return true;
        }
        for (const RECT& r : rects) {
            D3D11_BOX box = BoxOf(r);
            ctx->CopySubresourceRegion(src[0].get(), 0, r.left, r.top, 0, frame, 0, &box);
        }
        cur = 0;
        return true;
    }

    // Cross-GPU path: move changed regions to the processing GPU via the CPU
    bool IngestCross(ID3D11Texture2D* frame) {
        for (const RECT& r : rects) {
            D3D11_BOX box = BoxOf(r);
            cctx->CopySubresourceRegion(stage.get(), 0, r.left, r.top, 0, frame, 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(cctx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        bool any = false;
        const size_t pitch = (size_t)w * 4;
        const bool skip = haveFrame && GetCfg().skipUnchanged;
        for (const RECT& r : rects) {
            size_t bytes = (size_t)(r.right - r.left) * 4;
            bool differs = !skip;
            for (LONG y = r.top; y < r.bottom; y++) {
                const uint8_t* sp = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)r.left * 4;
                uint8_t* dp = shadow.data() + (size_t)y * pitch + (size_t)r.left * 4;
                if (!differs && memcmp(dp, sp, bytes) != 0) differs = true;
                memcpy(dp, sp, bytes);
            }
            if (!differs) continue;
            D3D11_BOX box = BoxOf(r);
            ctx->UpdateSubresource(src[0].get(), 0, &box, shadow.data() + (size_t)r.top * pitch + (size_t)r.left * 4, (UINT)pitch, 0);
            any = true;
        }
        if (!any && skip) dSame++;
        cctx->Unmap(stage.get(), 0);
        cur = 0;
        return any;
    }

    // GPU time of the draw, measured with timestamp queries and read back two frames later without stalling
    void DiagGpuPoll() {
        for (int k = 0; k < 3; k++) {
            if (!dQReady[k]) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(dQDisjoint[k].get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (ctx->GetData(dQStart[k].get(), &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (ctx->GetData(dQEnd[k].get(), &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            dQReady[k] = false;
            if (!dj.Disjoint && dj.Frequency) dGpu.add((t1 - t0) * 1000.0 / dj.Frequency);
        }
    }
    void DiagGpuBegin() {
        DiagGpuPoll();
        int k = dQWrite;
        if (dQReady[k]) { dQPending = -1; return; }  // all slots still in flight: skip this frame
        if (!dQDisjoint[k]) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(dev->CreateQuery(&qd, dQDisjoint[k].put()))) { dQPending = -1; return; }
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, dQStart[k].put())) || FAILED(dev->CreateQuery(&qd, dQEnd[k].put()))) {
                dQDisjoint[k].reset();
                dQPending = -1;
                return;
            }
        }
        ctx->Begin(dQDisjoint[k].get());
        ctx->End(dQStart[k].get());
        dQPending = k;
    }
    void DiagGpuEnd() {
        int k = dQPending;
        if (k < 0) return;
        ctx->End(dQEnd[k].get());
        ctx->End(dQDisjoint[k].get());
        dQReady[k] = true;
        dQWrite = (k + 1) % 3;
        dQPending = -1;
    }

    // FP16 target for the ClearType pre-pass, created lazily so QD-OLED users never pay the memory
    bool EnsureCt() {
        if (ctTex && ctSrv && ctRtv) return true;
        if (ctFailed || !psCt || !cbCt) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, ctTex.put())) ||
            FAILED(dev->CreateShaderResourceView(ctTex.get(), nullptr, ctSrv.put())) ||
            FAILED(dev->CreateRenderTargetView(ctTex.get(), nullptr, ctRtv.put()))) {
            ctRtv.reset();
            ctSrv.reset();
            ctTex.reset();
            ctFailed = true;
            Log("ClearType pre-pass: could not create the FP16 target, feature off");
            return false;
        }
        return true;
    }

    void Render() {
        Config c = GetCfg();
        bool bypass = g_bypass.load();
        // pre-pass only for WOLED layouts, 8-bit capture (HDR/10-bit untested), and when the user turned it on
        bool ctOn = !bypass && c.ctCancel && c.woled && capFmt == DXGI_FORMAT_B8G8R8A8_UNORM && EnsureCt();
        lastCtOn = ctOn;
        CBData d{};
        const float lumX = c.woled ? c.lumX : 0.0f, lumY = c.woled ? c.lumY : 0.0f;  // WOLED layouts only
        for (int ch = 0; ch < 3; ch++) {
            if (bypass) {
                d.base[ch][0] = d.base[ch][1] = -1;
                float id[4] = {0, 1, 0, 0};
                memcpy(d.wx[ch], id, sizeof id);
                memcpy(d.wy[ch], id, sizeof id);
            } else {
                // ClearType-compatible mode: ClearType already draws red shifted by -1/3 px and blue by +1/3 px
                // (reversed for BGR). Required shift = panel position - position ClearType assumes.
                float ct = 0;
                if (c.cleartype && !ctOn && ch != 1) ct = ((ch == 0) != g_ctBGR.load()) ? -1.0f / 3 : 1.0f / 3;
                ChannelTaps((c.off[ch][0] - ct) * c.strength + lumX, c.kernel, d.base[ch][0], d.wx[ch]);
                ChannelTaps(c.off[ch][1] * c.strength + lumY, c.kernel, d.base[ch][1], d.wy[ch]);
            }
        }
        d.size[0] = (int)w - 1;
        d.size[1] = (int)h - 1;
        d.tile[0] = TILE;
        d.tile[2] = c.overlayTest;
        {
            Com<ID3D11RenderTargetView>* v0 = cross ? outRtv : rtv;
            bool lin0 = c.linear && srv[cur][1] && v0[1];
            bool scrgb = capFmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.tile[3] = (c.flatSkip > 0 && !bypass && !ctOn && !scrgb) ? (lin0 ? c.flatSkip : -c.flatSkip) : 0;
        }
        d.tile[1] = capFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 0;  // scRGB: no clamping
        ctx->UpdateSubresource(maskTex.get(), 0, nullptr, mask.data(), (UINT)tw, 0);
        lastMask = mask;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &d, sizeof d);
            ctx->Unmap(cb.get(), 0);
        }
        Com<ID3D11RenderTargetView>* views = cross ? outRtv : rtv;
        bool lin = c.linear && srv[cur][1] && views[1];
        ID3D11RenderTargetView* rt = lin ? views[1].get() : views[0].get();
        ID3D11ShaderResourceView* sv = srv[cur][lin ? 1 : 0].get();
        D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        if (ctOn) {
            // pass 1: remove ClearType colour; output is in the same space the main pass expects (linear when it
            // reads the sRGB view, sRGB-encoded otherwise), so the main pass only reads ctSrv instead of the capture
            CTData t{};
            t.gate[0] = c.ctQ0; t.gate[1] = c.ctQ1; t.gate[2] = c.ctLuma; t.gate[3] = c.ctAmount;
            t.rough[0] = c.ctRough0; t.rough[1] = c.ctRough1; t.rough[2] = 1.0f;
            t.mode[0] = t.mode[1] = lin ? 0 : 1;
            t.mode[2] = g_ctBGR.load() ? 1 : 0;
            t.size[0] = (int)w - 1; t.size[1] = (int)h - 1;
            D3D11_MAPPED_SUBRESOURCE mc;
            if (SUCCEEDED(ctx->Map(cbCt.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
                memcpy(mc.pData, &t, sizeof t);
                ctx->Unmap(cbCt.get(), 0);
            }
            ID3D11RenderTargetView* crt = ctRtv.get();
            ctx->OMSetRenderTargets(1, &crt, nullptr);
            ctx->PSSetShader(psCt.get(), nullptr, 0);
            ID3D11Buffer* b1 = cbCt.get();
            ctx->PSSetConstantBuffers(1, 1, &b1);
            ctx->PSSetShaderResources(0, 1, &sv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none1 = nullptr;
            ctx->PSSetShaderResources(0, 1, &none1);
            sv = ctSrv.get();
        }
        const bool diagOn = GetCfg().diag != 0;
        if (diagOn) DiagGpuBegin();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->PSSetShader(ps.get(), nullptr, 0);
        ID3D11Buffer* b = cb.get();
        ctx->PSSetConstantBuffers(0, 1, &b);
        ID3D11ShaderResourceView* svs[2] = {sv, maskSrv.get()};
        ctx->PSSetShaderResources(0, 2, svs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ctx->PSSetShaderResources(0, 2, nullSrv);
        if (diagOn) DiagGpuEnd();
        if (cross && !TransferToDisplay()) return;
        LARGE_INTEGER pf, p0, p1;
        QueryPerformanceFrequency(&pf);
        QueryPerformanceCounter(&p0);
        HRESULT ph = sc->Present(0, 0);
        QueryPerformanceCounter(&p1);
        if (ph != S_OK && dPresentLogs < 20) { dPresentLogs++; Log("Present returned 0x%08lx", (unsigned long)ph); }
        dLastPresentMs = (p1.QuadPart - p0.QuadPart) * 1000.0 / pf.QuadPart;
        Show();
    }

    // Cross mode: move changed regions (grown by the filter reach) to the display GPU
    bool TransferToDisplay() {
        std::vector<RECT> list;
        const LONG M = lastCtOn ? 10 : 6;  // the filter samples at most ~4 px away (+3 px for the ClearType pre-pass)
        if (accFull) {
            list.push_back(RECT{0, 0, (LONG)w, (LONG)h});
        } else {
            RECT u{(LONG)w, (LONG)h, 0, 0};
            for (RECT r : acc) {
                r.left = std::max(0L, r.left - M);
                r.top = std::max(0L, r.top - M);
                r.right = std::min((LONG)w, r.right + M);
                r.bottom = std::min((LONG)h, r.bottom + M);
                if (r.right <= r.left || r.bottom <= r.top) continue;
                list.push_back(r);
                u.left = std::min(u.left, r.left);
                u.top = std::min(u.top, r.top);
                u.right = std::max(u.right, r.right);
                u.bottom = std::max(u.bottom, r.bottom);
            }
            if (list.size() > 48) list.assign(1, u);
        }
        for (const RECT& r : list) {
            D3D11_BOX box = BoxOf(r);
            ctx->CopySubresourceRegion(outStage.get(), 0, r.left, r.top, 0, outProc.get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(outStage.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        for (const RECT& r : list) {
            D3D11_BOX box = BoxOf(r);
            cctx->UpdateSubresource(outDisp.get(), 0, &box, (const uint8_t*)m.pData + (size_t)r.top * m.RowPitch + (size_t)r.left * 4,
                                    m.RowPitch, 0);
        }
        ctx->Unmap(outStage.get(), 0);
        Com<ID3D11Texture2D> bb;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.put()))) return false;
        cctx->CopyResource(bb.get(), outDisp.get());  // with the flip model the whole buffer must be written on every present
        acc.clear();
        accFull = false;
        return true;
    }

    // Mark tiles of changed regions: a tile that changes repeatedly within a short time counts as "moving"
    void MarkTiles(LONGLONG now, LONGLONG freq, const Config& c) {
        if (!tw) return;
        LONGLONG win = freq * c.motionWindowMs / 1000, settle = freq * c.motionSettleMs / 1000;
        for (const RECT& r : rects) {
            int x0 = r.left / TILE, x1 = (r.right - 1) / TILE, y0 = r.top / TILE, y1 = (r.bottom - 1) / TILE;
            for (int ty = std::max(0, y0); ty <= std::min(th - 1, y1); ty++)
                for (int tx = std::max(0, x0); tx <= std::min(tw - 1, x1); tx++) {
                    size_t i = (size_t)ty * tw + tx;
                    if (now < tUntil[i]) { tUntil[i] = now + settle; continue; }
                    if (now - tFirst[i] > win) { tFirst[i] = now; tCount[i] = 1; }
                    else if (++tCount[i] >= c.motionFrames) tUntil[i] = now + settle;
                }
        }
    }

    // Build the mask for the current time; returns true if it changed. *count: number of moving tiles
    bool BuildMask(LONGLONG now, bool enabled, int* count) {
        int n = 0;
        for (size_t i = 0; i < mask.size(); i++) {
            mask[i] = (enabled && now < tUntil[i]) ? 255 : 0;
            n += mask[i] != 0;
        }
        *count = n;
        return mask != lastMask;
    }

    void Loop() {
        LARGE_INTEGER freq, lastRender{}, statT0, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&statT0);
        int frames = 0, dda = 0;
        bool pending = false;
        int motionTiles = 0;
        LONGLONG pendingSince = 0;  // DWM present time of the oldest desktop frame not yet shown by the overlay
        auto ms = [&](LONGLONG ticks) { return ticks * 1000.0 / (double)freq.QuadPart; };
        while (g_running) {
            if (g_restart.exchange(false)) {
                Log("Restarting");
                Teardown();
            }
            Config c = GetCfg();
            if (!c.enabled || g_fullscreenSuspend) {
                g_status = !c.enabled ? ST_DISABLED : ST_FULLSCREEN;
                Hide(!c.enabled ? "disabled" : "fullscreen application");
                dup.reset();
                pending = false;
                Sleep(100);
                continue;
            }
            if (!dev) {
                if (!Init()) {
                    Teardown();
                    g_status = ST_ERROR;
                    Sleep(2000);
                    continue;
                }
            }
            if (!dup) {
                if (!StartDuplication()) {
                    Hide("capture could not start / HDR");
                    Sleep(g_status == ST_HDR || g_status == ST_ACM ? 1000 : 250);
                    continue;
                }
                g_forceRedraw = true;
            }
            g_status = ST_ACTIVE;

            // frame cap: if a draw is pending, wake up no later than when the interval elapses
            LONGLONG interval = c.fpsCap > 0 ? freq.QuadPart / c.fpsCap : 0;
            UINT timeout = 50;
            QueryPerformanceCounter(&now);
            if ((pending || g_forceRedraw) && haveFrame && interval) {
                LONGLONG remain = interval - (now.QuadPart - lastRender.QuadPart);
                timeout = remain <= 0 ? 0 : (UINT)std::max<LONGLONG>(1, remain * 1000 / freq.QuadPart);
            } else if ((pending || g_forceRedraw) && haveFrame) {
                timeout = 0;
            }
            if (motionTiles) timeout = std::min<UINT>(timeout, 10);  // notice quickly when motion stops

            DXGI_OUTDUPL_FRAME_INFO fi{};
            Com<IDXGIResource> res;
            LARGE_INTEGER tA0, tA1;
            QueryPerformanceCounter(&tA0);
            HRESULT hr = dup->AcquireNextFrame(timeout, &fi, res.put());
            QueryPerformanceCounter(&tA1);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                if (c.diag && (pending || g_forceRedraw)) dTimeouts++;
                // a pending draw is handled below
            } else if (FAILED(hr)) {
                // ACCESS_LOST: mode change, UAC, fullscreen game, HDR toggle...
                // Never leave a stale image on screen: hide first.
                char why[64];
                snprintf(why, sizeof why, "AcquireNextFrame 0x%08lx", (unsigned long)hr);
                Hide(why);
                dup.reset();
                haveFrame = false;
                pending = false;
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_INVALID_CALL) {
                    Log("Device lost 0x%08lx — recreating", hr);
                    Teardown();
                }
                Sleep(50);
                continue;
            } else {
                dda++;
                if (c.diag) dAcq.add(ms(tA1.QuadPart - tA0.QuadPart));
                bool hasUpdate = !haveFrame || (fi.LastPresentTime.QuadPart != 0 && fi.TotalMetadataBufferSize > 0);
                if (hasUpdate) {
                    GatherRects(fi);
                    if (c.diag) {
                        double area = 0;
                        for (const RECT& r : rects) area += (double)(r.right - r.left) * (r.bottom - r.top);
                        dArea.add(area);
                        dMaxRects = std::max(dMaxRects, (int)rects.size());
                    }
                    Com<ID3D11Texture2D> t;
                    if (!rects.empty() && SUCCEEDED(res.as(t))) {
                        LARGE_INTEGER tI0, tI1;
                        QueryPerformanceCounter(&tI0);
                        bool changed = cross ? IngestCross(t.get()) : IngestSameGpu(t.get());
                        QueryPerformanceCounter(&tI1);
                        if (c.diag) {
                            double d = ms(tI1.QuadPart - tI0.QuadPart);
                            dIngest.add(d);
                            if (d > 4.0 && dSlowLogs < 60) { dSlowLogs++; Log("DIAG slow ingest %.1f ms, %zu rects", d, rects.size()); }
                        }
                        haveFrame = true;
                        if (changed) {
                            if (!pending && fi.LastPresentTime.QuadPart) pendingSince = fi.LastPresentTime.QuadPart;
                            pending = true;
                            QueryPerformanceCounter(&now);
                            MarkTiles(now.QuadPart, freq.QuadPart, c);
                            if (cross) acc.insert(acc.end(), rects.begin(), rects.end());
                        }
                    }
                }
                res.reset();
                dup->ReleaseFrame();
            }

            if (haveFrame && tw) {
                QueryPerformanceCounter(&now);
                if (BuildMask(now.QuadPart, c.motionBypass && !g_bypass, &motionTiles)) {
                    pending = true;   // a region started or stopped moving
                    accFull = true;   // move the whole screen in cross mode
                }
                g_motionTiles = motionTiles;
            }
            if ((pending || g_forceRedraw) && haveFrame) {
                QueryPerformanceCounter(&now);
                if (!interval || now.QuadPart - lastRender.QuadPart >= interval) {
                    if (g_forceRedraw) accFull = true;  // settings changed: the whole screen must be transferred again
                    g_forceRedraw = false;
                    pending = false;
                    LARGE_INTEGER tR0, tR1;
                    QueryPerformanceCounter(&tR0);
                    Render();
                    QueryPerformanceCounter(&tR1);
                    lastRender = now;
                    frames++;
                    if (c.diag) {
                        double rm = ms(tR1.QuadPart - tR0.QuadPart);
                        dRender.add(rm);
                        dPresent.add(dLastPresentMs);
                        dMaxTiles = std::max(dMaxTiles, motionTiles);
                        if (pendingSince) {
                            double lat = ms(tR1.QuadPart - pendingSince);
                            if (lat >= 0 && lat < 1000) {
                                dLat.add(lat);
                                dBuckets[lat < 4 ? 0 : lat < 8 ? 1 : lat < 16 ? 2 : 3]++;
                            }
                        }
                        if (rm > 8.0 && dSlowLogs < 60) { dSlowLogs++; Log("DIAG slow render %.1f ms (present %.1f ms), %d moving tiles", rm, dLastPresentMs, motionTiles); }
                    }
                    pendingSince = 0;
                }
            }

            QueryPerformanceCounter(&now);
            if (now.QuadPart - statT0.QuadPart >= freq.QuadPart) {
                if (c.diag) {
                    if (!dHeader) {
                        dHeader = true;
                        Log("DIAG v%s capture=%s %ux%u cross=%d fpsCap=%d motion=%d(%d frames/%d ms, settle %d ms) linear=%d kernel=%d ctCancel=%d preset=%ls",
                            APP_VERSION, FormatName(capFmt), w, h, cross ? 1 : 0, c.fpsCap, c.motionBypass, c.motionFrames,
                            c.motionWindowMs, c.motionSettleMs, c.linear, c.kernel, c.ctCancel, c.preset);
                        Log("DIAG columns: dda=desktop updates/s draw=overlay frames/s acq=AcquireNextFrame ms ingest=CPU copy ms render=Render() CPU ms "
                            "present=Present() ms gpu=draw GPU ms lat=DWM present -> overlay Present returned (avg/max ms) buckets=<4/<8/<16/>=16 ms same=updates with identical pixels (skipped)");
                    }
                    if (++dOvTick % 10 == 1) {  // overlay window state: is it visible, cloaked by DWM, on the right display?
                        DWORD cloaked = 0;
                        DwmGetWindowAttribute(g_overlay, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
                        RECT orc{};
                        GetWindowRect(g_overlay, &orc);
                        MONITORINFOEXW mi{};
                        mi.cbSize = sizeof mi;
                        DEVMODEW dm{};
                        dm.dmSize = sizeof dm;
                        unsigned long hz = 0;
                        if (GetMonitorInfoW(g_targetMon.load(), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency;
                        Log("DIAG overlay visible=%d cloaked=%lu rect=%ld,%ld,%ld,%ld monitor=%ld,%ld,%ld,%ld refresh=%luHz test=%d bypass=%d strength=%.2f kernel=%d linear=%d opaque=%d off R(%.2f,%.2f) G(%.2f,%.2f) B(%.2f,%.2f) cleartype=%d ctCancel=%d",
                            IsWindowVisible(g_overlay) ? 1 : 0, (unsigned long)cloaked, orc.left, orc.top, orc.right, orc.bottom,
                            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, hz, c.overlayTest,
                            g_bypass.load() ? 1 : 0, c.strength, c.kernel, c.linear, c.overlayAlpha == 255 ? 1 : 0,
                            c.off[0][0], c.off[0][1], c.off[1][0], c.off[1][1], c.off[2][0], c.off[2][1], c.cleartype, c.ctCancel);
                    }
                    if (dda || frames) {
                        Log("DIAG dda=%d draw=%d moveTiles<=%d rects<=%d area=%.0f/%.0f px acq=%.2f/%.2f ingest=%.2f/%.2f render=%.2f/%.2f "
                            "present=%.2f/%.2f gpu=%.2f/%.2f lat=%.2f/%.2f buckets=%d/%d/%d/%d timeouts=%d same=%d",
                            dda, frames, dMaxTiles, dMaxRects, dArea.avg(), dArea.mx, dAcq.avg(), dAcq.mx, dIngest.avg(), dIngest.mx,
                            dRender.avg(), dRender.mx, dPresent.avg(), dPresent.mx, dGpu.avg(), dGpu.mx, dLat.avg(), dLat.mx,
                            dBuckets[0], dBuckets[1], dBuckets[2], dBuckets[3], dTimeouts, dSame);
                    }
                    dAcq.reset(); dIngest.reset(); dRender.reset(); dPresent.reset(); dGpu.reset(); dLat.reset(); dArea.reset();
                    dBuckets[0] = dBuckets[1] = dBuckets[2] = dBuckets[3] = 0;
                    dMaxRects = dMaxTiles = dTimeouts = dSame = 0;
                }
                g_fps = frames;
                g_ddaFps = dda;
                frames = dda = 0;
                statT0 = now;
            }
        }
        Teardown();
    }
};

static DWORD WINAPI RenderThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Renderer r;
    r.Loop();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- fullscreen detection

static bool IsFullscreenAppOnTarget() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE))
        return true;
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_overlay || fg == g_setWnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    HMONITOR target = g_targetMon.load();
    if (!target || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != target) return false;
    LONG style = GetWindowLongW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(target, &mi)) return false;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(fg, &r);
    const RECT& m = mi.rcMonitor;
    return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// ---------------------------------------------------------------- autostart

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kAppName = L"OLEDTextFix";

static bool AutostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + std::wstring(g_exePath) + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- icon

static HICON MakeIcon() {
    const int S = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return LoadIcon(nullptr, IDI_APPLICATION);
    auto px = (DWORD*)bits;
    struct Dot { float x, y; DWORD c; } dots[3] = {{16, 9, 0xFF2BD957}, {9, 22, 0xFFFF4040}, {23, 22, 0xFF4A7BFF}};
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            DWORD v = 0;
            float bx = x - 15.5f, by = y - 15.5f;
            if (bx * bx + by * by < 15.5f * 15.5f) v = 0xFF141414;
            for (auto& d : dots) {
                float dx = x + 0.5f - d.x, dy = y + 0.5f - d.y;
                if (dx * dx + dy * dy < 30) v = d.c;
            }
            px[y * S + x] = v;
        }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic ? ic : LoadIcon(nullptr, IDI_APPLICATION);
}

// ---------------------------------------------------------------- UI shared

static const wchar_t* StatusText() {
    switch (g_status.load()) {
        case ST_ACTIVE: return L"Active";
        case ST_DISABLED: return L"Off";
        case ST_FULLSCREEN: return L"Fullscreen application — paused";
        case ST_HDR: return L"HDR is on — paused (turn on Experimental HDR in Settings)";
        case ST_ACM: return L"Auto Color Management is on — paused (turn it off in Windows display settings)";
        case ST_FORMAT: return L"Unsupported desktop colour format — paused (see log)";
        case ST_WAITING: return L"Waiting for capture";
        case ST_ERROR: return L"Error — see OLEDTextFix.log";
        default: return L"Starting";
    }
}

static void TrayAdd(bool modify = false) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g_msgWnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_icon;
    swprintf(n.szTip, 128, L"OLEDTextFix — %ls", StatusText());
    Shell_NotifyIconW(modify ? NIM_MODIFY : NIM_ADD, &n);
}
static void TrayRemove() {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g_msgWnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static void OpenSettings();
static void SyncControls();

static void Changed() { g_forceRedraw = true; }

static void ApplyPreset(int i) {
    if (i < 0 || i >= (int)g_presets.size()) return;
    const Preset& p = g_presets[i];
    EditCfg([&](Config& c) {
        memcpy(c.off, p.off, sizeof c.off);
        c.strength = p.strength;
        c.lumX = c.lumY = 0.0f;  // the luminance shift is not stored in presets
        c.kernel = p.kernel;
        c.woled = p.woled;
        wcsncpy(c.preset, p.name.c_str(), 63);
        c.preset[63] = 0;
    });
    Changed();
    SyncControls();
}

// ---------------------------------------------------------------- self-update

// Looks at the latest GitHub release; if it is newer, downloads OLEDTextFix.exe and its .sha256 from
// that release over HTTPS, checks the hash, and swaps the exe in place (a running exe can be renamed,
// so the old copy moves to OLEDTextFix.exe.old and is deleted on the next start). The new version
// takes effect after a restart (tray menu).
#ifndef APP_VERSION
#define APP_VERSION "1.0"
#endif
#define WL2_(x) L##x
#define WL_(x) WL2_(x)
static const wchar_t* kRepo = L"/oguz463/OLEDTextFix";
static const wchar_t* kReleasesPage = L"https://github.com/oguz463/OLEDTextFix/releases/latest";

enum UpdState { UPD_IDLE = 0, UPD_BUSY, UPD_UPTODATE, UPD_READY, UPD_MANUAL, UPD_FAILED };
static std::atomic<int> g_updState{UPD_IDLE};
static wchar_t g_updVer[40];

static bool HttpGet(const wchar_t* host, const std::wstring& path, std::string& out, size_t maxBytes) {
    out.clear();
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"OLEDTextFix/" WL_(APP_VERSION), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 10000, 10000, 20000, 60000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)
                        : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        if (code == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                if (avail == 0) break;
                if (out.size() + avail > maxBytes) { ok = false; break; }
                size_t at = out.size();
                out.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, &out[at], avail, &got)) { ok = false; break; }
                out.resize(at + got);
            }
        } else {
            Log("Update: HTTP %lu for %ls", code, path.c_str());
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static bool Sha256Hex(const std::string& data, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    UCHAR h[32];
    NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) return false;
    static const char* d = "0123456789abcdef";
    hex.clear();
    for (UCHAR b : h) { hex += d[b >> 4]; hex += d[b & 15]; }
    return true;
}

static DWORD WINAPI UpdateThread(LPVOID) {
    auto finish = [](int st) {
        g_updState = st;
        PostMessageW(g_msgWnd, WM_APP_UPDATE, st, 0);
    };
    std::string json;
    if (!HttpGet(L"api.github.com", std::wstring(L"/repos") + kRepo + L"/releases/latest", json, 1 << 20)) {
        Log("Update: could not reach GitHub");
        finish(UPD_FAILED);
        return 0;
    }
    char tag[40] = {};
    size_t k = json.find("\"tag_name\"");
    if (k != std::string::npos) {
        k = json.find('"', json.find(':', k) + 1);
        size_t e = k == std::string::npos ? k : json.find('"', k + 1);
        if (e != std::string::npos && e - k - 1 < sizeof(tag)) json.copy(tag, e - k - 1, k + 1);
    }
    for (char* q = tag; *q; q++)  // the tag ends up in a URL: only plain characters
        if (!(isalnum((unsigned char)*q) || *q == '.' || *q == '-' || *q == '_')) { tag[0] = 0; break; }
    if (!tag[0]) { Log("Update: no usable tag in the response"); finish(UPD_FAILED); return 0; }
    if (!IsNewerVersion(tag, APP_VERSION)) {
        Log("Update: %s is the latest (running %s)", tag, APP_VERSION);
        finish(UPD_UPTODATE);
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, g_updVer, 40);
    Log("Update: %s is available (running %s)", tag, APP_VERSION);

    std::wstring base = std::wstring(kRepo) + L"/releases/download/" + g_updVer + L"/";
    std::string exe, sum;
    if (!HttpGet(L"github.com", base + L"OLEDTextFix.exe", exe, 64u << 20) || !HttpGet(L"github.com", base + L"OLEDTextFix.exe.sha256", sum, 4096)) {
        Log("Update: release has no downloadable exe/hash; manual update needed");
        finish(UPD_MANUAL);
        return 0;
    }
    std::string hex;
    bool good = exe.size() > 100000 && exe[0] == 'M' && exe[1] == 'Z' && sum.size() >= 64 && Sha256Hex(exe, hex);
    if (good) {
        for (int i = 0; i < 64; i++)
            if (tolower((unsigned char)sum[i]) != hex[i]) { good = false; break; }
    }
    if (!good) { Log("Update: downloaded file failed the integrity check"); finish(UPD_FAILED); return 0; }

    std::wstring nw = std::wstring(g_exePath) + L".new", old = std::wstring(g_exePath) + L".old";
    HANDLE f = CreateFileW(nw.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool wrote = f != INVALID_HANDLE_VALUE && WriteFile(f, exe.data(), (DWORD)exe.size(), &wr, nullptr) && wr == exe.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!wrote) { Log("Update: cannot write next to the exe (%lu)", GetLastError()); DeleteFileW(nw.c_str()); finish(UPD_MANUAL); return 0; }
    DeleteFileW(old.c_str());
    if (!MoveFileExW(g_exePath, old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log("Update: cannot rename the running exe (%lu)", GetLastError());
        DeleteFileW(nw.c_str());
        finish(UPD_MANUAL);
        return 0;
    }
    if (!MoveFileExW(nw.c_str(), g_exePath, 0)) {
        Log("Update: cannot move the new exe into place (%lu); rolling back", GetLastError());
        MoveFileExW(old.c_str(), g_exePath, MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(nw.c_str());
        finish(UPD_FAILED);
        return 0;
    }
    Log("Update: installed %ls, restart to use it", g_updVer);
    finish(UPD_READY);
    return 0;
}

static void StartUpdateCheck() {
    int idle = UPD_IDLE;
    int cur = g_updState.load();
    if (cur == UPD_BUSY || cur == UPD_READY) return;
    (void)idle;
    g_updState = UPD_BUSY;
    HANDLE t = CreateThread(nullptr, 0, UpdateThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_updState = UPD_FAILED;
}

static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = g_msgWnd;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static wchar_t g_autoNote[256] = L"";  // shown once after the first start (auto-picked preset)
static bool g_updManual = false;  // the check was started from the menu: report "up to date" / errors too

static void OnUpdateResult(int st) {
    wchar_t b[256];
    if (st == UPD_READY) {
        swprintf(b, 256, L"%ls is installed. Right-click the tray icon and choose \"Restart to finish updating\".", g_updVer);
        TrayBalloon(L"OLEDTextFix updated", b);
    } else if (st == UPD_MANUAL) {
        swprintf(b, 256, L"%ls is available. Right-click the tray icon to open the download page.", g_updVer);
        TrayBalloon(L"OLEDTextFix update available", b);
    } else if (g_updManual && st == UPD_UPTODATE) {
        TrayBalloon(L"OLEDTextFix", L"You have the latest version (" WL_(APP_VERSION) L").");
    } else if (g_updManual && st == UPD_FAILED) {
        TrayBalloon(L"OLEDTextFix", L"Could not check for updates. See OLEDTextFix.log.");
    }
    g_updManual = false;
}

// ---------------------------------------------------------------- tray menu

enum {
    ID_ENABLE = 100, ID_SETTINGS, ID_EXIT, ID_UPD_CHECK, ID_UPD_AUTO, ID_UPD_RESTART, ID_UPD_OPEN,
    ID_PRESET0 = 300,  // ..399
    HK_TOGGLE = 1, HK_CAL = 2,
};

static void ShowTrayMenu() {
    Config c = GetCfg();
    LoadPresets();
    HMENU m = CreatePopupMenu();
    wchar_t st[160];
    swprintf(st, 160, L"Status: %ls  (v%ls)", StatusText(), WL_(APP_VERSION));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, st);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (c.enabled ? MF_CHECKED : 0), ID_ENABLE, L"Enabled\tCtrl+Alt+F");
    AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_SETTINGS, L"Settings...\tCtrl+Alt+C");
    HMENU pm = CreatePopupMenu();
    for (size_t i = 0; i < g_presets.size() && i < 100; i++) {
        if (i == std::size(kBuiltinPresets) && i < g_presets.size()) AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pm, MF_STRING | (g_presets[i].name == c.preset ? MF_CHECKED : 0), ID_PRESET0 + i, g_presets[i].name.c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pm, L"Preset");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    int us = g_updState.load();
    if (us == UPD_READY) {
        wchar_t t[96];
        swprintf(t, 96, L"Restart to finish updating to %ls", g_updVer);
        AppendMenuW(m, MF_STRING, ID_UPD_RESTART, t);
    } else if (us == UPD_MANUAL) {
        wchar_t t[96];
        swprintf(t, 96, L"%ls is available — open download page", g_updVer);
        AppendMenuW(m, MF_STRING, ID_UPD_OPEN, t);
    } else {
        AppendMenuW(m, MF_STRING | (us == UPD_BUSY ? MF_GRAYED : 0), ID_UPD_CHECK, us == UPD_BUSY ? L"Checking for updates..." : L"Check for updates now");
    }
    AppendMenuW(m, MF_STRING | (c.updateCheck ? MF_CHECKED : 0), ID_UPD_AUTO, L"Check for updates automatically");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_msgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_msgWnd, nullptr);
    PostMessageW(g_msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void HandleTrayCommand(UINT id) {
    if (id == ID_ENABLE) {
        EditCfg([](Config& c) { c.enabled = !c.enabled; });
        SaveConfig();
        SyncControls();
    } else if (id == ID_SETTINGS) {
        OpenSettings();
    } else if (id == ID_UPD_CHECK) {
        g_updManual = true;
        StartUpdateCheck();
    } else if (id == ID_UPD_AUTO) {
        EditCfg([](Config& c) { c.updateCheck = !c.updateCheck; });
        SaveConfig();
    } else if (id == ID_UPD_OPEN) {
        ShellExecuteW(nullptr, L"open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == ID_UPD_RESTART) {
        SaveConfig();
        ShellExecuteW(nullptr, L"open", g_exePath, L"/restarted", g_exeDir, SW_SHOWNORMAL);
        DestroyWindow(g_msgWnd);
    } else if (id == ID_EXIT) {
        SaveConfig();
        DestroyWindow(g_msgWnd);
    } else if (id >= ID_PRESET0 && id < ID_PRESET0 + 100) {
        ApplyPreset((int)(id - ID_PRESET0));
        SaveConfig();
    }
}

// ---------------------------------------------------------------- settings window

enum {
    IDC_ENABLED = 1001, IDC_COMPARE, IDC_LINEAR, IDC_CLEARTYPE, IDC_SUSPEND, IDC_AUTOSTART,
    IDC_MONITOR, IDC_PRESET, IDC_DELETE, IDC_NAME, IDC_SAVEAS, IDC_KERNEL,
    IDC_SAVE, IDC_DEFAULTS, IDC_CLOSE, IDC_GPU, IDC_FPSCAP, IDC_OPAQUE, IDC_MOTION, IDC_HDR, IDC_CTCANCEL, IDC_SLIDER0 = 1100,
};

static const int kSliders = 9;  // 0..5: RX RY GX GY BX BY, 6: strength, 7..8: luminance shift X Y
static const wchar_t* kSliderNames[kSliders] = {L"Red  X", L"Red  Y", L"Green  X", L"Green  Y", L"Blue  X", L"Blue  Y", L"Strength", L"Luma X (WOLED)", L"Luma Y (WOLED)"};

struct SettingsUI {
    HWND wnd = nullptr, pattern = nullptr;
    HWND hdrGeneral, chkEnabled, chkCompare, chkLinear, chkCT, lblCT, chkCtCancel, lblCtCancel, chkSuspend, chkAutostart, lblMon, cmbMon;
    HWND hdrPreset, cmbPreset, btnDelete, edtName, btnSaveAs;
    HWND hdrSub, lblSl[kSliders], sl[kSliders], val[kSliders], lblKernel, cmbKernel;
    HWND hdrPerf, lblGpu, cmbGpu, lblFps, cmbFps, chkOpaque, chkMotion, chkHdr;
    HWND btnSave, btnDefaults, btnClose, lblStatus;
    std::vector<GpuInfo> gpus;
    HFONT font = nullptr, fontBold = nullptr;
    UINT dpi = 96;
    bool syncing = false;
    std::vector<OutInfo> outs;
};
static SettingsUI g_ui;
static const int kFpsCaps[] = {0, 240, 165, 144, 120, 60};
static const wchar_t* kFpsCapNames[] = {L"Unlimited (monitor refresh rate)", L"240 fps", L"165 fps", L"144 fps", L"120 fps", L"60 fps"};
static const int kFpsCapCount = 6;

static int S(int v) { return MulDiv(v, (int)g_ui.dpi, 96); }

static void UpdateSliderLabel(int i) {
    if (!g_ui.wnd) return;
    int pos = (int)SendMessageW(g_ui.sl[i], TBM_GETPOS, 0, 0);
    wchar_t b[32];
    if (i < 6 || i >= 7) swprintf(b, 32, L"%+.2f px", pos / 100.0);
    else swprintf(b, 32, L"%.2f", pos / 100.0);
    SetWindowTextW(g_ui.val[i], b);
}

static void UpdateClearTypeLabel() {
    if (!g_ui.wnd) return;
    bool bgr = false;
    bool on = DetectClearType(&bgr);
    bool mode = GetCfg().cleartype != 0;
    wchar_t b[200];
    swprintf(b, 200, L"Windows ClearType: %ls%ls", on ? (bgr ? L"on (BGR)" : L"on (RGB)") : L"off / grayscale",
             on != mode ? L"  ⚠ does not match this checkbox" : L"  ✓ matches");
    SetWindowTextW(g_ui.lblCT, b);

    // ClearType colour removal: only meaningful on WOLED presets with ClearType text
    Config c = GetCfg();
    const wchar_t* st = !c.ctCancel   ? L"Off (experimental, WOLED presets only)"
                        : !c.woled    ? L"On, but inactive: the selected preset is not a WOLED layout"
                        : !on         ? L"On, but Windows ClearType is off: nothing to remove"
                                      : L"Active: ClearType colour on grey text is removed before the correction";
    SetWindowTextW(g_ui.lblCtCancel, st);
}

static void FillPresetCombo() {
    LoadPresets();
    Config c = GetCfg();
    SendMessageW(g_ui.cmbPreset, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_ui.cmbPreset, CB_ADDSTRING, 0, (LPARAM)L"— Custom (unsaved) —");
    int sel = 0;
    for (size_t i = 0; i < g_presets.size(); i++) {
        std::wstring t = g_presets[i].name + (g_presets[i].builtin ? L"   [built-in]" : L"");
        SendMessageW(g_ui.cmbPreset, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        if (g_presets[i].name == c.preset) sel = (int)i + 1;
    }
    SendMessageW(g_ui.cmbPreset, CB_SETCURSEL, sel, 0);
    EnableWindow(g_ui.btnDelete, sel > 0 && !g_presets[sel - 1].builtin);
}

static void SyncControls() {
    if (!g_ui.wnd) return;
    g_ui.syncing = true;
    Config c = GetCfg();
    auto chk = [](HWND h, bool v) { SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); };
    chk(g_ui.chkEnabled, c.enabled);
    chk(g_ui.chkCompare, g_bypass);
    chk(g_ui.chkLinear, c.linear);
    chk(g_ui.chkCT, c.cleartype);
    chk(g_ui.chkSuspend, c.autoSuspend);
    chk(g_ui.chkAutostart, AutostartEnabled());
    for (int i = 0; i < 6; i++)
        SendMessageW(g_ui.sl[i], TBM_SETPOS, TRUE, (LPARAM)lroundf(c.off[i / 2][i % 2] * 100));
    SendMessageW(g_ui.sl[6], TBM_SETPOS, TRUE, (LPARAM)lroundf(c.strength * 100));
    SendMessageW(g_ui.sl[7], TBM_SETPOS, TRUE, (LPARAM)lroundf(c.lumX * 100));
    SendMessageW(g_ui.sl[8], TBM_SETPOS, TRUE, (LPARAM)lroundf(c.lumY * 100));
    for (int i = 0; i < kSliders; i++) UpdateSliderLabel(i);
    for (int i = 7; i < 9; i++) {  // the luminance shift only applies to WOLED layouts
        EnableWindow(g_ui.sl[i], c.woled);
        EnableWindow(g_ui.lblSl[i], c.woled);
        EnableWindow(g_ui.val[i], c.woled);
    }
    for (int i = 0; i < kKernelListN; i++)
        if (kKernelList[i] == c.kernel) SendMessageW(g_ui.cmbKernel, CB_SETCURSEL, i, 0);
    SendMessageW(g_ui.cmbMon, CB_SETCURSEL, c.monitor < 0 ? 0 : c.monitor + 1, 0);
    int gsel = 0;
    for (size_t i = 0; i < g_ui.gpus.size(); i++)
        if (c.gpu[0] && g_ui.gpus[i].name == c.gpu) gsel = (int)i + 1;
    SendMessageW(g_ui.cmbGpu, CB_SETCURSEL, gsel, 0);
    int fsel = 0;
    for (int i = 0; i < kFpsCapCount; i++)
        if (kFpsCaps[i] == c.fpsCap) fsel = i;
    SendMessageW(g_ui.cmbFps, CB_SETCURSEL, fsel, 0);
    chk(g_ui.chkMotion, c.motionBypass);
    chk(g_ui.chkOpaque, c.overlayAlpha >= 255);
    chk(g_ui.chkHdr, c.hdr);
    chk(g_ui.chkCtCancel, c.ctCancel);
    FillPresetCombo();
    UpdateClearTypeLabel();
    g_ui.syncing = false;
}

static void MarkCustom() {
    EditCfg([](Config& c) { c.preset[0] = 0; });
    SendMessageW(g_ui.cmbPreset, CB_SETCURSEL, 0, 0);
    EnableWindow(g_ui.btnDelete, FALSE);
}

static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_ui.wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
}

static void Place(HWND h, int x, int y, int w, int hgt) { SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE); }

static void ApplyFonts() {
    if (g_ui.font) DeleteObject(g_ui.font);
    if (g_ui.fontBold) DeleteObject(g_ui.fontBold);
    g_ui.font = CreateFontW(-MulDiv(9, (int)g_ui.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_ui.fontBold = CreateFontW(-MulDiv(10, (int)g_ui.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    EnumChildWindows(
        g_ui.wnd,
        [](HWND h, LPARAM) -> BOOL {
            bool hdr = h == g_ui.hdrGeneral || h == g_ui.hdrPreset || h == g_ui.hdrSub || h == g_ui.hdrPerf;
            SendMessageW(h, WM_SETFONT, (WPARAM)(hdr ? g_ui.fontBold : g_ui.font), TRUE);
            return TRUE;
        },
        0);
}

static const int kLeftW = 460;
static const int kLabelW = 112;  // width of the label column (fits "Processing GPU")

static void Layout() {
    RECT rc;
    GetClientRect(g_ui.wnd, &rc);
    int x = S(14), w = S(kLeftW), y = S(10), rh = S(24);
    Place(g_ui.hdrGeneral, x, y, w, S(20)); y += S(24);
    Place(g_ui.chkEnabled, x, y, w, rh); y += rh;
    Place(g_ui.chkCompare, x, y, w, rh); y += rh;
    Place(g_ui.chkLinear, x, y, w, rh); y += rh;
    Place(g_ui.chkCT, x, y, w, rh); y += rh;
    Place(g_ui.lblCT, x + S(20), y, w - S(20), S(18)); y += S(22);
    Place(g_ui.chkCtCancel, x, y, w, rh); y += rh;
    Place(g_ui.lblCtCancel, x + S(20), y, w - S(20), S(18)); y += S(22);
    Place(g_ui.chkSuspend, x, y, w, rh); y += rh;
    Place(g_ui.chkAutostart, x, y, w, rh); y += rh + S(6);
    Place(g_ui.lblMon, x, y + S(4), S(kLabelW), S(20));
    Place(g_ui.cmbMon, x + S(kLabelW + 4), y, w - S(kLabelW + 4), S(300)); y += S(36);

    Place(g_ui.hdrPreset, x, y, w, S(20)); y += S(24);
    Place(g_ui.cmbPreset, x, y, w - S(78), S(400));
    Place(g_ui.btnDelete, x + w - S(72), y - S(1), S(72), S(26)); y += S(32);
    Place(g_ui.edtName, x, y, w - S(178), S(24));
    Place(g_ui.btnSaveAs, x + w - S(172), y - S(1), S(172), S(26)); y += S(38);

    Place(g_ui.hdrSub, x, y, w, S(20)); y += S(24);
    for (int i = 0; i < kSliders; i++) {
        if (i == 6) y += S(6);
        Place(g_ui.lblSl[i], x, y + S(5), S(kLabelW), S(20));
        Place(g_ui.sl[i], x + S(kLabelW + 2), y, w - S(kLabelW + 2 + 66), S(28));
        Place(g_ui.val[i], x + w - S(62), y + S(5), S(62), S(20));
        y += S(30);
    }
    Place(g_ui.lblKernel, x, y + S(4), S(kLabelW), S(20));
    Place(g_ui.cmbKernel, x + S(kLabelW + 4), y, w - S(kLabelW + 4), S(200)); y += S(40);

    Place(g_ui.hdrPerf, x, y, w, S(20)); y += S(24);
    Place(g_ui.lblGpu, x, y + S(4), S(kLabelW), S(20));
    Place(g_ui.cmbGpu, x + S(kLabelW + 4), y, w - S(kLabelW + 4), S(200)); y += S(32);
    Place(g_ui.lblFps, x, y + S(4), S(kLabelW), S(20));
    Place(g_ui.cmbFps, x + S(kLabelW + 4), y, w - S(kLabelW + 4), S(200)); y += S(32);
    // let long entries (monitor/GPU names) show in full when a list is open
    for (HWND cb : {g_ui.cmbMon, g_ui.cmbGpu, g_ui.cmbPreset})
        SendMessageW(cb, CB_SETDROPPEDWIDTH, S(520), 0);
    Place(g_ui.chkMotion, x, y, w, rh); y += rh;
    Place(g_ui.chkOpaque, x, y, w, rh); y += rh;
    Place(g_ui.chkHdr, x, y, w, rh); y += rh + S(10);

    Place(g_ui.btnSave, x, y, S(110), S(28));
    Place(g_ui.btnDefaults, x + S(118), y, S(150), S(28));
    Place(g_ui.btnClose, x + w - S(100), y, S(100), S(28)); y += S(36);
    Place(g_ui.lblStatus, x, y, w, std::max(S(54), (int)rc.bottom - y - S(4)));

    int px = x + w + S(14);
    Place(g_ui.pattern, px, 0, std::max(10, (int)rc.right - px), rc.bottom);
}

static const int kClientW = 14 + kLeftW + 14 + 720, kClientH = 1035;

static void ResizeForDpi() {
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, g_ui.dpi);
    SetWindowPos(g_ui.wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CreateControls() {
    const DWORD CB = BS_AUTOCHECKBOX | WS_TABSTOP;
    g_ui.hdrGeneral = Mk(L"STATIC", L"General", 0, 0);
    g_ui.chkEnabled = Mk(L"BUTTON", L"Effect enabled   (Ctrl+Alt+F)", CB, IDC_ENABLED);
    g_ui.chkCompare = Mk(L"BUTTON", L"Compare: temporarily turn the effect off (before/after)", CB, IDC_COMPARE);
    g_ui.chkLinear = Mk(L"BUTTON", L"Process in linear light (recommended)", CB, IDC_LINEAR);
    g_ui.chkCT = Mk(L"BUTTON", L"ClearType-compatible mode (check if ClearType is on)", CB, IDC_CLEARTYPE);
    g_ui.lblCT = Mk(L"STATIC", L"", 0, 0);
    g_ui.chkCtCancel = Mk(L"BUTTON", L"Remove ClearType colour fringes (WOLED, experimental)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CTCANCEL);
    g_ui.lblCtCancel = Mk(L"STATIC", L"", 0, 0);
    g_ui.chkSuspend = Mk(L"BUTTON", L"Pause for fullscreen games/video", CB, IDC_SUSPEND);
    g_ui.chkAutostart = Mk(L"BUTTON", L"Start with Windows", CB, IDC_AUTOSTART);
    g_ui.lblMon = Mk(L"STATIC", L"Monitor:", 0, 0);
    g_ui.cmbMon = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_MONITOR);

    g_ui.hdrPreset = Mk(L"STATIC", L"Preset", 0, 0);
    g_ui.cmbPreset = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_PRESET);
    g_ui.btnDelete = Mk(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, IDC_DELETE);
    g_ui.edtName = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NAME, WS_EX_CLIENTEDGE);
    SendMessageW(g_ui.edtName, EM_SETLIMITTEXT, 60, 0);
    SendMessageW(g_ui.edtName, 0x1501 /*EM_SETCUEBANNER*/, TRUE, (LPARAM)L"New preset name");
    g_ui.btnSaveAs = Mk(L"BUTTON", L"Save as preset", BS_PUSHBUTTON | WS_TABSTOP, IDC_SAVEAS);

    g_ui.hdrSub = Mk(L"STATIC", L"Subpixel positions and filter", 0, 0);
    for (int i = 0; i < kSliders; i++) {
        g_ui.lblSl[i] = Mk(L"STATIC", kSliderNames[i], 0, 0);
        g_ui.sl[i] = Mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP, IDC_SLIDER0 + i);
        if (i != 6) SendMessageW(g_ui.sl[i], TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));
        else SendMessageW(g_ui.sl[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(g_ui.sl[i], TBM_SETLINESIZE, 0, 1);
        SendMessageW(g_ui.sl[i], TBM_SETPAGESIZE, 0, 5);
        g_ui.val[i] = Mk(L"STATIC", L"", SS_RIGHT, 0);
    }
    g_ui.lblKernel = Mk(L"STATIC", L"Filter", 0, 0);
    g_ui.cmbKernel = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_KERNEL);
    for (int i = 0; i < kKernelListN; i++) SendMessageW(g_ui.cmbKernel, CB_ADDSTRING, 0, (LPARAM)KernelName(kKernelList[i]));

    g_ui.hdrPerf = Mk(L"STATIC", L"Performance / power testing", 0, 0);
    g_ui.lblGpu = Mk(L"STATIC", L"Processing GPU", 0, 0);
    g_ui.cmbGpu = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_GPU);
    g_ui.gpus = EnumGpus();
    SendMessageW(g_ui.cmbGpu, CB_ADDSTRING, 0, (LPARAM)L"Automatic — monitor's GPU (recommended)");
    for (auto& gi : g_ui.gpus) {
        std::wstring t = gi.name + L"   [experimental]";
        SendMessageW(g_ui.cmbGpu, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    g_ui.lblFps = Mk(L"STATIC", L"Frame cap", 0, 0);
    g_ui.cmbFps = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_FPSCAP);
    for (int i = 0; i < kFpsCapCount; i++) SendMessageW(g_ui.cmbFps, CB_ADDSTRING, 0, (LPARAM)kFpsCapNames[i]);
    g_ui.chkMotion = Mk(L"BUTTON", L"Show moving regions without latency (recommended)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_MOTION);
    g_ui.chkOpaque = Mk(L"BUTTON", L"Fully opaque overlay (sharper, recommended)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_OPAQUE);
    g_ui.chkHdr = Mk(L"BUTTON", L"Experimental: process HDR desktops (restarts capture)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_HDR);

    g_ui.btnSave = Mk(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, IDC_SAVE);
    g_ui.btnDefaults = Mk(L"BUTTON", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, IDC_DEFAULTS);
    g_ui.btnClose = Mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE);
    g_ui.lblStatus = Mk(L"STATIC", L"", 0, 0);

    g_ui.pattern = CreateWindowExW(0, L"OLEDTextFixPattern", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_ui.wnd, nullptr, g_inst, nullptr);

    g_ui.outs = EnumOutputs();
    SendMessageW(g_ui.cmbMon, CB_ADDSTRING, 0, (LPARAM)L"Automatic (primary display)");
    for (auto& o : g_ui.outs) SendMessageW(g_ui.cmbMon, CB_ADDSTRING, 0, (LPARAM)o.name.c_str());
}

static void OnSaveAs() {
    wchar_t name[64] = {};
    GetWindowTextW(g_ui.edtName, name, 64);
    std::wstring n = name;
    while (!n.empty() && iswspace(n.back())) n.pop_back();
    while (!n.empty() && iswspace(n.front())) n.erase(n.begin());
    if (n.empty() || n.find_first_of(L"[]=") != std::wstring::npos) {
        MessageBoxW(g_ui.wnd, L"Enter a valid preset name (the characters [ ] = are not allowed).", L"OLEDTextFix", MB_ICONINFORMATION);
        SetFocus(g_ui.edtName);
        return;
    }
    LoadPresets();
    int idx = FindPreset(n.c_str());
    if (idx >= 0 && g_presets[idx].builtin) {
        MessageBoxW(g_ui.wnd, L"This name belongs to a built-in preset. Choose another name.", L"OLEDTextFix", MB_ICONINFORMATION);
        return;
    }
    if (idx >= 0 && MessageBoxW(g_ui.wnd, L"A preset with this name already exists. Overwrite it?", L"OLEDTextFix", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    Config c = GetCfg();
    WriteUserPreset(n, c);
    EditCfg([&](Config& g) {
        wcsncpy(g.preset, n.c_str(), 63);
        g.preset[63] = 0;
    });
    SaveConfig();
    SetWindowTextW(g_ui.edtName, L"");
    FillPresetCombo();
}

static void OnDelete() {
    int sel = (int)SendMessageW(g_ui.cmbPreset, CB_GETCURSEL, 0, 0);
    if (sel <= 0 || sel - 1 >= (int)g_presets.size() || g_presets[sel - 1].builtin) return;
    std::wstring name = g_presets[sel - 1].name;
    std::wstring q = L"Delete preset \"" + name + L"\"?";
    if (MessageBoxW(g_ui.wnd, q.c_str(), L"OLEDTextFix", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    DeleteUserPreset(name);
    EditCfg([](Config& c) { c.preset[0] = 0; });
    SaveConfig();
    FillPresetCombo();
}

static void UpdateStatusLabel() {
    if (!g_ui.wnd) return;
    wchar_t b[512];
    if (g_cross)
        swprintf(b, 512, L"Status: %ls\nProcessed %d · captured %d fps\nCapture GPU: %ls\nProcessing GPU: %ls (cross-adapter)", StatusText(),
                 g_fps.load(), g_ddaFps.load(), g_capGpuName, g_gpuName);
    else
        swprintf(b, 512, L"Status: %ls\nProcessed %d · captured %d fps · moving %d tiles\nGPU: %ls", StatusText(),
                 g_fps.load(), g_ddaFps.load(), g_motionTiles.load(), g_gpuName);
    SetWindowTextW(g_ui.lblStatus, b);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_ui.wnd = hwnd;
            g_ui.dpi = GetDpiForWindow(hwnd);
            CreateControls();
            ApplyFonts();
            ResizeForDpi();
            Layout();
            SyncControls();
            UpdateStatusLabel();
            SetTimer(hwnd, 1, 500, nullptr);
            return 0;
        case WM_SIZE:
            if (g_ui.pattern) Layout();
            return 0;
        case WM_DPICHANGED: {
            g_ui.dpi = HIWORD(wp);
            RECT* r = (RECT*)lp;
            ApplyFonts();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = S(kLeftW + 300);
            mmi->ptMinTrackSize.y = S(kClientH) / 2;
            return 0;
        }
        case WM_TIMER:
            UpdateStatusLabel();
            UpdateClearTypeLabel();
            return 0;
        case WM_HSCROLL: {
            if (g_ui.syncing) return 0;
            for (int i = 0; i < kSliders; i++) {
                if ((HWND)lp != g_ui.sl[i]) continue;
                float v = (int)SendMessageW(g_ui.sl[i], TBM_GETPOS, 0, 0) / 100.0f;
                EditCfg([&](Config& c) {
                    if (i < 6) c.off[i / 2][i % 2] = v;
                    else if (i == 6) c.strength = v;
                    else if (i == 7) c.lumX = v;
                    else c.lumY = v;
                });
                UpdateSliderLabel(i);
                MarkCustom();
                Changed();
            }
            return 0;
        }
        case WM_COMMAND: {
            if (g_ui.syncing) return 0;
            int id = LOWORD(wp), code = HIWORD(wp);
            auto checked = [&](HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; };
            switch (id) {
                case IDC_ENABLED: EditCfg([&](Config& c) { c.enabled = checked(g_ui.chkEnabled); }); break;
                case IDC_COMPARE: g_bypass = checked(g_ui.chkCompare); Changed(); break;
                case IDC_LINEAR: EditCfg([&](Config& c) { c.linear = checked(g_ui.chkLinear); }); Changed(); break;
                case IDC_CLEARTYPE:
                    EditCfg([&](Config& c) { c.cleartype = checked(g_ui.chkCT); });
                    UpdateClearTypeLabel();
                    Changed();
                    break;
                case IDC_SUSPEND: EditCfg([&](Config& c) { c.autoSuspend = checked(g_ui.chkSuspend); }); break;
                case IDC_AUTOSTART: SetAutostart(checked(g_ui.chkAutostart)); break;
                case IDC_MONITOR:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_ui.cmbMon, CB_GETCURSEL, 0, 0);
                        EditCfg([&](Config& c) { c.monitor = sel - 1; });
                        SaveConfig();
                        g_restart = true;
                    }
                    break;
                case IDC_PRESET:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_ui.cmbPreset, CB_GETCURSEL, 0, 0);
                        if (sel > 0) ApplyPreset(sel - 1);
                        else MarkCustom();
                    }
                    break;
                case IDC_KERNEL:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_ui.cmbKernel, CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < kKernelListN) EditCfg([&](Config& c) { c.kernel = kKernelList[sel]; });
                        MarkCustom();
                        Changed();
                    }
                    break;
                case IDC_GPU:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_ui.cmbGpu, CB_GETCURSEL, 0, 0);
                        EditCfg([&](Config& c) {
                            c.gpu[0] = 0;
                            if (sel > 0 && sel - 1 < (int)g_ui.gpus.size()) {
                                wcsncpy(c.gpu, g_ui.gpus[sel - 1].name.c_str(), 127);
                                c.gpu[127] = 0;
                            }
                        });
                        SaveConfig();
                        g_restart = true;
                    }
                    break;
                case IDC_FPSCAP:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_ui.cmbFps, CB_GETCURSEL, 0, 0);
                        EditCfg([&](Config& c) { c.fpsCap = kFpsCaps[std::max(0, std::min(kFpsCapCount - 1, sel))]; });
                    }
                    break;
                case IDC_MOTION: EditCfg([&](Config& c) { c.motionBypass = checked(g_ui.chkMotion); }); Changed(); break;
                case IDC_HDR: EditCfg([&](Config& c) { c.hdr = checked(g_ui.chkHdr); }); g_restart = true; break;
                case IDC_CTCANCEL:
                    EditCfg([&](Config& c) { c.ctCancel = checked(g_ui.chkCtCancel); });
                    UpdateClearTypeLabel();
                    Changed();
                    break;
                case IDC_OPAQUE: {
                    int a = checked(g_ui.chkOpaque) ? 255 : 254;
                    EditCfg([&](Config& c) { c.overlayAlpha = a; });
                    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)a, LWA_ALPHA);
                    break;
                }
                case IDC_SAVEAS: OnSaveAs(); break;
                case IDC_DELETE: OnDelete(); break;
                case IDC_SAVE: SaveConfig(); MessageBeep(MB_OK); break;
                case IDC_DEFAULTS:
                    EditCfg([](Config& c) {
                        Config d;
                        c.linear = d.linear;
                        c.overlayAlpha = d.overlayAlpha;
                        c.motionBypass = d.motionBypass;
                        c.fpsCap = d.fpsCap;
                    });
                    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)GetCfg().overlayAlpha, LWA_ALPHA);
                    ApplyPreset(std::max(0, FindPreset(Config().preset)));
                    break;
                case IDC_CLOSE:
                case IDCANCEL: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            g_bypass = false;
            g_forceRedraw = true;
            SaveConfig();
            if (g_ui.font) DeleteObject(g_ui.font);
            if (g_ui.fontBold) DeleteObject(g_ui.fontBold);
            g_ui = SettingsUI{};
            g_setWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenSettings() {
    if (g_ui.wnd) {
        ShowWindow(g_ui.wnd, SW_RESTORE);
        SetForegroundWindow(g_ui.wnd);
        return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = g_targetMon.load();
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    // open the window on the target monitor; size is set from the DPI in WM_CREATE
    int x = mi.rcWork.left + 40, y = mi.rcWork.top + 40;
    g_setWnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"OLEDTextFixSettings", L"OLEDTextFix — Settings", WS_OVERLAPPEDWINDOW, x, y, 800, 600,
                               nullptr, nullptr, g_inst, nullptr);
    if (!g_setWnd) return;
    RECT wr;
    GetWindowRect(g_setWnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(g_setWnd, nullptr, std::max(mi.rcWork.left, (mi.rcWork.left + mi.rcWork.right - w) / 2),
                 std::max(mi.rcWork.top, (mi.rcWork.top + mi.rcWork.bottom - h) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_setWnd, SW_SHOW);
    SetForegroundWindow(g_setWnd);
}

// ---------------------------------------------------------------- test pattern

static void DrawPattern(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    SetBkMode(mdc, TRANSPARENT);
    int dpi = (int)GetDpiForWindow(hwnd);
    auto P = [&](int v) { return MulDiv(v, dpi, 96); };
    HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0)), wh = CreateSolidBrush(RGB(255, 255, 255)), cap = CreateSolidBrush(RGB(70, 70, 70));

    const wchar_t* sample = L"Hamburgefonstiv quick brown 0123";
    const wchar_t* code = L"if (x == 0) { return f(y); }  // code sample";
    for (int half = 0; half < 2; half++) {
        bool dark = half == 0;
        RECT area{0, half * H / 2, W, (half + 1) * H / 2};
        FillRect(mdc, &area, dark ? bk : wh);
        COLORREF fg = dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        COLORREF dim = dark ? RGB(150, 150, 150) : RGB(110, 110, 110);
        int y = area.top + P(10), x = P(14);
        auto text = [&](const wchar_t* face, int px, int quality, COLORREF col, const wchar_t* s, int xx, int yy) {
            HFONT f = CreateFontW(-P(px), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, quality, 0, face);
            HGDIOBJ of = SelectObject(mdc, f);
            SetTextColor(mdc, col);
            // clip to this half of the pane so long lines never spill into the other column
            RECT clip{xx, yy, (xx >= W / 2 ? W : W / 2) - P(4), yy + P(px) * 2};
            ExtTextOutW(mdc, xx, yy, ETO_CLIPPED, &clip, s, (UINT)wcslen(s), nullptr);
            SelectObject(mdc, of);
            DeleteObject(f);
        };
        text(L"Segoe UI", 11, ANTIALIASED_QUALITY, dim, dark ? L"DARK BACKGROUND — grayscale" : L"LIGHT BACKGROUND — grayscale", x, y);
        text(L"Segoe UI", 11, ANTIALIASED_QUALITY, dim, L"ClearType", W / 2 + x, y);
        y += P(18);
        const int sizes[] = {12, 14, 16, 20};
        for (int s : sizes) {
            text(L"Segoe UI", s, ANTIALIASED_QUALITY, fg, sample, x, y);
            text(L"Segoe UI", s, CLEARTYPE_QUALITY, fg, sample, W / 2 + x, y);
            y += P(s + 8);
        }
        // shrink the code line until it fits in half the pane
        int codePx = 14;
        for (; codePx > 8; codePx--) {
            HFONT f = CreateFontW(-P(codePx), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
            HGDIOBJ of = SelectObject(mdc, f);
            SIZE sz{};
            GetTextExtentPoint32W(mdc, code, (int)wcslen(code), &sz);
            SelectObject(mdc, of);
            DeleteObject(f);
            if (sz.cx <= W / 2 - 2 * x) break;
        }
        text(L"Consolas", codePx, ANTIALIASED_QUALITY, fg, code, x, y);
        text(L"Consolas", codePx, CLEARTYPE_QUALITY, fg, code, W / 2 + x, y);
        y += P(26);
        // thin horizontal lines (green-top / magenta-bottom fringe test) and vertical lines
        HBRUSH line = dark ? wh : bk;
        for (int i = 0; i < 4; i++) {
            RECT l{x, y + i * P(5), W / 2 - x, y + i * P(5) + 1};
            FillRect(mdc, &l, line);
        }
        int sq = std::max(8, std::min(P(36), (int)(area.bottom - y - P(8))));
        RECT s1{W / 2 + x, y, W / 2 + x + sq, y + sq};
        FillRect(mdc, &s1, line);
        for (int i = 0; i < 8; i++) {
            RECT v{W / 2 + x + sq + P(14) + i * P(5), y, W / 2 + x + sq + P(14) + i * P(5) + 1, y + sq};
            FillRect(mdc, &v, line);
        }
    }
    RECT sep{W / 2 - 1, 0, W / 2, H};
    FillRect(mdc, &sep, cap);
    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    DeleteObject(bk);
    DeleteObject(wh);
    DeleteObject(cap);
}

static LRESULT CALLBACK PatternProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawPattern(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- overlay + message windows

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_TASKBARCREATED != 0 && msg == WM_TASKBARCREATED) {
        TrayAdd();
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
            else if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) OpenSettings();
            return 0;
        case WM_COMMAND: HandleTrayCommand(LOWORD(wp)); return 0;
        case WM_APP_UPDATE: OnUpdateResult((int)wp); return 0;
        case WM_HOTKEY:
            if (wp == HK_TOGGLE) {
                EditCfg([](Config& c) { c.enabled = !c.enabled; });
                SaveConfig();
                SyncControls();
            } else if (wp == HK_CAL) {
                OpenSettings();
            }
            return 0;
        case WM_APP_SETRECT: {
            HMONITOR mon = g_targetMon.load();
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                g_monRect = mi.rcMonitor;
                // One pixel row short of the monitor: a topmost window that covers the whole monitor is
                // treated by the shell as a fullscreen app, which pushes the taskbar behind other windows.
                SetWindowPos(g_overlay, HWND_TOPMOST, g_monRect.left, g_monRect.top, g_monRect.right - g_monRect.left,
                             g_monRect.bottom - g_monRect.top - 1, SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wp == 3) {  // one-time note about the auto-picked preset
                KillTimer(g_msgWnd, 3);
                if (g_autoNote[0]) TrayBalloon(L"OLEDTextFix preset", g_autoNote);
                g_autoNote[0] = 0;
                return 0;
            }
            if (wp == 2) {  // update check: 20 s after start, then every 24 h
                SetTimer(g_msgWnd, 2, 24 * 60 * 60 * 1000, nullptr);
                if (GetCfg().updateCheck) StartUpdateCheck();
                return 0;
            }
            Config c = GetCfg();
            // fullscreen detection: require 2 consecutive readings to avoid toggling (flicker)
            bool fs = c.autoSuspend && IsFullscreenAppOnTarget();
            static int streak = 0;
            if (fs != g_fullscreenSuspend.load()) {
                if (++streak >= 2) {
                    g_fullscreenSuspend = fs;
                    streak = 0;
                    wchar_t cls[64] = {};
                    if (HWND fg = GetForegroundWindow()) GetClassNameW(fg, cls, 64);
                    Log("Fullscreen pause: %s (foreground class: %ls)", fs ? "ON" : "off", cls);
                }
            } else {
                streak = 0;
            }
            static int tick = 0;
            if (++tick % 8 == 0) {
                TrayAdd(true);
                bool wasBgr = g_ctBGR.load();
                DetectClearType();  // keep the BGR/RGB orientation up to date
                if (wasBgr != g_ctBGR.load()) Changed();
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            g_restart = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// First run: choose the preset matching the target monitor's resolution, refresh rate and size.
// All presets of one layout family share the same values; this mostly picks the right name.
static void AutoPickPreset() {
    auto outs = EnumOutputs();
    int idx = PickOutput(outs, GetCfg().monitor);
    if (idx < 0) return;
    const OutInfo& o = outs[idx];
    int w = o.rc.right - o.rc.left, h = o.rc.bottom - o.rc.top;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    int hz = EnumDisplaySettingsW(o.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
    int widthMm = 0;
    if (HDC dc = CreateDCW(L"DISPLAY", o.device.c_str(), nullptr, nullptr)) {
        widthMm = GetDeviceCaps(dc, HORZSIZE);
        DeleteDC(dc);
    }
    // EDID manufacturer code from the monitor's device ID ("MONITOR\\GSM5BBF\\..."). LG Electronics (GSM)
    // only uses LG Display WOLED panels in its OLED monitors; other brands are told apart by size and refresh.
    wchar_t mfg[4] = L"";
    {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(o.device.c_str(), 0, &dd, 0)) {
            const wchar_t* id = wcschr(dd.DeviceID, L'\\');
            if (id && wcslen(id) > 3) wcsncpy(mfg, id + 1, 3), mfg[3] = 0;
        }
    }
    const bool lg = wcscmp(mfg, L"GSM") == 0;
    const wchar_t* name = nullptr;
    if (w == 2560 && h == 1440)
        name = (hz >= 470 && hz <= 490) ? L"27\" 1440p 480 Hz WOLED — RGWB (2024-2025)"  // QD-OLED is 360 or 500 Hz
             : lg                       ? L"27\" 1440p 240 Hz WOLED — RWBG (2023-2024)"
                                        : L"27\" 1440p QD-OLED";
    else if (w == 3840 && h == 2160)
        name = widthMm > 650 ? (lg ? L"32\" 4K 240 Hz WOLED — RGWB (2024-2025)" : L"32\" 4K QD-OLED")
             : lg ? L"RGB stripe (Tandem RGB WOLED / QD-OLED V-Stripe) — no correction"  // LG 27" 4K OLED: RGB stripe
                  : L"27\" 4K QD-OLED";
    else if (w == 3440 && h == 1440)
        name = widthMm > 950 ? L"45\" 3440x1440 240 Hz WOLED — RWBG (2023-2024)"
             : widthMm > 850 ? L"39\" 3440x1440 240 Hz WOLED — RWBG (2024)"  // QD-OLED ultrawides are 34"
             : lg            ? L"34\" 3440x1440 240 Hz WOLED — RWBG (2024)"
                             : L"34\" 3440x1440 QD-OLED";
    else if (w == 5120 && h == 2160)
        name = widthMm > 980 ? L"45\" 5120x2160 165 Hz WOLED — RGWB (2025)" : L"39\" 5120x2160 Tandem WOLED — RGWB (2026)";
    else if (w == 5120 && h == 1440)
        name = L"49\" 5120x1440 QD-OLED";
    Log("Monitor: %ls, %dx%d, %d Hz, %d mm wide -> preset: %ls", mfg, w, h, hz, widthMm, name ? name : L"(default)");
    if (name) {
        ApplyPreset(FindPreset(name));
        if (!lg)  // WOLED and QD-OLED can't be told apart by size and refresh rate on other brands
            swprintf(g_autoNote, 256, L"Auto-selected preset: %ls. If your monitor is WOLED (it has a white subpixel), pick a WOLED preset in Settings.", name);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = inst;
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\OLEDTextFix.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && cmdLine && wcsstr(cmdLine, L"/restarted")) {
        // started by "Restart to finish updating": wait for the old instance to exit, then take over
        DWORD w = WaitForSingleObject(mtx, 10000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // if already running, ask it to open the settings window
        if (HWND w = FindWindowW(L"OLEDTextFixMsg", nullptr)) PostMessageW(w, WM_HOTKEY, HK_CAL, 0);
        return 0;
    }

    // Per-monitor DPI awareness (the manifest sets it too; this is a fallback)
    if (auto user32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI * PFN)(HANDLE);
        if (auto fn = (PFN)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext")) fn((HANDLE)-4);
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(nullptr, g_exePath, MAX_PATH);
    wcscpy(g_exeDir, g_exePath);
    if (wchar_t* s = wcsrchr(g_exeDir, L'\\')) *s = 0;
    swprintf(g_iniPath, MAX_PATH, L"%ls\\OLEDTextFix.ini", g_exeDir);
    swprintf(g_logPath, MAX_PATH, L"%ls\\OLEDTextFix.log", g_exeDir);
    {
        FILE* f = _wfopen(g_logPath, L"w");
        if (f) fclose(f);
    }
    Log("OLEDTextFix started (v%s)", APP_VERSION);
    {
        std::wstring leftovers[] = {std::wstring(g_exePath) + L".old", std::wstring(g_exePath) + L".new"};
        for (auto& l : leftovers) DeleteFileW(l.c_str());  // from an earlier update
    }
    // Migrate from the old name (QDTextFix): settings file and "Start with Windows" entry
    {
        // If the old QDTextFix is still running, ask it to exit (it saves its settings first)
        if (HWND old = FindWindowW(L"QDTextFixMsg", nullptr)) {
            PostMessageW(old, WM_COMMAND, ID_EXIT, 0);
            for (int i = 0; i < 30 && IsWindow(old); i++) Sleep(100);
            Log("Closed the running QDTextFix instance");
        }
        wchar_t oldIni[MAX_PATH];
        swprintf(oldIni, MAX_PATH, L"%ls\\QDTextFix.ini", g_exeDir);
        if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(oldIni) != INVALID_FILE_ATTRIBUTES &&
            CopyFileW(oldIni, g_iniPath, TRUE))
            Log("Imported settings from QDTextFix.ini");
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ | KEY_WRITE, &k) == ERROR_SUCCESS) {
            bool hadOld = RegQueryValueExW(k, L"QDTextFix", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
            if (hadOld) RegDeleteValueW(k, L"QDTextFix");
            RegCloseKey(k);
            if (hadOld) {
                SetAutostart(true);
                Log("Moved the Start with Windows entry from QDTextFix");
            }
        }
    }
    bool firstRun = GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES;
    LoadConfig();
    LoadPresets();
    if (firstRun) AutoPickPreset();
    if (firstRun) SaveConfig();

    g_icon = MakeIcon();
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hIcon = g_icon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    wc.lpfnWndProc = MsgProc;
    wc.lpszClassName = L"OLEDTextFixMsg";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = L"OLEDTextFixOverlay";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = PatternProc;
    wc.lpszClassName = L"OLEDTextFixPattern";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"OLEDTextFixSettings";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(0, L"OLEDTextFixMsg", L"OLEDTextFix", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    // Overlay window: topmost, click-through, hidden from the taskbar and Alt+Tab
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                    WS_EX_NOREDIRECTIONBITMAP,
                                L"OLEDTextFixOverlay", L"OLEDTextFix Overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)GetCfg().overlayAlpha, LWA_ALPHA);

    // Critical: the overlay itself must not be captured, otherwise a feedback loop occurs.
    if (!SetWindowDisplayAffinity(g_overlay, 0x00000011 /*WDA_EXCLUDEFROMCAPTURE*/)) {
        Log("WDA_EXCLUDEFROMCAPTURE failed (error %lu)", GetLastError());
        MessageBoxW(nullptr,
                    L"This application requires Windows 10 version 2004 or later "
                    L"(excluding a window from screen capture).",
                    L"OLEDTextFix", MB_ICONERROR);
        return 1;
    }

    TrayAdd();
    RegisterHotKey(g_msgWnd, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(g_msgWnd, HK_CAL, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    SetTimer(g_msgWnd, 1, 250, nullptr);
    SetTimer(g_msgWnd, 2, 20000, nullptr);
    if (g_autoNote[0]) SetTimer(g_msgWnd, 3, 8000, nullptr);

    HANDLE th = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);

    if (firstRun) OpenSettings();  // show the settings on first run

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_ui.wnd && IsDialogMessageW(g_ui.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    WaitForSingleObject(th, 3000);
    CloseHandle(th);
    TrayRemove();
    if (g_overlay) DestroyWindow(g_overlay);
    Log("Exit");
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return 0;
}
