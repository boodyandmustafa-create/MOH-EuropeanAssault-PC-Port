import struct, sys, os, collections, zlib
from walk import walk
from closure import load, closure
from rebase import rebase_body, PTR_TYPES
def merge(target, donor, names):
    td, tp, tr, tend = load(target)
    dd, dp, dr, dend = load(donor)
    tnames = {(r[1], r[2]) for r in tr}
    want = []
    seen = set()
    for n in names:
        for r in closure(dd, dr, n):
            if r[0] in seen or (r[1], r[2]) in tnames:
                continue
            seen.add(r[0]); want.append(r)
    want.sort()
    types = dict(struct.iter_unpack("<II", td[24:24 + 8 * struct.unpack_from("<I", td, 20)[0]]))
    cnt = collections.Counter(r[2] for r in want)
    for t in cnt:
        if t not in types:
            raise SystemExit("type %08x missing in target" % t)
    n = struct.unpack_from("<I", td, 20)[0]
    out = bytearray(td)
    for i in range(n):
        h, c = struct.unpack_from("<II", out, 24 + 8 * i)
        if h in cnt:
            struct.pack_into("<II", out, 24 + 8 * i, h, c + cnt[h])
    parts = []
    for r in want:
        rec = dd[r[0]:r[0] + r[3]]
        if r[2] in PTR_TYPES:
            body, c = rebase_body(rec[16:])
            rec = rec[:16] + body
            print("rebased %d ptrs in %08x type %08x" % (c, r[1], r[2]))
        parts.append(rec)
    add = b"".join(parts)
    out += add
    bm = os.environ.get("BANKMERGE")           # BANKMERGE=<target bank name>:<donor bank name>:<new name> (hex): clips the added anims use, merged into a new bank
    if bm:
        import bankmerge
        tb, db, nn = (int(x, 16) for x in bm.split(":"))
        r_, b_, ttab_, te_, tcl = bankmerge.load_bank(td, tr, tb)
        r2_, b2_, dtab_, de_, dcl = bankmerge.load_bank(dd, dr, db)
        keys = set()
        for r in want:
            if r[2] != 0x4f888d9c:
                continue
            body = dd[r[0] + 16:r[0] + r[3]]
            for (w,) in struct.iter_unpack("<I", body[:len(body) // 4 * 4]):
                if w in dcl and w not in tcl:
                    keys.add(w)
        rec, added, st = bankmerge.build(target, tb, donor, db, sorted(keys), nn)
        print("BANKMERGE: %d anim clip keys wanted, %d clips added (closure), record %d bytes, ptr words %s" % (len(keys), len(added), len(rec), st))
        if os.environ.get("BANKINPLACE"):       # same name, same position in the file (references only bind to records already loaded)
            r0 = [x for x in tr if x[1] == tb][0]
            out = out[:r0[0]] + bytearray(rec) + out[r0[0] + r0[3]:]
            print("BANKINPLACE: bank %08x replaced at file offset %x, %d -> %d bytes" % (tb, r0[0], r0[3], len(rec)))
        else:
            out += rec
            for i in range(n):
                h, c = struct.unpack_from("<II", out, 24 + 8 * i)
                if h == 0x7c5686d5:
                    struct.pack_into("<II", out, 24 + 8 * i, h, c + 1)
    struct.pack_into("<II", out, 8, len(out), len(out))
    swap = os.environ.get("BANKSWAP")          # BANKSWAP=<old bank name>:<new bank name> (hex): the level manifest (type 26ed4dc8) names its animation bank
    if swap:
        ob, nb = (int(x, 16) for x in swap.split(":"))
        pat = struct.pack("<I", ob)
        for r in tr:
            if r[2] == 0x26ed4dc8:
                body = bytes(out[r[0] + 16:r[0] + r[3]])
                i = body.find(pat)
                if i >= 0:
                    struct.pack_into("<I", out, r[0] + 16 + i, nb)
                    print("BANKSWAP: manifest %08x +0x%x %08x -> %08x" % (r[1], i, ob, nb))
    return bytes(out), want
if __name__ == "__main__":
    target, donor = sys.argv[1], sys.argv[2]
    names = [int(x, 16) if x.startswith("0x") else zlib.crc32(x.encode()) & 0xffffffff for x in sys.argv[3:]]  # 0x... = raw record name hash (e.g. an animation bank)
    data, want = merge(target, donor, names)
    outp = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\mods\levelpatch"
    os.makedirs(outp, exist_ok=True)
    fn = os.path.join(outp, "%s_level_a.str" % target)
    open(fn, "wb").write(data)
    print("wrote", fn, len(data), "added", len(want), "records", sum(r[3] for r in want), "bytes")
    # self check
    from walk import walk as w
    d, pairs, recs, end = w(fn)
    print("walk", len(recs), sum(c for h, c in pairs), end == len(d))
