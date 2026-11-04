"""Before / after preview of the font upscale.  python proto.py <game DATA dir> <out png>

Rows per sample: the game's atlas as it is stretched today (1x glyphs, bilinear to the screen size),
(a) the same glyphs rebuilt at 4x (Lanczos + edge sharpening), (b) a Windows font drawn at screen size.
Only 12-byte-record fonts (ExtBd_18, Courier_18, Trajan_18, TwCen_36) are drawn here.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "arabic"))
from vivlib import read_viv  # noqa: E402
from xfnlib import Xfn  # noqa: E402

F = 3          # screen magnification of the game's 480p text (1440p)
S = 4          # atlas scale of the new textures


def smooth(x, lo, hi):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def upscale_rgba(rgba, s, lo=0.30, hi=0.70):
    """rgba uint8 HxWx4 -> uint8 (H*s)x(W*s)x4: Lanczos on premultiplied colour, then the edges are pulled tight."""
    h, w = rgba.shape[:2]
    a = rgba[:, :, 3].astype(np.float32) / 255.0
    ch = []
    for c in range(3):
        ch.append(rgba[:, :, c].astype(np.float32) / 255.0 * a)
    ch.append(a)
    up = []
    for plane in ch:
        im = Image.fromarray(plane, mode="F").resize((w * s, h * s), Image.LANCZOS)
        up.append(np.clip(np.asarray(im), 0.0, 1.0))
    a2 = up[3]
    a_sharp = smooth(a2, lo, hi)
    out = np.zeros((h * s, w * s, 4), dtype=np.float32)
    safe = np.maximum(a2, 1e-4)
    for c in range(3):
        col = np.clip(up[c] / safe, 0.0, 1.0)            # straight colour at this texel
        # fill (light) versus shadow (dark) is tightened the same way as the silhouette
        out[:, :, c] = smooth(col, 0.35, 0.65) * 0.75 + col * 0.25
    out[:, :, 3] = a_sharp
    return (out * 255.0 + 0.5).astype(np.uint8)


def load(data_dir, vivname, fontname):
    _, ents, _, _, fl = read_viv(os.path.join(data_dir, vivname))
    for name, d in fl:
        if name == fontname:
            return Xfn(d)
    raise KeyError(fontname)


def draw_text(x, atlas_rgba, scale_atlas, text, canvas, pos, topmode):
    """Blit glyphs of 'text' onto 'canvas' (PIL RGBA, screen size) at pos (screen px): the glyph picture
    is cut from atlas_rgba (scale_atlas x the game's) and resized so that 1 game px = F screen px."""
    recs = {r[0]: r for r in x.recs()}
    pen = 0
    hrec = recs[ord("H")]
    base = -np.int8(hrec[8] & 0xFF).item() if not topmode else 0
    for ch in text:
        r = recs.get(ord(ch))
        if r is None:
            continue
        code, w, h, gx, gy, adv, xoff, yoff, kern = r
        if w and h:
            tile = atlas_rgba[gy * scale_atlas:(gy + h) * scale_atlas, gx * scale_atlas:(gx + w) * scale_atlas]
            im = Image.fromarray(tile, "RGBA")
            im = im.resize((w * F, h * F), Image.BILINEAR)
            if topmode:
                y0 = yoff
            else:
                y0 = base + np.int8(yoff & 0xFF).item() if False else base + (yoff if yoff < 128 else yoff - 256)
            px = pos[0] + (pen + (xoff if xoff < 128 else xoff - 256)) * F
            py = pos[1] + y0 * F
            canvas.alpha_composite(im, (int(px), int(py)))
        pen += adv
    return pen


def main():
    data_dir, out = sys.argv[1], sys.argv[2]
    samples = [
        ("SHARED/UI/FE/realfont.viv", "Futura Std Condensed ExtBd_18.xfn", True, "Objective: Destroy the petrol tanks", "impact.ttf", 20),
        ("SHARED/UI/FE/realfont.viv", "Courier New_18.xfn", True, "Saving Holt's profile... Medkit used!", "cour.ttf", 18),
    ]
    rows = []
    for viv, name, topmode, text, ttf, tsize in samples:
        x = load(data_dir, viv, name)
        pal = x.pal.astype(np.uint8)
        rgba = pal[x.atlas]
        up = upscale_rgba(rgba, S)
        W = 1500
        H = 150
        bg = (58, 64, 54, 255)
        row = Image.new("RGBA", (W, H * 3), bg)
        for k, (atl, sc) in enumerate(((rgba, 1), (up, S))):
            c = Image.new("RGBA", (W, H), bg)
            draw_text(x, atl, sc, text, c, (10, 40), topmode)
            row.paste(c, (0, k * H))
        c = Image.new("RGBA", (W, H), bg)
        d = ImageDraw.Draw(c)
        f = ImageFont.truetype(os.path.join("C:/Windows/Fonts", ttf), int(tsize * F * 0.95))
        d.text((13, 43), text, font=f, fill=(20, 20, 20, 255))
        d.text((10, 40), text, font=f, fill=(255, 255, 255, 255))
        row.paste(c, (0, 2 * H))
        rows.append(row)
    sheet = Image.new("RGBA", (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.convert("RGB").save(out)
    print("saved", out, sheet.size)


main()
