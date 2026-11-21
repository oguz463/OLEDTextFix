"""Sweeps correction strength on each measured QD-OLED panel class (Sharp filter, both backgrounds).

Prints fringing reduction and sharpness change (positive = better) for strengths 0.5 ... 1.2.
"""
import numpy as np
from sim import correct, perceive, metrics, srgb_to_lin, text_image

CLASSES = {
    "27 1440p": ((-0.22, 0.50), (0.27, 0.52)),
    "27 4K":    ((-0.17, 0.50), (0.32, 0.50)),
    "32 4K":    ((-0.13, 0.50), (0.37, 0.50)),
    "34 UW":    ((-0.31, 0.49), (0.22, 0.49)),
    "49 SUW":   ((-0.18, 0.50), (0.32, 0.50)),
}
STR = [0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2]

def run(kernel="catmull"):
    for name, (R, B) in CLASSES.items():
        R = np.array(R); G = np.zeros(2); B = np.array(B)
        c = (R + G + B) / 3
        emit = [tuple(R - c), tuple(G - c), tuple(B - c)]
        off = [tuple(R), tuple(G), tuple(B)]
        print(f"\n{name}  ({kernel})   fringe% / sharp% dark | light")
        res = {}
        for dark in (True, False):
            src = srgb_to_lin(text_image(dark))
            ideal = perceive(src, [tuple(G - c)] * 3)
            ce0, le0 = metrics(perceive(src, emit), ideal)
            for s in STR:
                ce, le = metrics(perceive(correct(src, off, kernel, s), emit), ideal)
                res[(dark, s)] = (100 * (1 - ce / ce0), 100 * (1 - le / le0))
        for s in STR:
            d, l = res[(True, s)], res[(False, s)]
            print(f"  {s:.1f}  {d[0]:+4.0f}/{d[1]:+4.0f}  |  {l[0]:+4.0f}/{l[1]:+4.0f}")

if __name__ == "__main__":
    import sys
    run(sys.argv[1] if len(sys.argv) > 1 else "catmull")
