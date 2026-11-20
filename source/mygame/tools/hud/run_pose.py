"""The fourth stance silhouette (running), from the figure the user supplied (run_pose_source.png: a running soldier, grey
upper body and white legs on a dark background). Only its shape is used: it is cut out by brightness, re-drawn in the
same flat white / alpha 114 as stand, crouch and prone, and given the same crisp contour.

The cell is 27 x 57 original sheet pixels (x 144..171, y 77..134 of HUD_Sheet_Shared, free space in the sheet). The game
stretches the cell to the stand icon's 28 x 61 box (x 1.037, y 1.07), so the figure is pre-squeezed to look right on screen.

usage (preview): python -I run_pose.py <out png>
"""
import os
import sys

import numpy as np
from PIL import Image

CELL_W, CELL_H = 27, 57
STRETCH_X, STRETCH_Y = 28.0 / 27.0, 61.0 / 57.0
SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_pose_source.png")
UP = 4                                        # work oversampling on top of the final scale


def _blur(a, sigma):
    r = max(1, int(sigma * 3))
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-(x * x) / (2 * sigma * sigma)).astype(np.float32)
    k /= k.sum()
    p = np.pad(a, r)
    h = np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 1, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 0, h)


def source_mask():
    """Float 0..1 mask of the figure, cropped to its bounding box."""
    a = np.asarray(Image.open(SOURCE).convert("RGB"), dtype=np.float32).mean(2)
    m = np.clip((a - 60.0) / 40.0, 0.0, 1.0)      # background is < 35, the grey body is about 130, the legs about 230
    # keep the biggest blob only (the dark rounded frame and stray specks are below the threshold anyway)
    ys, xs = np.where(m > 0.5)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    return m[max(0, y0 - 3):y1 + 3, max(0, x0 - 3):x1 + 3], (x1 - x0, y1 - y0)


def run_mask(scale=4, margin_bottom=1.3):
    """Float mask 0..1 of the 27 x 57 cell at `scale` x, contour crisp like the other icons."""
    m, (fw, fh) = source_mask()
    fa = fw / float(fh)
    # figure size in cell pixels: as wide as the cell allows (1 px side margin); height from the on-screen aspect
    wc = CELL_W - 2.0
    hc = wc * STRETCH_X / (STRETCH_Y * fa)
    if hc > CELL_H - 2.0:
        hc = CELL_H - 2.0
        wc = hc * STRETCH_Y * fa / STRETCH_X
    # resample the cropped source so that the figure's bounding box is wc x hc cell pixels, at scale*UP per pixel
    pad_x = (m.shape[1] - fw) / 2.0
    pad_y = (m.shape[0] - fh) / 2.0
    sx = wc * scale * UP / fw
    sy = hc * scale * UP / fh
    tw, th = int(round(m.shape[1] * sx)), int(round(m.shape[0] * sy))
    big = np.asarray(Image.fromarray(m.astype(np.float32), "F").resize((tw, th), Image.BICUBIC), dtype=np.float32)
    big = _blur(big, UP * 0.45)                                    # no pixel steps, thin rifle kept
    edge = np.clip((big - 0.5) * (UP * 1.6) + 0.5, 0.0, 1.0)       # crisp contour, about one final pixel of anti-aliasing
    # box down to `scale` x
    h2, w2 = th // UP, tw // UP
    edge = edge[:h2 * UP, :w2 * UP].reshape(h2, UP, w2, UP).mean(axis=(1, 3))
    # place: centred, standing on the cell's bottom (same ground line as the stand icon)
    out = np.zeros((CELL_H * scale, CELL_W * scale), np.float32)
    fx0, fy0 = int(round(pad_x * sx / UP)), int(round(pad_y * sy / UP))      # figure's top-left inside `edge`
    fwp, fhp = int(round(fw * sx / UP)), int(round(fh * sy / UP))
    X = int(round((CELL_W * scale - fwp) / 2.0)) - fx0
    Y = int(round((CELL_H - margin_bottom) * scale - fhp)) - fy0
    for yy in range(edge.shape[0]):
        oy = Y + yy
        if 0 <= oy < out.shape[0]:
            x0 = max(0, X)
            x1 = min(out.shape[1], X + edge.shape[1])
            if x1 > x0:
                out[oy, x0:x1] = np.maximum(out[oy, x0:x1], edge[yy, x0 - X:x1 - X])
    return out


if __name__ == "__main__":
    m = run_mask()
    print(m.shape, float(m.max()))
    Image.fromarray((m * 255).astype(np.uint8)).resize((m.shape[1] * 3, m.shape[0] * 3), Image.LANCZOS).save(sys.argv[1])
