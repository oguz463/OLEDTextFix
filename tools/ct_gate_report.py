#!/usr/bin/env python3
"""Reports what the experimental "Remove ClearType colour fringes" pass would do to a screenshot.

Take lossless screenshots with Win+PrintScreen (native resolution, 100% zoom, HDR off), then run:
    python3 tools/ct_gate_report.py shot.png [--bgr] [--label text|ui|photo|gray] [--heatmap out.png]
It is a NumPy port of PSCancel in src/main.cpp (same maths as CtGate in src/core.h), so the numbers show how the
gate behaves on real ClearType rendering (DirectWrite, GDI, Chrome/Skia) before trusting its default thresholds.

Expected for the shipped thresholds: ClearType text >= 70% of coloured text pixels cleaned; grayscale text,
coloured UI and photos <= 0.1% of pixels changed (anything else needs a look at the heatmap).
"""
import argparse
import sys
import numpy as np
from PIL import Image

LW = np.array([0.2126, 0.7152, 0.0722])


def srgb_to_lin(e):
    return np.where(e <= 0.04045, e / 12.92, ((e + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(l):
    l = np.maximum(l, 0)
    return np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1 / 2.4) - 0.055)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def gate(lin, bgr=False, q0=0.50, q1=0.80, luma=0.25, r0=1.00, r1=1.30, amount=1.0):
    """lin: HxWx3 linear RGB. Returns (filtered, g)."""
    H, W, _ = lin.shape
    pad = np.pad(lin, ((0, 0), (3, 3), (0, 0)), mode='edge')
    win = np.stack([pad[:, i:i + W] for i in range(7)], axis=0)       # 7,H,W,3
    Y = win @ LW
    C = win - Y[..., None]
    Cm = C.mean(axis=0)
    A = np.linalg.norm(C, axis=-1).mean(axis=0)
    L = np.power(np.clip(Y, 0, 1), 1 / 2.2)
    g = 1 - smoothstep(q0, q1, np.linalg.norm(Cm, axis=-1) / (A + 1e-4))
    g *= smoothstep(luma, 2 * luma, L.max(axis=0) - L.min(axis=0))
    idx = np.nonzero(g > 0)
    if idx[0].size:
        e = lin_to_srgb(np.clip(win[1:6][:, idx[0], idx[1], :], 0, 1))   # 5,N,3
        if bgr:
            e = e[..., ::-1]
        s = np.stack([e[0, :, 2], e[1, :, 0], e[1, :, 1], e[1, :, 2], e[2, :, 0], e[2, :, 1], e[2, :, 2],
                      e[3, :, 0], e[3, :, 1], e[3, :, 2], e[4, :, 0]], axis=0)
        D1 = np.abs(s[1:10] - s[0:9]).sum(axis=0)
        D2 = np.abs(s[2:11] - 2 * s[1:10] + s[0:9]).sum(axis=0)
        g[idx] *= 1 - smoothstep(r0, r1, D2 / (D1 + 3e-3))
    g *= amount
    out = win[3] + (Y[3][..., None] - win[3]) * g[..., None]
    return out, g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('png')
    ap.add_argument('--bgr', action='store_true', help='Windows ClearType orientation is BGR')
    ap.add_argument('--label', default='', help='text | ui | photo | gray (what the screenshot shows)')
    ap.add_argument('--heatmap', help='write a PNG with changed pixels in red over the dimmed screenshot')
    a = ap.parse_args()
    img = np.asarray(Image.open(a.png).convert('RGB'), dtype=np.float64) / 255.0
    lin = srgb_to_lin(img)
    out, g = gate(lin, bgr=a.bgr)
    changed = np.abs(lin_to_srgb(out) - img).max(axis=-1) * 255 > 4
    Y = lin @ LW
    chroma = np.linalg.norm(lin - Y[..., None], axis=-1)
    coloured = chroma > 0.02
    n = img.shape[0] * img.shape[1]
    print(f'{a.png}: {img.shape[1]}x{img.shape[0]}  label={a.label or "-"}  orientation={"BGR" if a.bgr else "RGB"}')
    print(f'  pixels with colour (|C| > 0.02 linear): {100 * coloured.mean():.2f}%')
    if coloured.any():
        print(f'  of those, gate > 0.5 (treated as ClearType): {100 * (g[coloured] > 0.5).mean():.1f}%')
    print(f'  pixels changed by more than 4/255: {100 * changed.mean():.3f}%  (max change {255 * np.abs(lin_to_srgb(out) - img).max():.0f}/255)')
    if coloured.any():
        rough = np.percentile(g[coloured], [10, 50, 90])
        print(f'  gate percentiles over coloured pixels p10/p50/p90: {rough[0]:.2f} / {rough[1]:.2f} / {rough[2]:.2f}')
    if a.heatmap:
        dim = (img * 0.35 * 255).astype(np.uint8)
        dim[changed] = [255, 0, 0]
        Image.fromarray(dim).save(a.heatmap)
        print(f'  heatmap: {a.heatmap}')
    limit = {'gray': 0.05, 'ui': 0.1, 'photo': 0.1}.get(a.label)
    if limit is not None and 100 * changed.mean() > limit:
        print(f'  WARNING: more than {limit}% changed for a "{a.label}" screenshot')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
