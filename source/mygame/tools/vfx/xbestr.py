import struct, sys, re
xbe = open(r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DEFAULT.XBE', 'rb').read()
base = struct.unpack_from('<I', xbe, 0x104)[0]
nsec = struct.unpack_from('<I', xbe, 0x11C)[0]
secp = struct.unpack_from('<I', xbe, 0x120)[0] - base
secs = []
for i in range(nsec):
    o = secp + i * 56
    flags, va, vsz, raw, rsz = struct.unpack_from('<IIIII', xbe, o)
    secs.append((va, vsz, raw, rsz))


def off2va(off):
    for va, vsz, raw, rsz in secs:
        if raw <= off < raw + rsz:
            return va + off - raw
    return None


def va2off(va):
    for v, vsz, raw, rsz in secs:
        if v <= va < v + vsz:
            return raw + va - v
    return None


if __name__ == '__main__':
    pat = sys.argv[1].encode()
    for m in re.finditer(re.escape(pat), xbe):
        va = off2va(m.start())
        end = xbe.find(b'\0', m.start())
        print(hex(va) if va else None, xbe[m.start():end][:100])
