import struct, sys, glob, os
def walk(path, verbose=False):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 20)[0]
    pairs = list(struct.iter_unpack("<II", d[24:24 + 8 * n]))
    types = {h: c for h, c in pairs}
    off = 24 + 8 * n
    recs = []
    while off + 16 <= len(d):
        name, typ, size, tag = struct.unpack_from("<IIII", d, off)
        if size < 16 or off + size > len(d) or typ not in types:
            break
        recs.append((off, name, typ, size, tag))
        off += size
    return d, pairs, recs, off
if __name__ == "__main__":
    base = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
    for p in sorted(glob.glob(base + r"\*\*\level_a.str")):
        d, pairs, recs, end = walk(p)
        tot = sum(c for h, c in pairs)
        print(os.path.relpath(p, base), "len", len(d), "recs", len(recs), "of", tot, "end", hex(end), "left", len(d) - end)
