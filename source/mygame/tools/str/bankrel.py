import struct, bisect, collections
from walk import walk
B = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
def lv(l):
    return walk(B + "\%s\%s\level_a.str" % (l.split("_")[0], l))
def bank(d, rr, h):
    r = [x for x in rr if x[1] == h][0]; b = d[r[0]+16:r[0]+r[3]]
    n = struct.unpack_from("<I", b, 0x18)[0]
    tab = [struct.unpack_from("<II", b, 0x1c + 8*i) for i in range(n)]
    return b, tab
def spans(b, tab):
    return [(o, tab[i+1][1] if i+1 < len(tab) else len(b) - 0x10) for i, (k, o) in enumerate(tab)]
def reloc_clip(clip, src_tab, src_spans, dst_off_of_key):
    """words inside [src data start, src end) -> same position inside the clip that contains them, in the dst bank"""
    starts = [s for s, e in src_spans]; keys = [k for k, o in src_tab]
    n = len(clip) // 4; ws = list(struct.unpack("<%dI" % n, clip[:n*4])); miss = []
    lo, hi = src_spans[0][0], src_spans[-1][1]
    for i, w in enumerate(ws):
        if lo <= w < hi:
            j = bisect.bisect_right(starts, w) - 1
            k = keys[j]
            if k in dst_off_of_key:
                ws[i] = dst_off_of_key[k] + (w - starts[j])
            else:
                miss.append((i * 4, w, k))
    return struct.pack("<%dI" % n, *ws) + clip[n*4:], miss
