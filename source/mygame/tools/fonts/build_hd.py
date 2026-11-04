"""High-resolution English fonts for MoH: European Assault.

For each font listed in FONTS the glyphs are redrawn from a Windows TrueType font at S times the game's
resolution. Two things come out:
  * a language-pack layer (default languages\\english_hd) holding realfont.viv / realftFE.viv whose xfn has the
    new glyph records and an ordinary 1x atlas (the fallback; same atlas size as the game's), and
  * mods\\textures\\<hash>_<name>.png = the S-times atlas, named after the hash the engine computes for the
    new 1x atlas (texmod replacement, see mods\\textures\\README.txt).
Glyph records keep the game's kerning byte; codes are unchanged.

  python build_hd.py <game DATA dir> <out pack dir> <out textures dir> [--preview out.png] [--only <font name>]
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "arabic"))
from vivlib import read_viv, build_viv  # noqa: E402
from xfnlib import Xfn  # noqa: E402
from swz import swizzle  # noqa: E402
import arfont  # noqa: E402

S = 4
VIVS = ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv")
WINFONTS = "C:/Windows/Fonts"
# font name in the viv -> (TrueType file, tracking in 1x px added to every advance, fraction of the cap height)
FONTS = {
    "Futura Std Condensed ExtBd_18.xfn": dict(ttf="impact.ttf", track=0.0, tag="ExtBd18"),
    "Courier New_18.xfn": dict(ttf="impact.ttf", track=0.0, tag="Courier18"),
    "Impact_24.xfn": dict(ttf="impact.ttf", track=0.0, tag="Impact24"),
    # (mode="up" = the game's own glyphs, 4x, edges tightened, records and hash stay the original's; unused now:
    #  every face is redrawn in the Impact look of ExtBd_18 that the user chose)
    "Futura Std Book_18.xfn": dict(ttf="impact.ttf", track=0.0, tag="Book18"),
    "Trajan Pro_18.xfn": dict(ttf="impact.ttf", track=0.0, tag="Trajan18"),
    "Trajan Pro_24.xfn": dict(ttf="impact.ttf", track=0.0, tag="Trajan24"),
    "Futura Std Condensed_24.xfn": dict(ttf="impact.ttf", track=0.0, tag="FuturaCond24"),
    "Tw Cen MT Condensed_36.xfn": dict(ttf="impact.ttf", track=0.0, tag="TwCen36"),
}

M64 = (1 << 64) - 1
MUL = 0x100000001B3


def hash_bytes(h, p):
    """the engine's texdec_hash_bytes (nv2a_pb_exec.c)"""
    n = len(p)
    lanes = [h, h ^ 0x9E3779B97F4A7C15, h ^ 0xC2B2AE3D27D4EB4F, h ^ 0x165667B19E3779F9]
    i = 0
    while i + 32 <= n:
        for k in range(4):
            w = int.from_bytes(p[i + 8 * k:i + 8 * k + 8], "little")
            v = ((lanes[k] ^ w) * MUL) & M64
            lanes[k] = v ^ (v >> 29)
        i += 32
    h = (((lanes[0] * 31 + lanes[1]) * 31 + lanes[2]) * 31 + lanes[3]) & M64
    while i + 8 <= n:
        w = int.from_bytes(p[i:i + 8], "little")
        h = ((h ^ w) * MUL) & M64
        h ^= h >> 29
        i += 8
    while i < n:
        h = ((h ^ p[i]) * MUL) & M64
        i += 1
    return h


def tex_hash(x):
    """hash of the font atlas texture as the engine sees it: swizzled P8 bytes, then the 256 RGBA palette"""
    h = hash_bytes(0xCBF29CE484222325, swizzle(x.atlas.reshape(-1), x.W, x.H))
    return hash_bytes(h, bytes(x.post[0x40:0x440]))


