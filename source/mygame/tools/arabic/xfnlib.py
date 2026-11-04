"""FntX (.xfn) font reader/writer for the EA fonts of MOH European Assault (P8 swizzled atlas flavour,
the ones inside realfont.viv / realftFE.viv).

File layout (all little endian):
  0x00  'FntX'
  0x04  u32  atlas_end + 96
  0x0a  u16  glyph count n
  0x0e  u16  flags (bit 0x0c set -> 16 byte glyph records, else 12 byte)
  0x14  u32  0x80 (glyph table offset)
  0x18  u32  start of the kerning block (0 when the font has none)
  0x1c  u32  texture header + 0x20
  0x30  u32  texture header offset
  0x80  glyph records (sorted by code)
        12 byte: code u16, w u8, h u8, x u16, y u16, adv u8, xoff s8, yoff u8, kern u8
  ....  kerning block + 'Buy ERTS' + zero padding so that the atlas starts on a 0x80 boundary
  tex   0x30 byte texture header (width at +0x18, height at +0x1c) then the swizzled P8 atlas
  post  1184 bytes: palette header, 256 x RGBA palette, EAGL64 footer
"""
import struct
import numpy as np
from swz import unswizzle, swizzle

REC12 = struct.Struct("<HBBHHBbBB")


class Xfn:
    def __init__(self, data):
        b = bytes(data)
        assert b[:4] == b"FntX"
        u32 = lambda o: struct.unpack_from("<I", b, o)[0]
        u16 = lambda o: struct.unpack_from("<H", b, o)[0]
        self.hdr = bytearray(b[:0x80])
        self.n = u16(0x0a)
        kern = u32(0x18)
        self.tex_hdr = u32(0x30)
        # record size: 12 unless the flags say 16
        self.rs = 16 if (u16(0x0e) & 0x0c) else 12
        self.rec_raw = [b[0x80 + i * self.rs:0x80 + (i + 1) * self.rs] for i in range(self.n)]
        mid_start = 0x80 + self.n * self.rs
        mid = b[mid_start:self.tex_hdr]
        k = mid.find(b"Buy ERTS")
        assert k >= 0, "no ERTS marker"
        self.mid = mid[:k + 8]
        self.has_kern = kern != 0
        self.texhdr = bytearray(b[self.tex_hdr:self.tex_hdr + 0x30])
        self.W = u32(self.tex_hdr + 0x18)
        self.H = u32(self.tex_hdr + 0x1c)
        st = self.tex_hdr + 0x30
        self.atlas = unswizzle(b[st:st + self.W * self.H], self.W, self.H).copy()
        self.post = b[st + self.W * self.H:]
        pal = self.post[0x40:0x440]
        self.pal = np.frombuffer(pal, dtype=np.uint8).reshape(256, 4).astype(int)

    # ---- records (12 byte flavour) ----
    def recs(self):
        assert self.rs == 12
        return [REC12.unpack(r) for r in self.rec_raw]

    def set_recs(self, recs):
        assert self.rs == 12
        recs = sorted(recs, key=lambda r: r[0])
        self.rec_raw = [REC12.pack(*r) for r in recs]
        self.n = len(recs)

    def serialize(self):
        n = self.n
        mid_start = 0x80 + n * self.rs
        need = mid_start + len(self.mid)
        t = need
        while (t + 0x30) % 0x80:
            t += 1
        hdr = bytearray(self.hdr)
        struct.pack_into("<H", hdr, 0x0a, n)
        struct.pack_into("<I", hdr, 0x18, mid_start if self.has_kern else 0)
        struct.pack_into("<I", hdr, 0x1c, t + 0x20)
        struct.pack_into("<I", hdr, 0x30, t)
        atlas_end = t + 0x30 + self.W * self.H
        struct.pack_into("<I", hdr, 4, atlas_end + 96)
        texhdr = bytearray(self.texhdr)
        struct.pack_into("<I", texhdr, 0x18, self.W)
        struct.pack_into("<I", texhdr, 0x1c, self.H)
        out = bytearray(hdr)
        for r in self.rec_raw:
            out += r
        out += self.mid
        out += b"\x00" * (t - len(out))
        out += texhdr
        out += swizzle(self.atlas.reshape(-1), self.W, self.H)
        out += self.post
        return bytes(out)

    def resize_atlas(self, newH):
        """Grow (only) the atlas downwards to newH rows (power of two) and fix the size fields in the
        texture header: tex[1] = data+0x40, tex[3] = data, tail word0 = W*H/256, word2 = H."""
        assert newH >= self.H and (newH & (newH - 1)) == 0
        if newH == self.H:
            return
        pad = np.zeros((newH - self.H, self.W), dtype=self.atlas.dtype) + self.zero_index()
        self.atlas = np.vstack([self.atlas, pad])
        self.H = newH
        size = self.W * self.H
        struct.pack_into("<I", self.texhdr, 4, size + 0x40)
        struct.pack_into("<I", self.texhdr, 0xc, size)
        # tail words after the 0x20 tex header: u16 fmt, u16 W*H/256, u16 W, u16 H
        struct.pack_into("<H", self.texhdr, 0x22, (self.W * self.H) // 256)
        struct.pack_into("<H", self.texhdr, 0x24, self.W)
        struct.pack_into("<H", self.texhdr, 0x26, self.H)

    def zero_index(self):
        """palette index of a fully transparent texel (most common value in an empty atlas row)"""
        a = self.pal[:, 3]
        cand = [i for i in range(256) if a[i] == 0]
        # prefer the index the original atlas uses for empty space
        vals, cnt = np.unique(self.atlas, return_counts=True)
        best = None
        for v, c in sorted(zip(vals, cnt), key=lambda t: -t[1]):
            if a[v] == 0:
                best = int(v); break
        return best if best is not None else (cand[0] if cand else 0)


if __name__ == "__main__":
    import os, sys
    from vivlib import read_viv
    root = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
    bad = 0
    for v in ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv"):
        _, ents, _, _, fl = read_viv(os.path.join(root, v))
        for name, d in fl:
            x = Xfn(d)
            ok = x.serialize() == d
            bad += (not ok)
            print("OK " if ok else "BAD", v.split("/")[-1], name, "rs", x.rs, "n", x.n, x.W, x.H)
    print("bad:", bad)
