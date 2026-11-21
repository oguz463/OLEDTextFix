"""Compares correction variants on a QD-OLED triad when ClearType (RGB) is on."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import convolve1d
from sim import correct, perceive, metrics, srgb_to_lin

W, H, UP = 220, 60, 12  # UP: horizontal/vertical supersampling (multiple of 3)


def coverage():
    img = Image.new("L", (W * UP, H * UP), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype("DejaVuSans.ttf", 13 * UP)
    d.text((2 * UP, 2 * UP), "Hamburgefonstiv 0123", fill=255, font=f)
    d.text((2 * UP, 30 * UP), "The quick brown fox", fill=255, font=f)
    return np.asarray(img, np.float64) / 255.0


def grayscale_aa(cov):
    a = cov.reshape(H, UP, W, UP).mean((1, 3))
    return np.stack([a, a, a], -1)


def cleartype_rgb(cov):
    # vertical: per-pixel box; horizontal: subpixel (1/3 px) resolution + 5-tap FIR
    v = cov.reshape(H, UP, W * UP).mean(1)  # H x (W*UP)
    sub = v.reshape(H, W * 3, UP // 3).mean(2)  # H x 3W subpixel coverage
    sub = convolve1d(sub, np.array([1, 2, 3, 2, 1]) / 9.0, axis=1, mode="constant")
    r, g, b = sub[:, 0::3], sub[:, 1::3], sub[:, 2::3]
    return np.stack([r, g, b], -1)


if __name__ == "__main__":
    a, bx = 0.20, 0.20
    emit = [(-bx, +a), (0, -a), (+bx, +a)]
    centre = [(0, 0)] * 3
    cov = coverage()
    ideal_src = srgb_to_lin(grayscale_aa(cov))
    ideal = perceive(ideal_src, centre)  # target: colourless, perfectly aligned

    def run(label, src, offs):
        p = perceive(src if offs is None else correct(src, offs, "mitchell"), emit)
        ce, le = metrics(p, ideal)
        print(f"{label:55s} colour_err={ce:.4f}  luma_err={le:.4f}")

    gs = srgb_to_lin(grayscale_aa(cov))
    ct = srgb_to_lin(cleartype_rgb(cov))
    ct_assumed = [(-1 / 3, 0), (0, 0), (1 / 3, 0)]
    ct_comp = [(e[0] - c[0], e[1] - c[1]) for e, c in zip(emit, ct_assumed)]
    vert_only = [(0, e[1]) for e in emit]
    print("Dark background, light text")
    run("Grayscale, no correction", gs, None)
    run("Grayscale + full correction", gs, emit)
    run("ClearType RGB, no correction", ct, None)
    run("ClearType RGB + full correction (wrong: horizontal counted twice)", ct, emit)
    run("ClearType RGB + vertical-only correction", ct, vert_only)
    run("ClearType RGB + ClearType-compensated correction", ct, ct_comp)
