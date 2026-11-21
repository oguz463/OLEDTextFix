"""Simulates the effect of per-channel shift correction on LG WOLED (RWBG / RGWB) panels.

Panel model: each pixel is 4 vertical columns (width 1/4). The panel derives white from the RGB
input itself: W = beta * min(R,G,B); the colour subpixels emit the remainder (c - W).
beta is the white subpixel's share of luminance (~0.5-0.9 on real panels, varies with brightness).

The correction shifts R/G/B relative to a reference point. Compared here: the pixel centre and the
white column. The "ideal" image has every subpixel at that reference point (a uniform shift of the whole
image is harmless), so colour error = fringing and luma error = loss of sharpness.
Result: with the white column as reference and strength ~0.20 both errors drop for every beta;
the pixel-centre reference trades fringing for blur, and strengths above ~0.25 overshoot on bright
content. This is what the WOLED presets use.
"""
import numpy as np
from scipy.ndimage import gaussian_filter
from sim import correct, metrics, srgb_to_lin, text_image

SS = 8
# Column centres relative to the white column, measured from RTINGS macro photos (see README), then
# re-centred on the mean of the four columns (pixel centre).
_MEAS = {
    "RWBG": {"R": -0.32, "W": 0.0, "B": 0.20, "G": 0.47},
    "RGWB": {"R": -0.50, "G": -0.25, "W": 0.0, "B": 0.22},
}
COL = {k: {c: v - sum(m.values()) / 4 for c, v in m.items()} for k, m in _MEAS.items()}


def perceive_woled(img, layout, beta, sigma_px=0.55, ideal=False):
    H, W, _ = img.shape
    wv = beta * img.min(-1)
    sub = {"R": img[..., 0] - wv, "G": img[..., 1] - wv, "B": img[..., 2] - wv, "W": wv}
    fine = np.zeros((H * SS, W * SS, 3))
    for name, val in sub.items():
        dx = 0.0 if ideal else COL[layout][name]
        ox = int(round((0.5 + dx) * SS)) % SS
        chans = [0, 1, 2] if name == "W" else [{"R": 0, "G": 1, "B": 2}[name]]
        for c in chans:
            col = np.zeros((H * SS, W * SS))
            col[:, ox::SS][:, :W] = np.repeat(val, SS, axis=0)
            fine[..., c] += col
    for c in range(3):
        fine[..., c] = gaussian_filter(fine[..., c], sigma_px * SS) * SS
    return fine


def run(layout, anchor, strength, dark, beta):
    pos = COL[layout]
    a = 0.0 if anchor == "centre" else pos["W"]
    src = srgb_to_lin(text_image(dark))
    saved = dict(COL[layout])
    COL[layout] = {k: a for k in saved}
    ideal = perceive_woled(src, layout, beta)
    COL[layout] = saved
    ce0, le0 = metrics(perceive_woled(src, layout, beta), ideal)
    emit = [(pos["R"] - a, 0), (pos["G"] - a, 0), (pos["B"] - a, 0)]
    ce, le = metrics(perceive_woled(correct(src, emit, "catmull", strength), layout, beta), ideal)
    return 100 * (1 - ce / ce0), 100 * (1 - le / le0)


if __name__ == "__main__":
    print("Change in colour error (fringing) / luma error (blur), worst case over dark+light and beta 0.5/0.7/0.9")
    print("(positive = better)")
    for layout in ("RWBG", "RGWB"):
        for anchor in ("centre", "white"):
            line = f"{layout} ref={anchor:6s}"
            for s in (0.15, 0.2, 0.25, 0.3, 0.5):
                res = [run(layout, anchor, s, d, b) for d in (True, False) for b in (0.5, 0.7, 0.9)]
                line += f" | s={s}: {min(r[0] for r in res):+4.0f}% / {min(r[1] for r in res):+4.0f}%"
            print(line)
