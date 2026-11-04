import numpy as np


def morton_indices(W, H):
    """Xbox swizzle: bits of x and y interleaved (x in even bits first). For non-square textures the
    interleave stops when the smaller dimension's bits run out and the remaining bits of the larger
    dimension are appended on top. Returns array idx[y, x] -> linear position in swizzled memory."""
    xb = W.bit_length() - 1
    yb = H.bit_length() - 1
    idx = np.zeros((H, W), dtype=np.int64)
    ys, xs = np.mgrid[0:H, 0:W]
    pos = 0
    out = np.zeros((H, W), dtype=np.int64)
    xi = yi = 0
    for bit in range(xb + yb):
        # alternate x,y while both have bits left
        if xi < xb and (bit % 2 == 0 or yi >= yb):
            out |= ((xs >> xi) & 1) << bit
            xi += 1
        elif yi < yb:
            out |= ((ys >> yi) & 1) << bit
            yi += 1
        else:
            out |= ((xs >> xi) & 1) << bit
            xi += 1
    return out


def unswizzle(data, W, H, bpp=1):
    m = morton_indices(W, H)
    a = np.frombuffer(data, dtype=np.uint8)
    return a[m.reshape(-1)].reshape(H, W)


def swizzle(img, W, H):
    m = morton_indices(W, H)
    out = np.zeros(W * H, dtype=np.uint8)
    out[m.reshape(-1)] = img.reshape(-1)
    return out.tobytes()
