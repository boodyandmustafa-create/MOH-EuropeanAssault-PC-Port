"""Add Arabic presentation-form glyphs to the P8 FntX fonts of realfont.viv / realftFE.viv.

add_arabic(xfn, ttf) analyses the style of the font (fill colour, shadow size, baseline) from its own
'H' glyph, renders the Arabic forms with PIL in the same style, packs them into free atlas space (growing
the atlas if needed) and appends the glyph records.
"""
import struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import arshape

# code points that get a glyph: every presentation form the shaper can emit + Arabic punctuation
EXTRA = [0x060C, 0x061B, 0x061F, 0x0640]


def wanted_codes():
    s = set()
    for f in arshape.FORMS.values():
        for c in f.values():
            s.add(ord(c))
    for f in arshape.LIGS.values():
        for c in f.values():
            s.add(ord(c))
    s.update(EXTRA)
    return sorted(s)


def rec_fields(x, raw):
    """-> dict(code,w,h,x,y,adv,xoff,yoff(raw byte),k) for either record flavour"""
    code, w, h, ax, ay = struct.unpack_from("<HBBHH", raw)
    if x.rs == 12:
        adv, xoff, yoff, k = struct.unpack_from("<BbBB", raw, 8)
    else:
        xoff, yoff = struct.unpack_from("<bB", raw, 9)
        adv = struct.unpack_from("<H", raw, 14)[0]
        k = 0
    return dict(code=code, w=w, h=h, x=ax, y=ay, adv=adv, xoff=xoff, yoff=yoff, k=k)


def make_rec(x, code, w, h, ax, ay, adv, xoff, yoff):
    yoff &= 0xFF
    if x.rs == 12:
        return struct.pack("<HBBHHBbBB", code, w, h, ax, ay, adv & 0xFF, xoff, yoff, 0)
    return struct.pack("<HBBHH", code, w, h, ax, ay) + struct.pack("<BbBBHH", 0, xoff, yoff, 0, 0, adv)


def analyse(x):
    """style of the font from its 'H' glyph"""
    pal = x.pal
    H = None
    for raw in x.rec_raw:
        f = rec_fields(x, raw)
        if f["code"] == 0x48:
            H = f
            break
    sub = x.atlas[H["y"]:H["y"] + H["h"], H["x"]:H["x"] + H["w"]]
    L = pal[sub, 0]
    A = pal[sub, 3]
    amax = int(A.max())
    solid = A >= (250 if amax >= 250 and (A >= 250).sum() > 8 else int(amax * 0.85))   # flat fonts reach 255 on few texels
    lmax = L[solid].max() if solid.any() else 255
    cand = solid & (L >= 0.6 * lmax)
    fillL = int(np.bincount(L[cand].astype(int)).argmax())
    rows = np.where(cand.any(axis=1))[0]
    cols = np.where(cand.any(axis=0))[0]
    r0, r1, c0, c1 = rows[0], rows[-1], cols[0], cols[-1]
    vis = A >= 128
    sb = int(vis[r1 + 1:].any(axis=1).sum()) if r1 + 1 < H["h"] else 0
    st = int(vis[:r0].any(axis=1).sum())
    sr = int(vis[:, c1 + 1:].any(axis=0).sum()) if c1 + 1 < H["w"] else 0
    sl = int(vis[:, :c0].any(axis=0).sum())
    signed = H["yoff"] >= 128
    yoff_h = H["yoff"] - 256 if signed else H["yoff"]
    base = yoff_h + int(r1) + 1          # row (in the font's yoff units) just under the baseline
    # a font without any dark, opaque texel has no shadow at all (Courier): flat grey with an alpha ramp
    used = np.unique(x.atlas)
    flat = not ((pal[used, 0] < 20) & (pal[used, 3] > 200)).any()
    capH = int(r1 - r0 + 1)
    if flat:
        sl = st = sr = sb = 0
        if capH < 5:                       # thin strokes: take the whole glyph box
            capH = int(H["h"])
            base = yoff_h + int(H["h"])
    return dict(fillL=fillL, capH=capH, sl=sl, st=st, sr=sr, sb=sb, signed=signed, base=base, H=H, flat=flat)


