"""Movie subtitles: DATA/SHARED/MOVIES/LOC/<movie>.loc  ->  languages/arabic/DATA/SHARED/MOVIES/LOC/<movie>.loc

LOC layout (little endian):  "LOCHD\\0\\0\\0" u32 0, u32 n(13), n * u32 block offsets, then n blocks
  block = "LOCL", u32 block length, u32 0, u32 count, count * u32 frame ids, count NUL-terminated strings, zero padding to 4
Block 0 is the English one. Its strings are 8-bit: one byte = one glyph code. Arabic text is therefore stored as
alias codes 0x80..0xFF; every used presentation form gets one alias (ar/loc_alias.json) and build_pack adds the
alias glyph records to the fonts (arfont.add_arabic alias=).
Translations: ar/movies.json {movie: {entry index: Arabic}}.   python build_loc.py [out_dir]
"""
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arshape

GAME = os.path.normpath(os.path.join(HERE, "..", "..", "game", "DATA", "SHARED", "MOVIES", "LOC"))
_args = [a for a in sys.argv[1:] if not a.startswith("--")]
OUT = _args[0] if _args else os.path.normpath(os.path.join(HERE, "..", "..", "languages", "arabic"))
WRAP = 44          # characters per subtitle line (one line = about 400 px; the subtitle box is 530 px)
ALIAS_FILE = os.path.join(HERE, "ar", "loc_alias.json")


def parse(d):
    assert d[:5] == b"LOCHD"
    n = struct.unpack_from("<I", d, 12)[0]
    offs = list(struct.unpack_from("<%dI" % n, d, 16))
    ends = offs[1:] + [len(d)]
    blocks = []
    for o, e in zip(offs, ends):
        assert d[o:o + 4] == b"LOCL"
        size, f2, cnt = struct.unpack_from("<III", d, o + 4)
        assert size == e - o
        ids = list(struct.unpack_from("<%dI" % cnt, d, o + 16))
        body = d[o + 16 + 4 * cnt:e]
        strs = body.split(b"\0")[:cnt]
        blocks.append(dict(f2=f2, ids=ids, strs=strs))
    return d[:16], n, blocks


def build_block(b):
    cnt = len(b["ids"])
    body = b"".join(s + b"\0" for s in b["strs"])
    body += b"\0" * (-len(body) % 4)
    size = 16 + 4 * cnt + len(body)
    return b"LOCL" + struct.pack("<III", size, b["f2"], cnt) + struct.pack("<%dI" % cnt, *b["ids"]) + body


def assemble(head, blocks):
    parts = [build_block(b) for b in blocks]
    n = len(parts)
    hdr_len = 16 + 4 * n
    offs, o = [], hdr_len
    for p in parts:
        offs.append(o)
        o += len(p)
    return head[:12] + struct.pack("<I", n) + struct.pack("<%dI" % n, *offs) + b"".join(parts)


def wrap_logical(text, width=WRAP):
    """greedy word wrap of the logical (not yet shaped) text; the pieces keep the reading order top to bottom"""
    words = text.split(" ")
    lines, cur = [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w) if cur else w
    if cur:
        lines.append(cur)
    return lines


MOVIE_CHUNK = 70   # chars (about 550 units): only entries longer than this are cut into two chunks = two lines


def shape(text):
    # No 0x0A line breaks. The engine keeps just the LAST wrapped line of a movie subtitle and wraps at plain
    # spaces, so every entry is one unbreakable chunk (NBSP = 0xA0 inside, drawn as a space by the fonts);
    # only entries longer than MOVIE_CHUNK become two chunks.
    return arshape.visual_chunks(text.strip(), MOVIE_CHUNK)


def build_alias(shaped_all):
    use = {}
    for s in shaped_all:
        for c in s:
            if ord(c) >= 0x80 and c != arshape.NBSP:
                use[c] = use.get(c, 0) + 1
    chars = sorted(use, key=lambda c: (use[c], ord(c)))        # rarest first -> lowest codes (0x80..0x9F are unproven)
    slots = [c for c in range(0x80, 0x100) if c != 0xA0]       # 0xA0 stays the NBSP
    if len(chars) > len(slots):
        raise SystemExit("too many distinct Arabic forms for the 0x80..0xFF alias space: %d" % len(chars))
    return {c: slots[i] for i, c in enumerate(chars)}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    tr = json.load(open(os.path.join(HERE, "ar", "movies.json"), encoding="utf-8"))
    files = {os.path.basename(p)[:-4]: p for p in glob.glob(os.path.join(GAME, "*.loc"))}
    # 1. round trip must be byte exact
    for name, p in files.items():
        d = open(p, "rb").read()
        head, n, blocks = parse(d)
        assert assemble(head, blocks) == d, "round trip differs: " + name
    # 2. shape
    shaped = {}
    for name, ents in tr.items():
        for idx, t in ents.items():
            shaped[(name, int(idx))] = shape(t)
    alias = build_alias(shaped.values())
    json.dump({"%04X" % ord(c): v for c, v in alias.items()}, open(ALIAS_FILE, "w", encoding="utf-8"), indent=0)
    print("  loc: %d distinct forms -> alias codes 0x80..0x%02X" % (len(alias), max(alias.values())))
    # 3. write
    odir = os.path.join(OUT, "DATA", "SHARED", "MOVIES", "LOC")
    os.makedirs(odir, exist_ok=True)
    for name, p in sorted(files.items()):
        d = open(p, "rb").read()
        head, n, blocks = parse(d)
        b0 = blocks[0]
        cnt = 0
        for (nm, idx), s in shaped.items():
            if nm != name:
                continue
            assert idx < len(b0["strs"]) and b0["strs"][idx].strip(b" ") != b"", "entry %s:%d is empty in the game file" % (name, idx)
            raw = bytearray()
            for c in s:
                o = ord(c)
                raw.append(o if (o < 0x80 or c == arshape.NBSP) else alias[c])
            b0["strs"][idx] = bytes(raw)
            cnt += 1
        out = assemble(head, blocks)
        open(os.path.join(odir, os.path.basename(p)), "wb").write(out)
        print("  loc %-10s %2d entries, %5d -> %5d bytes" % (name, cnt, len(d), len(out)))


if __name__ == "__main__":
    main()
