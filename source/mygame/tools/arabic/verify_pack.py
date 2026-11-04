"""Sanity checks of mods/Arabic against the game files."""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vivlib import read_viv
from xfnlib import Xfn

GAME = os.path.normpath(os.path.join(HERE, "..", "..", "game", "DATA"))
PACK = os.path.normpath(os.path.join(HERE, "..", "..", "languages", "arabic", "DATA"))
sys.stdout.reconfigure(encoding="utf-8")
bad = 0
for dp, dn, fn in os.walk(PACK):
    for f in fn:
        p = os.path.join(dp, f)
        rel = os.path.relpath(p, PACK)
        q = os.path.join(GAME, rel)
        if f.lower().endswith(".loc"):
            import struct
            import build_loc
            h1, n1, b1 = build_loc.parse(open(p, "rb").read())
            h0, n0, b0 = build_loc.parse(open(q, "rb").read())
            assert n1 == n0 and len(b1) == len(b0), rel
            assert build_loc.assemble(h1, b1) == open(p, "rb").read(), rel
            for i, (x1, x0) in enumerate(zip(b1, b0)):
                assert x1["ids"] == x0["ids"] and x1["f2"] == x0["f2"] and len(x1["strs"]) == len(x0["strs"]), (rel, i)
                if i > 0:
                    assert x1["strs"] == x0["strs"], (rel, i)
            changed = sum(1 for a, b in zip(b1[0]["strs"], b0[0]["strs"]) if a != b)
            for a, b in zip(b1[0]["strs"], b0[0]["strs"]):
                if a != b:
                    assert a.count(b" ") <= 1, (rel, a)                 # at most one break opportunity (two chunks)
            hi = {c for s in b1[0]["strs"] for c in s if c >= 0x80}
            print("loc ok", rel, "blocks", n1, "block0 entries changed", changed, "alias codes used", len(hi))
            continue
        _, e1, _, _, f1 = read_viv(p)
        _, e0, _, _, f0 = read_viv(q)
        assert [n for n, _ in f1] == [n for n, _ in f0], rel
        for (n, d1), (_, d0) in zip(f1, f0):
            if n.endswith(".xfn"):
                x1, x0 = Xfn(d1), Xfn(d0)
                assert x1.serialize() == d1
                codes0 = {int.from_bytes(r[:2], "little") for r in x0.rec_raw}
                codes1 = {int.from_bytes(r[:2], "little") for r in x1.rec_raw}
                lost = sorted(codes0 - codes1)
                assert all(0xA0 <= c < 0x100 for c in lost), lost            # accents + the Latin-1 symbols (alias codes of the movie subtitles)
                byc = {int.from_bytes(r[:2], "little"): r for r in x1.rec_raw}
                assert byc[0xA0][2:11] == byc[0x20][2:11] and byc[0xA0][12:] == byc[0x20][12:], (n, "NBSP record")
                al = os.path.join(HERE, "ar", "loc_alias.json")
                if os.path.exists(al):
                    import json
                    by = {int.from_bytes(r[:2], "little"): r for r in x1.rec_raw}
                    assert 0xA0 not in json.load(open(al)).values()
                    for form, code in json.load(open(al)).items():
                        a, b_ = by[int(form, 16)], by[code]
                        assert a[2:11] == b_[2:11] and a[12:] == b_[12:], (n, form, code)           # same picture + metrics (k byte stays)
                print("font ok", rel, n, "n", x0.n, "->", x1.n, "atlas", x0.W, x0.H, "->", x1.W, x1.H)
            else:
                t0 = d0.decode("utf-16")
                t1 = d1.decode("utf-16")
                k0 = re.findall(r'string name="([^"]*)"', t0)
                k1 = re.findall(r'string name="([^"]*)"', t1)
                assert k0 == k1, rel
                import xml.dom.minidom as md
                try:
                    md.parseString(t1.encode("utf-8").replace(b'encoding="utf-16"', b'encoding="utf-8"'))
                    ok = "xml ok"
                except Exception as ex:
                    ok = "XML ERROR %s" % ex
                    bad += 1
                print("strings", rel, len(k1), "keys", ok, len(d0), "->", len(d1))
print("problems:", bad)