def palette_lut(x):
    """LUT[L][A] -> palette index (nearest in (L, A) among the entries the atlas actually uses)"""
    used = np.unique(x.atlas)
    pal = x.pal[used]
    Ls = np.arange(256)[:, None, None]
    As = np.arange(256)[None, :, None]
    # weigh alpha more: the shadow/edge shape matters more than the grey level
    d = (Ls - pal[None, None, :, 0]) ** 2 * 0.5 + 2.0 * (As - pal[None, None, :, 3]) ** 2
    return used[np.argmin(d, axis=2)].astype(np.uint8)


def render_bitmaps(x, ttf, st, size_delta=0):
    codes = wanted_codes()
    # pixel size: Arabic alef as tall as the Latin caps of this font
    target = st["capH"] + 1 + size_delta
    px = 8
    for p in range(8, 80):
        f = ImageFont.truetype(ttf, p, layout_engine=ImageFont.Layout.BASIC)
        b = f.getbbox("ﺍ", anchor="ls")
        if (b[3] - b[1]) >= target:
            px = p
            break
    font = ImageFont.truetype(ttf, px, layout_engine=ImageFont.Layout.BASIC)
    lut = palette_lut(x)
    zero = x.zero_index()
    fillL = st["fillL"]
    flat = st.get("flat", False)
    sl, sT, sr, sb = st["sl"], st["st"], (0 if flat else max(st["sr"], 1)), (0 if flat else max(st["sb"], 1))
    out = []
    for code in codes:
        ch = chr(code)
        fnt = font
        gpx = px
        bb = fnt.getbbox(ch, anchor="ls")
        # fonts whose yoff is an unsigned offset from the top of the line cannot hold a glyph that
        # reaches above the line (hamza forms): render those a little smaller until the top fits
        while not st["signed"] and st["base"] + bb[1] - st["st"] - 1 < 0 and gpx > 8:
            gpx -= 1
            fnt = ImageFont.truetype(ttf, gpx, layout_engine=ImageFont.Layout.BASIC)
            bb = fnt.getbbox(ch, anchor="ls")
        adv = fnt.getlength(ch)
        if bb[2] - bb[0] <= 0 or bb[3] - bb[1] <= 0:
            continue
        x0, y0, x1, y1 = bb
        xl = min(0, x0)
        xr = max(int(np.ceil(adv)), x1)
        cw, chh = xr - xl, y1 - y0
        im = Image.new("L", (cw, chh), 0)
        ImageDraw.Draw(im).text((-xl, -y0), ch, font=fnt, fill=255, anchor="ls")
        s = np.asarray(im, dtype=np.float32) / 255.0
        h, w = s.shape
        full = np.zeros((h + sT + sb, w + sl + sr), dtype=np.float32)
        full[sT:sT + h, sl:sl + w] = s
        shadow = np.zeros_like(full)
        for dy in range(-sT, sb + 1):
            for dx in range(-sl, sr + 1):
                sh = np.zeros_like(full)
                ys = slice(max(dy, 0), full.shape[0] + min(dy, 0))
                yd = slice(max(-dy, 0), full.shape[0] + min(-dy, 0))
                xs = slice(max(dx, 0), full.shape[1] + min(dx, 0))
                xd = slice(max(-dx, 0), full.shape[1] + min(-dx, 0))
                sh[ys, xs] = full[yd, xd]
                shadow = np.maximum(shadow, sh)
        sm = Image.fromarray((shadow * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))
        shadow = np.asarray(sm, dtype=np.float32) / 255.0
        A = np.maximum(full, shadow)
        Lt = np.where(A > 0, fillL * full / np.maximum(A, 1e-6), 0.0)
        Ai = np.clip(np.rint(A * 255), 0, 255).astype(np.uint8)
        Li = np.clip(np.rint(Lt), 0, 255).astype(np.uint8)
        idx = lut[Li, Ai]
        idx[Ai == 0] = zero
        # crop empty rows
        rows = np.where((Ai > 0).any(axis=1))[0]
        if len(rows) == 0:
            continue
        r0, r1 = rows[0], rows[-1]
        idx = idx[r0:r1 + 1]
        top_rel_base = y0 - sT + int(r0)             # top row of the bitmap, relative to the baseline
        yoff = st["base"] + top_rel_base
        out.append(dict(code=code, idx=idx, adv=int(np.ceil(adv)), xoff=xl - sl, yoff=yoff))
    return out, px


