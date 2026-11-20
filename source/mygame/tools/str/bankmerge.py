"""Build a merged animation bank: the target level's own bank + the clips a donor-level weapon needs.
Bank body = [1][name][9][size][0][0][count][(key,offset)*count sorted][clips]; offsets are relative to body+0x10 and the
clip header table (words 0xC,0x10 and 8*N bytes from 0x18, N = dword at 0x14) holds bank-absolute offsets that must be
re-based when clips move (see memory moh-level-str-format)."""
import struct, bisect
from walk import walk
BASE = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
def lv(l):
    return walk(BASE + "\%s\%s\level_a.str" % (l.split("_")[0], l))
def load_bank(d, rr, h):
    r = [x for x in rr if x[1] == h][0]
    b = d[r[0] + 16:r[0] + r[3]]
    n = struct.unpack_from("<I", b, 0x18)[0]
    tab = [struct.unpack_from("<II", b, 0x1c + 8 * i) for i in range(n)]
    ends = [tab[i + 1][1] if i + 1 < n else len(b) - 0x10 for i in range(n)]
    clips = {k: b[o + 0x10:e + 0x10] for (k, o), e in zip(tab, ends)}
    return r, b, tab, ends, clips
def ptr_positions(clip):
    n = struct.unpack_from("<I", clip, 0x14)[0]
    if n == 0 or n > 400 or 0x18 + 8 * n > len(clip):
        return []
    return [0xc, 0x10] + list(range(0x18, 0x18 + 8 * n, 4))
def build(target, tbank, donor, dbank, want_keys, new_name):
    td, tp, tr, _ = lv(target); dd, dp, dr, _ = lv(donor)
    tr_, tb, ttab, tends, tclips = load_bank(td, tr, tbank)
    dr_, db, dtab, dends, dclips = load_bank(dd, dr, dbank)
    dstart = {k: o for k, o in dtab}
    dspan = [(o, e) for (k, o), e in zip(dtab, dends)]
    dkeys = [k for k, o in dtab]; dstarts = [o for k, o in dtab]
    # closure over donor clips: pointers landing in a donor-only clip pull that clip in too
    add = []
    todo = list(want_keys)
    while todo:
        k = todo.pop()
        if k in tclips or k in add or k not in dclips:
            continue
        add.append(k)
        c = dclips[k]
        for p in ptr_positions(c):
            w = struct.unpack_from("<I", c, p)[0]
            if dstarts[0] <= w < dspan[-1][1]:
                j = bisect.bisect_right(dstarts, w) - 1
                todo.append(dkeys[j])
    keys = sorted(set(tclips) | set(add))
    n = len(keys)
    data_start = 0x1c + 8 * n - 0x10
    newoff = {}
    off = data_start
    for k in keys:
        newoff[k] = off
        off += len(tclips[k]) if k in tclips else len(dclips[k])
    tstarts = [o for k, o in ttab]; tkeys = [k for k, o in ttab]
    def remap(w, src):
        if src == "t":
            starts, kk, lo, hi = tstarts, tkeys, tstarts[0], tends[-1]
        else:
            starts, kk, lo, hi = dstarts, dkeys, dstarts[0], dspan[-1][1]
        if not (lo <= w < hi):
            return w
        j = bisect.bisect_right(starts, w) - 1
        k = kk[j]
        if src == "d" and k not in add and k in tclips:      # shared clip: same inner position in the target's own copy
            return newoff[k] + (w - starts[j])
        return newoff[k] + (w - starts[j])
    out_clips = []
    stats = {"t_ptrs": 0, "d_ptrs": 0}
    for k in keys:
        src = "t" if k in tclips else "d"
        c = bytearray(tclips[k] if src == "t" else dclips[k])
        for p in ptr_positions(bytes(c)):
            w = struct.unpack_from("<I", c, p)[0]
            nw = remap(w, src)
            if nw != w:
                struct.pack_into("<I", c, p, nw & 0xffffffff)
            stats[src + "_ptrs"] += 1
        out_clips.append(bytes(c))
    body = bytearray()
    body += struct.pack("<7I", 1, new_name, 9, 0, 0, 0, n)
    for k in keys:
        body += struct.pack("<II", k, newoff[k])
    assert len(body) - 0x10 == data_start + 0, (len(body), data_start)
    for c in out_clips:
        body += c
    struct.pack_into("<I", body, 0xc, len(body) - 0x10)
    rec = struct.pack("<IIII", new_name, 0x7c5686d5, 16 + len(body), tr_[4]) + bytes(body)
    return rec, add, stats
