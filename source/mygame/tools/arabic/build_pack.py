"""Build the Arabic mod pack:  python build_pack.py [out_dir]

  fonts    realfont.viv / realftFE.viv with the Arabic presentation forms added (arfont.py)
  strings  every strings.viv / stringMP.viv of the game with the <english> slot replaced by the shaped,
           visually ordered Arabic of tools/arabic/ar/<table>.json (keys without a translation stay English)

The pack goes to mods/Arabic/DATA/... next to the exe; removing the folder restores the game.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vivlib import read_viv, build_viv
from xfnlib import Xfn
import arfont
import arshape

GAME = os.path.normpath(os.path.join(HERE, "..", "..", "game", "DATA"))
_args = [a for a in sys.argv[1:] if not a.startswith("--")]
OUT = _args[0] if _args else os.path.normpath(os.path.join(HERE, "..", "..", "languages", "arabic"))
TTF = os.environ.get("AR_FONT", r"C:\Windows\Fonts\tahomabd.ttf")
SKIP_FONTS = set()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def load_alias():
    """{presentation form: 8-bit code} written by build_loc.py for the movie subtitles"""
    p = os.path.join(HERE, "ar", "loc_alias.json")
    if not os.path.exists(p):
        return None
    return {int(k, 16): v for k, v in json.load(open(p, encoding="utf-8")).items()}


def build_fonts():
    alias = load_alias()
    for v in ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv"):
        src = os.path.join(GAME, v)
        b, ents, hdr_len, end, files = read_viv(src)
        new = []
        for name, d in files:
            if name.endswith(".xfn") and name not in SKIP_FONTS:
                x = Xfn(d)
                info = arfont.add_arabic_auto(x, TTF, alias)
                d = x.serialize()
                print("  font %-40s %6d bytes  px %d  atlas %dx%d  +%d glyphs" % (name, len(d), info["px"], x.W, x.H, info["added"]))
            new.append((name, d))
        write(os.path.join(OUT, "DATA", v), build_viv(new, (b, ents, hdr_len, end)))


VAL = re.compile(r'(<english value=")([^"]*)(")')
BLOCK = re.compile(r'(<(?:global|local)string name=")([^"]*)(">)(.*?)(</(?:global|local)string>)', re.S)


def is_dialogue(key):
    """voice-line subtitles (numbered keys, squad callouts, level event lines): they go through visual_chunks"""
    return key[:1].isdigit() or key.startswith(("RU_", "UK_OFC_", "BombingRun", "TankDestroysWall"))


def tr_file(rel):
    return os.path.join(HERE, "ar", rel.replace("/", "__").replace(".viv", "") + ".json")


def load_tr(rel):
    tp = tr_file(rel)
    tr = {}
    for pp in sorted(glob.glob(tp[:-5] + ".json") + glob.glob(tp[:-5] + ".*.json")):
        tr.update(json.load(open(pp, encoding="utf-8")))
    return tr


def shared_translations():
    """(key, english text) -> Arabic over every table, so a string translated once is reused wherever the same
    key carries the same English text (stringMP and the level tables repeat many global strings)"""
    out = {}
    for dp, dn, fn in os.walk(GAME):
        for f in fn:
            if f in ("strings.viv", "stringMP.viv"):
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, GAME).replace("\\", "/")
                tr = load_tr(rel)
                if not tr:
                    continue
                text = read_viv(p)[4][0][1].decode("utf-16")
                for m in BLOCK.finditer(text):
                    key = m.group(2)
                    v = VAL.search(m.group(4))
                    if key in tr and tr[key] and v:
                        out.setdefault((key, v.group(2)), tr[key])
    return out


def build_strings():
    total = done = 0
    shared = shared_translations()
    for dp, dn, fn in os.walk(GAME):
        for f in fn:
            if f not in ("strings.viv", "stringMP.viv"):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, GAME).replace("\\", "/")
            tp = tr_file(rel)
            tr = load_tr(rel)
            b, ents, hdr_len, end, files = read_viv(p)
            name, data = files[0]
            text = data.decode("utf-16")
            cnt = [0, 0]

            def blk(m):
                key = m.group(2)
                cnt[0] += 1
                v = VAL.search(m.group(4))
                ar = tr.get(key) or (shared.get((key, v.group(2))) if v else None)
                if not ar:
                    return m.group(0)
                cnt[1] += 1
                vis = arshape.visual_chunks(ar) if is_dialogue(key) else arshape.visual(ar)
                return m.group(1) + key + m.group(3) + VAL.sub(lambda mm: mm.group(1) + vis + mm.group(3), m.group(4), count=1) + m.group(5)

            text2 = BLOCK.sub(blk, text)
            out = b"\xff\xfe" + text2.encode("utf-16-le")
            write(os.path.join(OUT, "DATA", rel), build_viv([(name, out)], (b, ents, hdr_len, end)))
            total += cnt[0]
            done += cnt[1]
            print("  strings %-28s %4d keys, %4d translated" % (rel, cnt[0], cnt[1]))
    print("  translated %d of %d keys in the tables that have a translation file" % (done, total))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    print("pack ->", OUT)
    import build_loc                  # movie subtitles first: it decides the alias codes the fonts need
    build_loc.OUT = OUT
    build_loc.main()
    if "--no-fonts" not in sys.argv:
        build_fonts()
    build_strings()