def free_rows_start(x):
    ymax = 0
    for raw in x.rec_raw:
        f = rec_fields(x, raw)
        ymax = max(ymax, f["y"] + f["h"])
    return ymax


def occupancy(x, recs_fields, gap=1):
    occ = np.zeros((x.H, x.W), dtype=np.int32)
    for f in recs_fields:
        occ[max(f["y"] - gap, 0):f["y"] + f["h"] + gap, max(f["x"] - gap, 0):f["x"] + f["w"] + gap] = 1
    return occ


def first_fit(occ, w, h):
    """first (y, x) in raster order where a w x h rectangle fits on free (0) cells, or None"""
    H, W = occ.shape
    if w > W or h > H:
        return None
    S = np.zeros((H + 1, W + 1), dtype=np.int64)
    S[1:, 1:] = occ.cumsum(0).cumsum(1)
    sums = S[h:, w:] - S[:-h, w:] - S[h:, :-w] + S[:-h, :-w]
    ok = np.argwhere(sums == 0)
    if len(ok) == 0:
        return None
    y, xx = ok[0]
    return int(y), int(xx)


def pack(x, glyphs, keep_fields, gap=1, allow_grow=True):
    """place every glyph bitmap on free atlas cells (first fit, tallest first); grow the atlas only
    when allow_grow and it does not fit. keep_fields = fields of the records that stay."""
    order = sorted(glyphs, key=lambda g: (-g["idx"].shape[0], -g["idx"].shape[1], g["code"]))
    while True:
        occ = occupancy(x, keep_fields, gap=gap)
        place = []
        ok = True
        for g in order:
            h, w = g["idx"].shape
            pos = first_fit(occ, w + gap, h + gap)
            if pos is None:
                ok = False
                break
            py, px_ = pos
            occ[py:py + h + gap, px_:px_ + w + gap] = 1
            place.append((g, px_, py))
        if ok:
            break
        if not allow_grow:
            raise RuntimeError("atlas too small for the Arabic glyphs")
        x.resize_atlas(x.H * 2)
    for g, gx, gy in place:
        h, w = g["idx"].shape
        x.atlas[gy:gy + h, gx:gx + w] = g["idx"]
        g["x"], g["y"] = gx, gy
    return glyphs


# Latin-1 letters with accents (and the Latin Extended-A ones): not needed in an Arabic build, their atlas
# space is reused. The 0xD7 / 0xF7 signs and everything below 0xC0 stay.
DROP = [c for c in range(0xC0, 0x100) if c not in (0xD7, 0xF7)]