def smooth(v, lo, hi):
    t = np.clip((v - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def upscale_rgba(rgba, s):
    """a glyph the TrueType font has no letter for: the game's own bitmap, Lanczos + tightened edges"""
    h, w = rgba.shape[:2]
    a = rgba[:, :, 3].astype(np.float32) / 255.0
    planes = [rgba[:, :, c].astype(np.float32) / 255.0 * a for c in range(3)] + [a]
    up = [np.clip(np.asarray(Image.fromarray(p, mode="F").resize((w * s, h * s), Image.LANCZOS)), 0, 1) for p in planes]
    a2 = up[3]
    out = np.zeros((h * s, w * s, 4), dtype=np.float32)
    for c in range(3):
        col = np.clip(up[c] / np.maximum(a2, 1e-4), 0, 1)
        out[:, :, c] = smooth(col, 0.35, 0.65) * 0.75 + col * 0.25
    out[:, :, 3] = smooth(a2, 0.30, 0.70)
    return (out * 255.0 + 0.5).astype(np.uint8)


def ttf_has(font, ch, missing_mask):
    m = font.getmask(ch)
    return not (m.size == missing_mask.size and bytes(m) == bytes(missing_mask))


def build_one(x, cfg):
    st = arfont.analyse(x)
    pal = x.pal
    zero = x.zero_index()
    lut = arfont.palette_lut(x)
    ttf = os.path.join(WINFONTS, cfg["ttf"])
    # em size so that the TrueType 'H' is as tall as the game's
    ref = ImageFont.truetype(ttf, 1000, layout_engine=ImageFont.Layout.BASIC)
    bb = ref.getbbox("H", anchor="ls")
    cap_ratio = (bb[3] - bb[1]) / 1000.0
    px1 = st["capH"] / cap_ratio
    if cfg.get("fit_adv"):
        # the text boxes of the game were laid out for the old advances: size the face to the old advance
        oa = [arfont.rec_fields(x, r) for r in x.rec_raw]
        oadv = [f["adv"] for f in oa if f["code"] == ord(cfg["fit_adv"])][0]
        px1 = oadv / (ref.getlength(cfg["fit_adv"]) / 1000.0)
    hi = ImageFont.truetype(ttf, px1 * S, layout_engine=ImageFont.Layout.BASIC)
    missing = hi.getmask("\uffff")
    sl, sT, sr, sb = st["sl"], st["st"], max(st["sr"], 0), max(st["sb"], 0)
    fillL = st["fillL"]

    orig = {}
    raw0 = {}
    orig_rgba = pal.astype(np.uint8)[x.atlas]
    for raw in x.rec_raw:
        f = arfont.rec_fields(x, raw)
        orig[f["code"]] = f
        raw0[f["code"]] = raw

    glyphs = []
    keep_ws = {}
    stats = dict(ttf=0, fallback=0, blank=0)
    for code in sorted(orig):
        f = orig[code]
        ch = chr(code)
        cell = orig_rgba[f["y"]:f["y"] + f["h"], f["x"]:f["x"] + f["w"]]
        if f["w"] == 0 or f["h"] == 0 or cell[:, :, 3].max() == 0:
            keep_ws[code] = f
            stats["blank"] += 1
            continue
        if code >= 0x20 and ttf_has(hi, ch, missing):
            b = hi.getbbox(ch, anchor="ls")
            adv_hi = hi.getlength(ch)
            if b[2] - b[0] > 0 and b[3] - b[1] > 0:
                bx0, by0 = b[0] - sl * S, b[1] - sT * S
                bx1, by1 = b[2] + sr * S, b[3] + sb * S
                left, top = int(np.floor(bx0 / S)), int(np.floor(by0 / S))
                right, bottom = int(np.ceil(bx1 / S)), int(np.ceil(by1 / S))
                w, h = right - left, bottom - top
                im = Image.new("L", (w * S, h * S), 0)
                ImageDraw.Draw(im).text((-left * S, -top * S), ch, font=hi, fill=255, anchor="ls")
                full = np.asarray(im, dtype=np.float32) / 255.0
                shadow = np.zeros_like(full)
                steps = max(sr, sb) * S
                for t in range(1, steps + 1):
                    dx, dy = int(round(sr * S * t / steps)), int(round(sb * S * t / steps))
                    sh = np.zeros_like(full)
                    sh[dy:, dx:] = full[:full.shape[0] - dy, :full.shape[1] - dx]
                    shadow = np.maximum(shadow, sh)
                if st["flat"]:
                    shadow = np.zeros_like(full)
                sm = Image.fromarray((shadow * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.4 * S))
                shadow = np.asarray(sm, dtype=np.float32) / 255.0
                A = np.maximum(full, shadow)
                Lt = np.where(A > 0, fillL * full / np.maximum(A, 1e-6), 0.0)
                rgba_hi = np.zeros((h * S, w * S, 4), dtype=np.uint8)
                Li = np.clip(np.rint(Lt), 0, 255).astype(np.uint8)
                rgba_hi[:, :, 0] = rgba_hi[:, :, 1] = rgba_hi[:, :, 2] = Li
                rgba_hi[:, :, 3] = np.clip(np.rint(A * 255), 0, 255).astype(np.uint8)
                adv = int(round(adv_hi / S + cfg["track"])) + (0 if st["flat"] else 1)
                glyphs.append(dict(raw=raw0[code], code=code, w=w, h=h, hi=rgba_hi, adv=max(adv, 1), xoff=left,
                                   yoff=st["base"] + top, k=f["k"]))
                stats["ttf"] += 1
                continue
        # no TrueType letter: keep the game's own picture and metrics, only sharper
        glyphs.append(dict(raw=raw0[code], code=code, w=f["w"], h=f["h"], hi=upscale_rgba(cell, S), adv=f["adv"],
                           xoff=f["xoff"], yoff=f["yoff"] if f["yoff"] < 128 or not st["signed"] else f["yoff"], k=f["k"],
                           raw_yoff=True))
        stats["fallback"] += 1

    # unsigned yoff fonts: nothing may start above the line; shift everything down together when needed
    if not st["signed"]:
        lows = [g["yoff"] for g in glyphs if not g.get("raw_yoff")]
        d = -min(lows) if lows and min(lows) < 0 else 0
        if d:
            for g in glyphs:
                if not g.get("raw_yoff"):
                    g["yoff"] += d
        stats["shift"] = d

    # 1x atlas: box-average the hi-res cells, quantise to the font's own palette
    for g in glyphs:
        r = g["hi"].reshape(g["h"], S, g["w"], S, 4).astype(np.float32)
        a = r[:, :, :, :, 3]
        a1 = a.mean(axis=(1, 3))
        l1 = (r[:, :, :, :, 0] * a).sum(axis=(1, 3)) / np.maximum(a.sum(axis=(1, 3)), 1e-6)
        ai = np.clip(np.rint(a1), 0, 255).astype(np.uint8)
        li = np.clip(np.rint(l1), 0, 255).astype(np.uint8)
        idx = lut[li, ai]
        idx[ai == 0] = zero
        g["idx"] = idx
    x.atlas[:] = zero
    arfont.pack(x, glyphs, [], gap=1, allow_grow=True)

    recs = []
    for g in glyphs:
        if x.rs == 12:
            recs.append(struct.pack("<HBBHHBbBB", g["code"], g["w"], g["h"], g["x"], g["y"], g["adv"] & 0xFF,
                                    g["xoff"], g["yoff"] & 0xFF, g["k"]))
        else:
            # 16 byte record: keep the unknown bytes [8], [11], [12:14] of the original, replace the known fields
            r = bytearray(g["raw"])
            struct.pack_into("<HBBHH", r, 0, g["code"], g["w"], g["h"], g["x"], g["y"])
            struct.pack_into("<bB", r, 9, g["xoff"], g["yoff"] & 0xFF)
            struct.pack_into("<H", r, 14, g["adv"])
            recs.append(bytes(r))
    for code, f in keep_ws.items():
        if x.rs == 12:
            recs.append(struct.pack("<HBBHHBbBB", code, 0, 0, 0, 0, f["adv"] & 0xFF, f["xoff"], f["yoff"] & 0xFF, f["k"]))
        else:
            recs.append(bytes(raw0[code]))
    recs.sort(key=lambda r: struct.unpack_from("<H", r)[0])
    x.rec_raw = recs
    x.n = len(recs)

    big = np.zeros((x.H * S, x.W * S, 4), dtype=np.uint8)
    for g in glyphs:
        big[g["y"] * S:(g["y"] + g["h"]) * S, g["x"] * S:(g["x"] + g["w"]) * S] = g["hi"]
    return big, stats, st, glyphs


def draw_preview(x, atlas, scale, text, canvas, pos, F=3):
    recs = {struct.unpack_from("<H", r)[0]: struct.unpack("<HBBHHBbBB", r) for r in x.rec_raw}
    sg = lambda v: v - 256 if v >= 128 else v
    signed = recs[ord("H")][7] >= 128
    hb = -sg(recs[ord("H")][7]) if signed else 0
    pen = 0
    for ch in text:
        r = recs.get(ord(ch))
        if r is None:
            continue
        code, w, h, gx, gy, adv, xoff, yoff, k = r
        if w and h:
            tile = atlas[gy * scale:(gy + h) * scale, gx * scale:(gx + w) * scale]
            im = Image.fromarray(np.ascontiguousarray(tile), "RGBA").resize((w * F, h * F), Image.BILINEAR)
            y0 = hb + sg(yoff) if signed else yoff
            canvas.alpha_composite(im, (int(pos[0] + (pen + xoff) * F), int(pos[1] + y0 * F)))
        pen += adv


def recs16(x):
    return x.recs() if x.rs == 12 else x.recs()


def main():
    args = sys.argv[1:]
    data_dir, pack_dir, tex_dir = args[0], args[1], args[2]
    preview = args[args.index("--preview") + 1] if "--preview" in args else None
    only = args[args.index("--only") + 1] if "--only" in args else None
    os.makedirs(tex_dir, exist_ok=True)
    rows = {}
    for viv in VIVS:
        b, ents, hdr_len, end, files = read_viv(os.path.join(data_dir, viv))
        new = []
        for name, d in files:
            if name in FONTS and (only is None or only == name):
                cfg = FONTS[name]
                x0 = Xfn(d)
                h0 = tex_hash(x0)
                if cfg.get("mode") == "up":
                    big = upscale_rgba(x0.pal.astype(np.uint8)[x0.atlas], S)
                    path = os.path.join(tex_dir, "%016x_%s_hd.png" % (h0, cfg["tag"]))
                    Image.fromarray(big, "RGBA").save(path)
                    print("%s %s: upscale-only, hash %016x (unchanged)" % (viv.split("/")[-1], name, h0))
                    rows.setdefault(name, (x0, x0, big, 1))
                else:
                    x = Xfn(d)
                    big, stats, st, glyphs = build_one(x, cfg)
                    h = tex_hash(x)
                    path = os.path.join(tex_dir, "%016x_%s_hd.png" % (h, cfg["tag"]))
                    Image.fromarray(big, "RGBA").save(path)
                    print("%s %s: %016x -> %016x %s" % (viv.split("/")[-1], name, h0, h, stats))
                    d = x.serialize()
                    rows.setdefault(name, (x0, x, big, 0))
            new.append((name, d))
        out = os.path.join(pack_dir, "DATA", viv)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(build_viv(new, (b, ents, hdr_len, end)))
    if preview and rows:
        text = "Objective: Destroy the petrol tanks 0123"
        W, H = 1500, 120
        bg = (58, 64, 54, 255)
        rows = {n: v for n, v in rows.items() if v[0].rs == 12}
        sheet = Image.new("RGBA", (W, H * 2 * len(rows)), bg)
        for k, (name, (x0, x, big, up)) in enumerate(rows.items()):
            orig_rgba = x0.pal.astype(np.uint8)[x0.atlas]
            draw_preview(x0, orig_rgba, 1, text, sheet, (10, 2 * k * H + 30))
            draw_preview(x, big, S, text, sheet, (10, (2 * k + 1) * H + 30))
            ImageDraw.Draw(sheet).text((1100, 2 * k * H + 5), name, fill=(255, 220, 120, 255))
        sheet.convert("RGB").save(preview)


main()
