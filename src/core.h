// Pure logic shared by OLEDTextFix (src/main.cpp) and the native tests (tests/test_core.cpp): no Windows headers.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

// Filters (all bicubic). Values are stored in the INI file: 0 and 1 were Bilinear and Mitchell in earlier builds.
enum Kernel { K_OLD_BILINEAR = 0, K_OLD_MITCHELL = 1, K_CATMULL = 2, K_BALANCED = 3, K_SMOOTH = 4, K_COUNT };

// Presets for LG-style white-subpixel panels. The RGB-stripe entry mentions WOLED in its name but needs no correction.
static inline bool NameIsWoled(const wchar_t* n) { return wcsstr(n, L"WOLED") && wcsncmp(n, L"RGB stripe", 10) != 0; }

static inline void KernelWeights(float t, int kind, float w[4]) {
    // taps: floor(position) + (-1, 0, 1, 2). Mitchell-Netravali bicubic with parameters (B, C):
    // B adds smoothing (less fringing, softer), C sharpens.
    float B = 0.0f, C = 0.5f;                       // Sharp: Catmull-Rom
    if (kind == K_BALANCED) { B = 0.2f; C = 0.6f; }
    else if (kind == K_SMOOTH) { B = 0.3f; C = 0.5f; }
    auto k = [&](float x) {
        x = fabsf(x);
        if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
        if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
        return 0.0f;
    };
    w[0] = k(t + 1); w[1] = k(t); w[2] = k(1 - t); w[3] = k(2 - t);
    float s = w[0] + w[1] + w[2] + w[3];
    for (int i = 0; i < 4; i++) w[i] /= s;
}

static inline void ChannelTaps(float d, int kind, int& base, float w[4]) {
    if (d < -2) d = -2;
    if (d > 2) d = 2;
    float fl = floorf(d);
    base = (int)fl - 1;
    KernelWeights(d - fl, kind, w);
}


static inline bool ParseVersion(const char* s, std::vector<int>& v) {
    v.clear();
    if (*s == 'v' || *s == 'V') s++;
    if (!isdigit((unsigned char)*s)) return false;
    while (*s) {
        if (!isdigit((unsigned char)*s)) break;
        int n = 0;
        while (isdigit((unsigned char)*s) && n < 100000) n = n * 10 + (*s++ - '0');
        v.push_back(n);
        if (*s != '.') break;
        s++;
    }
    return !v.empty();
}
static inline bool IsNewerVersion(const char* tag, const char* cur) {
    std::vector<int> a, b;
    if (!ParseVersion(tag, a) || !ParseVersion(cur, b)) return false;
    size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
}


// CPU reference of the ClearType colour-removal gate (PSCancel in kShader). Same maths, used by the tests.
// c[0..6] are linear RGB pixels of a horizontal 7-px window centred on c[3]; returns the filtered centre pixel.
struct CtParams { float q0 = 0.50f, q1 = 0.80f, luma = 0.25f, r0 = 1.00f, r1 = 1.30f, amount = 1.0f; bool bgr = false; };
static inline float CtSmoothstep(float a, float b, float x) {
    float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline float CtLinToSrgb(float l) {
    l = std::max(l, 0.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
}
// returns the gate value g in [0,1] (1 = fully ClearType text) and writes the filtered pixel to out
static inline float CtGate(const float c[7][3], const CtParams& P, float out[3]) {
    static const float LW[3] = {0.2126f, 0.7152f, 0.0722f};
    float Y[7], Cm[3] = {0, 0, 0}, A = 0, Lmin = 1e9f, Lmax = -1e9f;
    for (int i = 0; i < 7; i++) {
        Y[i] = LW[0] * c[i][0] + LW[1] * c[i][1] + LW[2] * c[i][2];
        float d0 = c[i][0] - Y[i], d1 = c[i][1] - Y[i], d2 = c[i][2] - Y[i];
        Cm[0] += d0; Cm[1] += d1; Cm[2] += d2;
        A += sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
        float L = powf(std::min(std::max(Y[i], 0.0f), 1.0f), 1.0f / 2.2f);
        Lmin = std::min(Lmin, L);
        Lmax = std::max(Lmax, L);
    }
    for (float& v : Cm) v /= 7.0f;
    A /= 7.0f;
    float g = 1.0f - CtSmoothstep(P.q0, P.q1, sqrtf(Cm[0] * Cm[0] + Cm[1] * Cm[1] + Cm[2] * Cm[2]) / (A + 1e-4f));
    g *= CtSmoothstep(P.luma, 2.0f * P.luma, Lmax - Lmin);
    if (g > 0.0f) {
        float e[5][3];
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 3; k++)
                e[i][k] = CtLinToSrgb(std::min(std::max(c[i + 1][k], 0.0f), 1.0f));
        if (P.bgr)
            for (int i = 0; i < 5; i++) std::swap(e[i][0], e[i][2]);
        float sg[11] = {e[0][2], e[1][0], e[1][1], e[1][2], e[2][0], e[2][1], e[2][2], e[3][0], e[3][1], e[3][2], e[4][0]};
        float D1 = 0, D2 = 0;
        for (int k = 1; k <= 9; k++) {
            D1 += fabsf(sg[k] - sg[k - 1]);
            D2 += fabsf(sg[k + 1] - 2.0f * sg[k] + sg[k - 1]);
        }
        g *= 1.0f - CtSmoothstep(P.r0, P.r1, D2 / (D1 + 3e-3f));
    }
    g *= P.amount;
    for (int k = 0; k < 3; k++) out[k] = c[3][k] + (Y[3] - c[3][k]) * g;
    return g;
}
