// Native tests for the pure logic in src/core.h. Build and run: sh tests/run.sh
#include "../src/core.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0, g_n = 0;
#define CHECK(cond) do { g_n++; if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool Near(float a, float b, float e = 1e-5f) { return fabsf(a - b) <= e; }

static void TestKernels() {
    for (int kind : {K_CATMULL, K_BALANCED, K_SMOOTH}) {
        for (float t = 0; t < 1.0f; t += 0.0625f) {
            float w[4];
            KernelWeights(t, kind, w);
            CHECK(Near(w[0] + w[1] + w[2] + w[3], 1.0f));
        }
    }
    float w[4];
    KernelWeights(0.0f, K_CATMULL, w);  // exactly on a sample: identity
    CHECK(Near(w[0], 0) && Near(w[1], 1) && Near(w[2], 0) && Near(w[3], 0));
    KernelWeights(0.5f, K_CATMULL, w);  // Catmull-Rom half-way: (-1, 9, 9, -1) / 16
    CHECK(Near(w[0], -1 / 16.0f) && Near(w[1], 9 / 16.0f) && Near(w[2], 9 / 16.0f) && Near(w[3], -1 / 16.0f));
    KernelWeights(0.5f, K_SMOOTH, w);  // symmetric about the half-way point
    CHECK(Near(w[0], w[3]) && Near(w[1], w[2]));
    int base;
    ChannelTaps(0.0f, K_CATMULL, base, w);
    CHECK(base == -1 && Near(w[1], 1));
    ChannelTaps(-1.5f, K_CATMULL, base, w);  // floor(-1.5) = -2, taps start at -3
    CHECK(base == -3);
    ChannelTaps(5.0f, K_CATMULL, base, w);   // clamped to +2
    CHECK(base == 1 && Near(w[1], 1));
    ChannelTaps(-9.0f, K_CATMULL, base, w);  // clamped to -2
    CHECK(base == -3 && Near(w[1], 1));
}

static void TestVersions() {
    CHECK(IsNewerVersion("v1.2.0", "1.1.9"));
    CHECK(IsNewerVersion("1.10", "1.9"));
    CHECK(!IsNewerVersion("v1.2", "1.2.0"));      // equal after padding
    CHECK(!IsNewerVersion("1.1.0", "1.2.0"));
    CHECK(!IsNewerVersion("garbage", "1.0.0"));
    CHECK(!IsNewerVersion("1.0.1", "garbage"));
    CHECK(IsNewerVersion("1.0.1", "1.0"));
    std::vector<int> v;
    CHECK(ParseVersion("V2.3.4-beta", v) && v.size() == 3 && v[0] == 2 && v[2] == 4);
    CHECK(!ParseVersion("", v));
}

static void TestWoledNames() {
    CHECK(NameIsWoled(L"27\" 1440p 480 Hz WOLED — RGWB (2024-2025)"));
    CHECK(NameIsWoled(L"Any WOLED — RWBG"));
    CHECK(!NameIsWoled(L"RGB stripe (Tandem RGB WOLED / QD-OLED V-Stripe) — no correction"));
    CHECK(!NameIsWoled(L"27\" 1440p QD-OLED"));
    CHECK(!NameIsWoled(L"My preset"));
}

// ---- ClearType colour-removal gate

