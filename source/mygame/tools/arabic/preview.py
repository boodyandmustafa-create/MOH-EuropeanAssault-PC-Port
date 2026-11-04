"""Draw strings with a (patched) Xfn the way the engine should: pen advance, xoff/yoff, palette alpha blend."""
import numpy as np
from PIL import Image
import arfont


def draw_text(x, text, line_h=24, pad=6, scale=3, bg=(70, 90, 70)):
    st = arfont.analyse(x)
    recs = {}
    for r in x.rec_raw:
        f = arfont.rec_fields(x, r)
        recs[f["code"]] = f
    width = pad * 2 + sum(recs.get(ord(c), {"adv": 8})["adv"] for c in text) + 4
    W, Hh = width, line_h + 2 * pad
    canvas = np.zeros((Hh, W, 3), dtype=np.float32)
    canvas[:] = bg
    pen = pad
    base_y = pad + st["base"]
    pal = x.pal
    for c in text:
        f = recs.get(ord(c))
        if f is None:
            pen += 8
            continue
        yoff = f["yoff"]
        if st["signed"]:
            top = pad + st["base"] - 0 + (yoff - 256 if yoff >= 128 else yoff) - st["base"] + st["base"]
            top = base_y - st["base"] + (yoff - 256 if yoff >= 128 else yoff) + 0
            top = pad + (st["base"] if False else 0) + 0
        # line-top relative (unsigned): top = pad + yoff ; baseline relative (signed): top = pad_baseline + yoff
        if not st["signed"]:
            top = pad + yoff
        else:
            top = pad + 14 + (yoff - 256 if yoff >= 128 else yoff)  # 14 = arbitrary baseline row for the preview
        left = pen + f["xoff"]
        sub = x.atlas[f["y"]:f["y"] + f["h"], f["x"]:f["x"] + f["w"]]
        p = pal[sub]
        a = (p[..., 3] / 255.0)[..., None]
        col = p[..., 0:3].astype(np.float32)
        for j in range(f["h"]):
            yy = top + j
            if yy < 0 or yy >= Hh:
                continue
            for i in range(f["w"]):
                xx = left + i
                if xx < 0 or xx >= W:
                    continue
                canvas[yy, xx] = canvas[yy, xx] * (1 - a[j, i]) + col[j, i] * a[j, i]
        pen += f["adv"]
    im = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8))
    return im.resize((W * scale, Hh * scale), Image.NEAREST)