def add_alias(x, raws, alias):
    """alias {presentation form code: 8-bit code}: the movie subtitles (.loc) are 8-bit strings, one byte = one
    glyph code. An alias record is a copy of the form's record (same atlas rectangle, no new pixels) under the byte
    code; the glyph that sat there before goes. A record that owns kerning entries (last byte of the 12 byte
    records; the entries are consecutive in glyph order) keeps its place and its count: only its picture changes."""
    alias = dict(alias or {})
    byc = {}
    for r in raws:
        byc[struct.unpack_from("<H", r)[0]] = r
    # U+00A0 (the non-breaking space the subtitle chunks use) draws like a plain space, in the UTF-16 tables and
    # in the 8-bit .loc strings
    sp = bytearray(byc[0x20])
    byc[0xFFFF] = bytes(sp)
    alias[0xFFFF] = 0xA0
    gone = set(alias.values())
    old = {struct.unpack_from("<H", r)[0]: r for r in raws if struct.unpack_from("<H", r)[0] in gone}
    out = [r for r in raws if struct.unpack_from("<H", r)[0] not in gone]
    for form, code in alias.items():
        r = bytearray(byc[form])
        struct.pack_into("<H", r, 0, code)
        if x.rs == 12 and code in old:
            r[11] = old[code][11]
        out.append(bytes(r))
    out.sort(key=lambda r: struct.unpack_from("<H", r)[0])
    return out


def add_arabic(x, ttf, drop_accents=True, allow_grow=True, size_delta=0, alias=None):
    st = analyse(x)
    glyphs, px = render_bitmaps(x, ttf, st, size_delta)
    fields = [rec_fields(x, r) for r in x.rec_raw]
    if drop_accents:
        # 12 byte records: the last byte counts the kerning entries of the glyph, stored consecutively in
        # glyph order -- dropping a glyph that owns entries would shift every later one
        drop = {f["code"] for f in fields if f["code"] in DROP and f["k"] == 0}
        keep = [f for f in fields if f["code"] not in drop]
        kept_raw = [r for r, f in zip(x.rec_raw, fields) if f["code"] not in drop]
    else:
        keep, kept_raw = fields, list(x.rec_raw)
    have = {f["code"] for f in keep}
    glyphs = [g for g in glyphs if g["code"] not in have]
    # free the dropped glyphs' pixels
    if drop_accents:
        zero = x.zero_index()
        for f in fields:
            if f["code"] in drop:
                x.atlas[f["y"]:f["y"] + f["h"], f["x"]:f["x"] + f["w"]] = zero
    pack(x, glyphs, keep, allow_grow=allow_grow)
    raws = list(kept_raw)
    for g in glyphs:
        h, w = g["idx"].shape
        if g["yoff"] < 0 and not st["signed"]:
            g["yoff"] = 0
        raws.append(make_rec(x, g["code"], w, h, g["x"], g["y"], g["adv"], g["xoff"], g["yoff"]))
    raws.sort(key=lambda r: struct.unpack_from("<H", r)[0])
    raws = add_alias(x, raws, alias)
    x.rec_raw = raws
    x.n = len(raws)
    return dict(px=px, added=len(glyphs), dropped=len(fields) - len(keep), atlasH=x.H, **{k: v for k, v in st.items() if k != "H"})


def atlas_png(x, path):
    rgba = np.zeros((x.H, x.W, 4), dtype=np.uint8)
    p = x.pal[x.atlas]
    # composite over mid grey so black shadows and white fills are both visible
    bg = 90.0
    a = p[..., 3] / 255.0
    for c in range(3):
        rgba[..., c] = np.clip(p[..., 0] * a + bg * (1 - a), 0, 255).astype(np.uint8)
    rgba[..., 3] = 255
    Image.fromarray(rgba).save(path)


def add_arabic_auto(x, ttf, alias=None):
    """try the natural size first, then smaller ones, without growing the atlas; grow as a last resort"""
    import copy
    for delta in (0, -1, -2, -3):
        y = copy.deepcopy(x)
        try:
            info = add_arabic(y, ttf, allow_grow=False, size_delta=delta, alias=alias)
        except RuntimeError as e:
            if "alias" in str(e):
                raise
            continue
        x.__dict__.update(y.__dict__)
        info["delta"] = delta
        return info
    info = add_arabic(x, ttf, allow_grow=True, size_delta=-1, alias=alias)
    info["delta"] = -1
    return info
