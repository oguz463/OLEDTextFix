"""Compares resampling filters on a 27" 1440p QD-OLED (measured layout), strength 1.0.

The app keeps the three filters on the best trade-off between fringing and sharpness: Sharp
(Catmull-Rom), Balanced (bicubic B=0.2, C=0.6) and Smooth (B=0.3, C=0.5). The others are listed for
comparison. A sweep of the whole bicubic (B, C) grid plus Lanczos-2/3/4, Kaiser- and Blackman-windowed
sinc (6 taps) and Gaussians found nothing better: longer sinc filters only add halo, and every other
filter is matched by a bicubic with the same fringing and more sharpness.

Prints the change in colour error (fringing) and luma error (sharpness loss) against an image whose
three colours all sit at the green subpixel; positive = better. Also prints how much the shifted
image over/undershoots [0, 1] (halo that gets clipped), in thousandths.
"""
import numpy as np
import sim
from sim import correct, perceive, metrics, srgb_to_lin, text_image

FILTERS = ["catmull", "balanced", "smooth", "lanczos2", "robidouxsharp", "mitchell", "bilinear", "bspline"]
R = np.array([-0.22, 0.50]); G = np.array([0.0, 0.0]); B = np.array([0.27, 0.52])
c = (R + G + B) / 3
EMIT = [tuple(R - c), tuple(G - c), tuple(B - c)]  # physical emitters, centred on their mean
OFF = [tuple(R), tuple(G), tuple(B)]                # preset values (green fixed)

if __name__ == "__main__":
    for dark in (True, False):
        src = srgb_to_lin(text_image(dark))
        ideal = perceive(src, [tuple(G - c)] * 3)
        ce0, le0 = metrics(perceive(src, EMIT), ideal)
        print(f"\n{'Dark' if dark else 'Light'} background   fringing   sharpness   halo")
        for k in FILTERS:
            ce, le = metrics(perceive(correct(src, OFF, k, 1.0), EMIT), ideal)
            raw = np.stack([sim.shift_channel(src[..., ch], OFF[ch][0], OFF[ch][1], k) for ch in range(3)], -1)
            halo = np.mean(np.clip(raw - 1, 0, None) + np.clip(-raw, 0, None)) * 1000
            print(f"  {k:14s} {100 * (1 - ce / ce0):+6.0f}%   {100 * (1 - le / le0):+6.0f}%   {halo:5.2f}")
