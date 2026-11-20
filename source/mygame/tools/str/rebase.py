import struct
D = 0xA0000  # 1_21 -> 1_2 shift of the shared 20-byte-item block (seg 0x50..0x52)
PTR_TYPES = {0x6b6e6d02, 0x42161ecc, 0xc602c263}
def is_ptr(w):
    return (w >> 24) == 2 and 0x500000 <= (w & 0xffffff) < 0x530000 and (w & 3) == 0
def rebase_body(body, shift=D):
    n = len(body) // 4
    ws = list(struct.unpack_from("<%dI" % n, body))
    cnt = 0
    for i, w in enumerate(ws):
        if is_ptr(w):
            ws[i] = (w + shift) & 0xffffffff; cnt += 1
    return struct.pack("<%dI" % n, *ws) + body[n * 4:], cnt
