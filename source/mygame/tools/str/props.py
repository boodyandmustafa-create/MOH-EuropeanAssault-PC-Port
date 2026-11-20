import struct, zlib
def parse(b):
    n = struct.unpack_from("<I", b, 0)[0]
    off = 4; out = []
    for i in range(n):
        h, t, sz = struct.unpack_from("<III", b, off)
        v = b[off + 12:off + 12 + sz]
        out.append((off, h, t, sz, v))
        off += 12 + sz
    return out, off
