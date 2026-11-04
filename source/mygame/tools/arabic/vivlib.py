import struct, os, sys


def parse_viv(b):
    """C0FB archive: c0fb, u16 BE header length, u16 BE count, then count entries of
    offset(u24 BE) size(u24 BE) name\\0. Returns (entries, hdr_len) with entries = [(name, offset, size)]."""
    assert b[:2] == b"\xc0\xfb", "not a C0FB archive"
    hdr_len, count = struct.unpack(">HH", b[2:6])
    p = 6
    out = []
    for _ in range(count):
        off = int.from_bytes(b[p:p + 3], "big")
        size = int.from_bytes(b[p + 3:p + 6], "big")
        p += 6
        e = b.index(b"\x00", p)
        name = b[p:e].decode("latin1")
        p = e + 1
        out.append((name, off, size))
    return out, hdr_len, p


def read_viv(path):
    b = open(path, "rb").read()
    ents, hdr_len, end = parse_viv(b)
    files = [(n, b[o:o + s]) for n, o, s in ents]
    return b, ents, hdr_len, end, files


def build_viv(files, orig=None):
    """files = [(name, bytes)]. If orig=(b, ents, hdr_len, end) is given, the layout mimics it:
    same header length field, data start alignment guessed from the original."""
    count = len(files)
    names_len = sum(len(n) + 1 for n, _ in files)
    hdr_end = 6 + 6 * count + names_len
    hdr_len = hdr_end if orig is None else orig[2]
    # data start: original first-offset when it fits, else aligned up to 8
    if orig is not None:
        first = orig[1][0][1]
        start = first
        if hdr_end > start:
            start = (hdr_end + 7) & ~7
    else:
        start = hdr_end
    blob = bytearray()
    entries = []
    pos = start
    for i, (n, d) in enumerate(files):
        entries.append((n, pos, len(d)))
        blob += d
        pos += len(d)
        if orig is not None and i + 1 < len(orig[1]):
            # padding between files in the original = next.off - (off+size)
            on, oo, os_ = orig[1][i]
            nxt = orig[1][i + 1][1]
            pad = nxt - (oo + os_)
            if pad > 0:
                blob += b"\x00" * pad
                pos += pad
    h = bytearray(b"\xc0\xfb" + struct.pack(">HH", hdr_len, count))
    for n, o, s in entries:
        h += o.to_bytes(3, "big") + s.to_bytes(3, "big") + n.encode("latin1") + b"\x00"
    h += b"\x00" * (start - len(h))
    return bytes(h + blob)


if __name__ == "__main__":
    root = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
    bad = 0
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if not f.lower().endswith(".viv"):
                continue
            p = os.path.join(dp, f)
            b = open(p, "rb").read()
            if b[:2] != b"\xc0\xfb":
                print("SKIP", os.path.relpath(p, root), b[:4]); continue
            _, ents, hdr_len, end, files = read_viv(p)
            nb = build_viv(files, (b, ents, hdr_len, end))
            ok = nb == b
            if not ok: bad += 1
            print("OK " if ok else "BAD", os.path.relpath(p, root), len(b), len(nb), [(n, o, s) for n, o, s in ents][:3])
    print("bad:", bad)
