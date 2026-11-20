"""Sharper HUD stance icons (stand / crouch / prone) for MoH European Assault.

The three silhouettes live in HUD_Sheet_Shared (256x256, texture hash eb0d3f9abd2b0110, cells from DATA/HUD/shared/hudart2.xml).
They are white with alpha 114 and an anti-aliased edge. This script rebuilds the sheet at 4x: everything else is a plain
upscale, the stance cells are re-drawn from a smoothed contour (same shape, clean edge, no pixel steps).

usage: python -I build_stance_hd.py <original sheet png> <out png> [--base existing_4x.png] [--preview preview.png]
"""
import os
import sys
import numpy as np
from PIL import Image

SCALE = 4
UP = 16                       # work resolution for the contour (per original pixel)
SIGMA = 0.30                  # contour smoothing in original pixels (0.55 was blobby, lost the rifle)
PEAK = 114                    # alpha of the silhouette body in the original sheet
RUN_CELL = (144, 77)          # top-left of the running icon's 27 x 57 cell in the 256 sheet; the exe knows the same numbers
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
CELLS = {                     # x0, x1 (inclusive), y0, y1 in original pixels (hudart2.xml stance_*_2)
    "stand":  (31, 58, 135, 196),
    "crouch": (59, 96, 135, 196),
    "prone":  (97, 165, 135, 196),
}


def gauss_blur(a, sigma):
    r = int(sigma * 3)
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-(x * x) / (2 * sigma * sigma)).astype(np.float32)
    k /= k.sum()
    pad = np.pad(a, r, mode="constant")
    h = np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 1, pad)
    return np.apply_along_axis(lambda v: np.convolve(v, k, mode="valid"), 0, h)


def drop_small(cell, frac=0.04):
    """Zero connected components (alpha > 0.25) smaller than frac of the biggest one: the neighbour cell's toes etc."""
    mk = cell > 0.25
    lab = np.zeros(cell.shape, np.int32)
    sizes = []
    h, w = cell.shape
    for sy in range(h):
        for sx in range(w):
            if mk[sy, sx] and not lab[sy, sx]:
                n = len(sizes) + 1
                stack = [(sy, sx)]
                lab[sy, sx] = n
                cnt = 0
                while stack:
                    y, x = stack.pop()
                    cnt += 1
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            yy, xx = y + dy, x + dx
                            if 0 <= yy < h and 0 <= xx < w and mk[yy, xx] and not lab[yy, xx]:
                                lab[yy, xx] = n
                                stack.append((yy, xx))
                sizes.append(cnt)
    big = max(sizes)
    out = cell.copy()
    for i, sz in enumerate(sizes, 1):
        if sz < frac * big:
            out[(lab == i)] = 0
    # also clear the soft fringe pixels next to removed components
    return out


def crisp_mask(alpha, out_w, out_h):
    """alpha: float 0..1 (original cell). Returns float 0..1 at SCALE x with a smooth, anti-aliased contour."""
    h, w = alpha.shape
    big = np.asarray(Image.fromarray(alpha.astype(np.float32), "F").resize((w * UP, h * UP), Image.BICUBIC), dtype=np.float32)
    big = gauss_blur(big, UP * SIGMA)                     # removes the stair steps of the 7-level edge
    # distance-like signal: steepness of the blurred edge is about 1/(UP*1.4) per work pixel, so map +-0.5 px to 0..1
    edge = np.clip((big - 0.5) * (UP * 1.1) * 0.5 + 0.5, 0.0, 1.0)
    # box down to SCALE x (UP/SCALE work pixels per output pixel)
    f = UP // SCALE
    edge = edge[: out_h * f, : out_w * f].reshape(out_h, f, out_w, f).mean(axis=(1, 3))
    return edge


def main():
    src, dst = sys.argv[1], sys.argv[2]
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    sheet = Image.open(src).convert("RGBA")
    assert sheet.size == (256, 256), sheet.size
    if "--base" in sys.argv:                              # an existing 4x sheet (the user's own upscale): only the stance cells change
        big = Image.open(sys.argv[sys.argv.index("--base") + 1]).convert("RGBA")
        assert big.size == (256 * SCALE, 256 * SCALE), big.size
    else:
        big = sheet.resize((256 * SCALE, 256 * SCALE), Image.LANCZOS)
    arr = np.asarray(big).copy()
    orig = np.asarray(sheet)
    pv = []
    for name, (x0, x1, y0, y1) in CELLS.items():
        cell = orig[y0:y1 + 1, x0:x1 + 1, 3].astype(np.float32) / PEAK
        cell = drop_small(np.clip(cell, 0, 1))
        h, w = cell.shape
        PAD = 3                                          # figures touch the cell border: continue the edge so it is not eaten
        padded = np.pad(cell, PAD, mode="edge")
        m = crisp_mask(padded, (w + 2 * PAD) * SCALE, (h + 2 * PAD) * SCALE)[PAD * SCALE:(PAD + h) * SCALE, PAD * SCALE:(PAD + w) * SCALE]
        X0, Y0 = x0 * SCALE, y0 * SCALE
        region = arr[Y0:Y0 + h * SCALE, X0:X0 + w * SCALE]
        region[..., :3] = 255
        region[..., 3] = np.round(m * PEAK).astype(np.uint8)
        pv.append((name, cell, m))
    # the fourth icon (running): a free 27 x 57 cell of the sheet, shown by the exe in place of "stand" while sprinting
    from run_pose import run_mask
    rm = run_mask(SCALE)
    RX0, RY0 = RUN_CELL[0] * SCALE, RUN_CELL[1] * SCALE
    region = arr[RY0:RY0 + rm.shape[0], RX0:RX0 + rm.shape[1]]
    region[..., :3] = 255
    region[..., 3] = np.round(rm * PEAK).astype(np.uint8)
    Image.fromarray(arr, "RGBA").save(dst)
    if preview:
        rows = []
        for name, cell, m in pv:
            old = np.asarray(Image.fromarray((cell * 255).astype(np.uint8)).resize((cell.shape[1] * SCALE * 2, cell.shape[0] * SCALE * 2), Image.NEAREST))
            new = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((m.shape[1] * 2, m.shape[0] * 2), Image.LANCZOS))
            rows.append(np.concatenate([old, np.full((old.shape[0], 16), 90, np.uint8), new], axis=1))
        wmax = max(r.shape[1] for r in rows)
        rows = [np.pad(r, ((0, 16), (0, wmax - r.shape[1]))) for r in rows]
        Image.fromarray(np.concatenate(rows, axis=0)).save(preview)


if __name__ == "__main__":
    main()