static float Lum(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float Chroma(const float* c) {
    float y = Lum(c), a = c[0] - y, b = c[1] - y, d = c[2] - y;
    return sqrtf(a * a + b * b + d * d);
}

// Grey text in RGB ClearType: subpixel coverage low-passed with the FIR [1 2 3 2 1]/9 (the classic ClearType filter),
// then read as R,G,B of consecutive subpixels. A white 2-px stem on black, optionally with a coloured foreground.
static void MakeText(float px[24][3], float x0sub, float widthSub, const float fg[3], bool bgr) {
    float cov[3 * 24 + 8] = {};
    for (int k = 0; k < 3 * 24; k++) cov[k + 4] = (k >= x0sub && k < x0sub + widthSub) ? 1.0f : 0.0f;
    static const float f[5] = {1, 2, 3, 2, 1};
    for (int x = 0; x < 24; x++)
        for (int ch = 0; ch < 3; ch++) {
            int k = 3 * x + ch;
            float s = 0;
            for (int j = 0; j < 5; j++) s += f[j] * cov[k + 4 - 2 + j];
            s /= 9.0f;
            int outCh = bgr ? 2 - ch : ch;
            px[x][outCh] = s * fg[outCh];
        }
}

static void TestGate() {
    const float white[3] = {1, 1, 1};
    CtParams P;
    float px[24][3], win[7][3], o[3];

    // 1. Pure grey input is returned unchanged for every window position.
    float grey[24][3];
    for (int x = 0; x < 24; x++) grey[x][0] = grey[x][1] = grey[x][2] = (x % 5) * 0.2f;
    for (int x = 3; x < 21; x++) {
        for (int i = 0; i < 7; i++) memcpy(win[i], grey[x + i - 3], sizeof win[i]);
        CtGate(win, P, o);
        CHECK(Near(o[0], grey[x][0], 1e-4f) && Near(o[1], grey[x][1], 1e-4f) && Near(o[2], grey[x][2], 1e-4f));
    }

    // 2. ClearType text: the gate fires on edge pixels and chroma shrinks; luminance of every pixel is preserved.
    for (int bgr = 0; bgr < 2; bgr++) {
        MakeText(px, 30.0f, 6.0f, white, bgr != 0);
        P.bgr = bgr != 0;
        float maxG = 0, chromaBefore = 0, chromaAfter = 0;
        for (int x = 3; x < 21; x++) {
            for (int i = 0; i < 7; i++) memcpy(win[i], px[x + i - 3], sizeof win[i]);
            float g = CtGate(win, P, o);
            maxG = std::max(maxG, g);
            chromaBefore += Chroma(px[x]);
            chromaAfter += Chroma(o);
            CHECK(Near(Lum(o), Lum(px[x]), 1e-4f));
            for (int k = 0; k < 3; k++) CHECK(o[k] >= -1e-4f && o[k] <= 1.0001f);
        }
        printf("  ClearType %s: max gate %.2f, chroma sum %.3f -> %.3f\n", bgr ? "BGR" : "RGB", maxG, chromaBefore, chromaAfter);
        CHECK(maxG > 0.5f);
        CHECK(chromaAfter < 0.5f * chromaBefore);
    }

    // 3. Wrong orientation: ClearType BGR text read as RGB must not be 'cleaned' better than the right orientation.
    {
        MakeText(px, 30.0f, 6.0f, white, true);
        float afterRight = 0, afterWrong = 0;
        for (int wrong = 0; wrong < 2; wrong++) {
            P.bgr = wrong == 0;
            for (int x = 3; x < 21; x++) {
                for (int i = 0; i < 7; i++) memcpy(win[i], px[x + i - 3], sizeof win[i]);
                CtGate(win, P, o);
                (wrong ? afterWrong : afterRight) += Chroma(o);
            }
        }
        CHECK(afterRight <= afterWrong + 1e-4f);
    }

    // 4. Saturated coloured UI (1-px red/blue checkerboard, coloured 1-px lines) is left untouched.
    {
        float ui[24][3];
        for (int x = 0; x < 24; x++) {
            bool a = x % 2 == 0;
            ui[x][0] = a ? 0.9f : 0.0f; ui[x][1] = 0.05f; ui[x][2] = a ? 0.0f : 0.9f;
        }
        P.bgr = false;
        for (int x = 3; x < 21; x++) {
            for (int i = 0; i < 7; i++) memcpy(win[i], ui[x + i - 3], sizeof win[i]);
            float g = CtGate(win, P, o);
            CHECK(g < 0.05f);
            CHECK(Near(o[0], ui[x][0], 0.05f) && Near(o[2], ui[x][2], 0.05f));
        }
    }

    // 5. A flat saturated colour field (no luma contrast) is untouched.
    {
        float c[7][3];
        for (auto& p : c) { p[0] = 0.8f; p[1] = 0.2f; p[2] = 0.1f; }
        float g = CtGate(c, P, o);
        CHECK(g == 0.0f && Near(o[0], 0.8f) && Near(o[1], 0.2f) && Near(o[2], 0.1f));
    }

    // 6. amount = 0 disables the effect.
    {
        MakeText(px, 30.0f, 6.0f, white, false);
        P.amount = 0;
        P.bgr = false;
        for (int x = 3; x < 21; x++) {
            for (int i = 0; i < 7; i++) memcpy(win[i], px[x + i - 3], sizeof win[i]);
            CtGate(win, P, o);
            CHECK(Near(o[0], px[x][0]) && Near(o[1], px[x][1]) && Near(o[2], px[x][2]));
        }
    }
}

int main() {
    TestKernels();
    TestVersions();
    TestWoledNames();
    TestGate();
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
