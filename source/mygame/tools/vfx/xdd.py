import struct, sys, re, zlib

LEVEL = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA\1\1_1\level_a.str'


def find_defs(d):
    out = []
    for m in re.finditer(rb'file:\\z:\\moh4\\data\\shared\\vfx\\definitions\\[A-Za-z0-9_\\]+\.xdd', d):
        out.append((m.group(0).decode().split('\\')[-1], m.start(), m.end() + 1))
    return out


def parse(d, pos, maxrec=2000, verbose=False):
    recs = []
    for _ in range(maxrec):
        if pos + 8 > len(d):
            break
        crc, typ = struct.unpack_from('<II', d, pos)
        if typ == 7:
            recs.append((pos, crc, 7, None))
            pos += 8
            continue
        if typ > 7 or pos + 12 > len(d):
            break
        size = struct.unpack_from('<I', d, pos + 8)[0]
        if size > 0x400:
            break
        recs.append((pos, crc, typ, d[pos + 12:pos + 12 + size]))
        pos += 12 + size
    return recs, pos


def fmt(typ, val):
    if val is None:
        return '{'
    if typ in (0, 1) and len(val) == 4:
        i = struct.unpack('<i', val)[0]
        f = struct.unpack('<f', val)[0]
        return 'f=%g' % f if typ == 1 else 'i=%d (%g)' % (i, f)
    if typ == 5 and len(val) == 16:
        return 'v4=%s' % (tuple(round(x, 4) for x in struct.unpack('<4f', val)),)
    return 'raw=' + val.hex()


if __name__ == '__main__':
    d = open(LEVEL, 'rb').read()
    for name, s, e in find_defs(d):
        recs, end = parse(d, e)
        print(name, hex(s), 'recs', len(recs), 'end', hex(end), 'next bytes', d[end:end + 24].hex())
