"""Simulate QD-OLED triangle subpixel fringing and per-channel resampling correction.

Panel model: pixel pitch 1. Emitter centres relative to pixel centre:
  G: (0, -a)   R: (-b, +a)   B: (+b, +a)
Perceived image = emitters splatted on a fine grid, blurred by an eye PSF.
Ideal = same, but all three emitters at the pixel centre (no geometric fringe).
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import gaussian_filter, convolve1d

SS = 8  # fine grid samples per pixel


def text_image(dark_mode=True):
    W, H = 220, 60
    img = Image.new("L", (W * 4, H * 4), 0)
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("DejaVuSans.ttf", 13 * 4)
    except OSError:
        f = ImageFont.load_default()
    d.text((8, 8), "Hamburgefonstiv 0123", fill=255, font=f)
    d.text((8, 120), "The quick brown fox", fill=255, font=f)
    img = img.resize((W, H), Image.LANCZOS)  # grayscale AA text
    a = np.asarray(img, np.float64) / 255.0
    if not dark_mode:
        a = 1 - a
    return np.stack([a, a, a], -1)  # linear-ish values


def srgb_to_lin(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


# Bicubic (Mitchell-Netravali B, C) family. The first three are the app's filters; the rest are
# kept for comparison in sim_filters.py.
BC = {
    "catmull": (0.0, 0.5),          # app: Sharp (Catmull-Rom)
    "balanced": (0.2, 0.6),         # app: Balanced
    "smooth": (0.3, 0.5),           # app: Smooth
    "sharp": (0.0, 0.75),           # sharp bicubic (Keys a = -0.75)
    "robidouxsharp": (0.2620, 0.3690),
    "robidoux": (0.3782, 0.3109),
    "mitchell": (1 / 3, 1 / 3),
    "hermite": (0.0, 0.0),
    "bspline": (1.0, 0.0),          # cubic B-spline (softest)
}


def kernel_weights(frac, kind):
    """weights for taps at offsets -1,0,1,2 relative to floor(sample position)"""
    t = frac
    if kind == "bilinear":
        return np.array([0, 1 - t, t, 0])
    if kind == "lanczos2":
        def k(x):
            x = abs(x)
            return 1.0 if x < 1e-6 else (np.sinc(x) * np.sinc(x / 2) if x < 2 else 0.0)
    else:
        B, C = BC[kind]

        def k(x):
            x = abs(x)
            if x < 1:
                return ((12 - 9 * B - 6 * C) * x**3 + (-18 + 12 * B + 6 * C) * x**2 + (6 - 2 * B)) / 6
            if x < 2:
                return ((-B - 6 * C) * x**3 + (6 * B + 30 * C) * x**2 + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6
            return 0.0

    w = np.array([k(t + 1), k(t), k(1 - t), k(2 - t)])
    return w / w.sum()


def shift_channel(ch, dx, dy, kind):
    """out(p) = in(p + (dx,dy)), separable 4-tap"""
    out = ch
    for axis, d in ((1, dx), (0, dy)):
        fl = np.floor(d)
        fr = d - fl
        w = kernel_weights(fr, kind)
        # taps at p + fl + (-1,0,1,2)
        res = np.zeros_like(out)
        for i, off in enumerate((-1, 0, 1, 2)):
            res += w[i] * np.roll(out, -int(fl + off), axis=axis)
        out = res
    return out


def correct(img, offs, kind, strength=1.0):
    out = np.empty_like(img)
    for c in range(3):
        dx, dy = offs[c]
        out[..., c] = shift_channel(img[..., c], dx * strength, dy * strength, kind)
    return np.clip(out, 0, 1)


def perceive(img, emit_offs, sigma_px=0.55):
    H, W, _ = img.shape
    fine = np.zeros((H * SS, W * SS, 3))
    for c in range(3):
        dx, dy = emit_offs[c]
        ox = int(round((0.5 + dx) * SS))
        oy = int(round((0.5 + dy) * SS))
        fine[oy::SS, ox::SS, c][: H, : W] = img[..., c]
    for c in range(3):
        fine[..., c] = gaussian_filter(fine[..., c], sigma_px * SS) * SS * SS
    return fine


def metrics(p, ideal):
    lum = lambda x: 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
    chroma = lambda x: x - lum(x)[..., None]
    ce = np.sqrt(np.mean((chroma(p) - chroma(ideal)) ** 2))
    le = np.sqrt(np.mean((lum(p) - lum(ideal)) ** 2))
    return ce, le


if __name__ == "__main__":
    a, b = 0.22, 0.22  # emitter offsets (fraction of pixel)
    emit = [(-b, +a), (0, -a), (+b, +a)]  # R, G, B
    centre = [(0, 0)] * 3
    for dark in (True, False):
        src = srgb_to_lin(text_image(dark))
        ideal = perceive(src, centre)
        base = perceive(src, emit)
        ce0, le0 = metrics(base, ideal)
        print(f"\n{'dark' if dark else 'light'} mode  uncorrected: chroma_err={ce0:.4f} luma_err={le0:.4f}")
        for kind in ("bilinear", "mitchell", "catmull"):
            for s in (0.5, 0.75, 1.0, 1.25):
                cor = correct(src, emit, kind, s)
                ce, le = metrics(perceive(cor, emit), ideal)
                print(f"  {kind:8s} s={s:4.2f}: chroma_err={ce:.4f} ({100*(1-ce/ce0):5.1f}% less) luma_err={le:.4f}")
        # sanity: wrong direction should be worse
        wrong = correct(src, [(-x, -y) for x, y in emit], "catmull", 1.0)
        ce, le = metrics(perceive(wrong, emit), ideal)
        print(f"  WRONG SIGN catmull: chroma_err={ce:.4f}")
