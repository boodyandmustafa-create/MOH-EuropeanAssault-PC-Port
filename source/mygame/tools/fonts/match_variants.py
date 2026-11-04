"""For every font atlas: ALL dumped textures of the same size whose alpha plane equals the font's
(same glyph picture, maybe another palette). Several hashes for one font = the palette changes at run time.

  python match_variants.py <game DATA dir> <tex_dump dir>
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


def main():
    data_dir, dump_dir = sys.argv[1], sys.argv[2]
    for viv in ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv"):
        _, ents, _, _, fl = read_viv(os.path.join(data_dir, viv))
        for name, d in fl:
            x = Xfn(d)
            alpha = x.pal.astype(np.uint8)[x.atlas][:, :, 3].astype(int)
            hits = []
            for p in sorted(glob.glob(os.path.join(dump_dir, "*_%dx%d_f0b.png" % (x.W, x.H)))):
                im = np.asarray(Image.open(p).convert("RGBA"))
                if int(np.abs(im[:, :, 3].astype(int) - alpha).sum()) == 0 and alpha.sum() > 0:
                    hits.append(os.path.basename(p).split("_")[0])
            print("%-22s %-34s %dx%d  %d hash(es): %s" % (viv.split("/")[-1], name, x.W, x.H, len(hits), " ".join(hits)))


main()
