"""Finds the dumped texture (hash) of every font atlas of the game.

Decodes the 10 FntX fonts of realfont.viv / realftFE.viv to RGBA and compares them with the PNGs of a
texture dump (name = <hash>_<W>x<H>_f0b.png) of the same size. Prints the best match per font.

  python match_fonts.py <game DATA dir> <tex_dump dir>
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "arabic"))
from vivlib import read_viv  # noqa: E402
from xfnlib import Xfn  # noqa: E402


def atlas_rgba(x, bgra):
    pal = x.pal.astype(np.uint8)
    if bgra:
        pal = pal[:, [2, 1, 0, 3]]
    return pal[x.atlas]


def main():
    data_dir, dump_dir = sys.argv[1], sys.argv[2]
    cache = {}
    for viv in ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv"):
        _, ents, _, _, fl = read_viv(os.path.join(data_dir, viv))
        for name, d in fl:
            x = Xfn(d)
            key = (x.W, x.H)
            if key not in cache:
                cache[key] = sorted(glob.glob(os.path.join(dump_dir, "*_%dx%d_f0b.png" % key)))
            best = None
            for bgra in (False, True):
                a = atlas_rgba(x, bgra).astype(int)
                for p in cache[key]:
                    im = np.asarray(Image.open(p).convert("RGBA")).astype(int)
                    if im.shape != a.shape:
                        continue
                    diff = int(np.abs(im - a).sum())
                    if best is None or diff < best[0]:
                        best = (diff, os.path.basename(p), bgra)
            print("%-22s %-14s %4dx%-4d glyphs %3d rs %d  candidates %4d  best %s" % (
                viv.split("/")[-1], name, x.W, x.H, x.n, x.rs, len(cache[key]), best))


main()
