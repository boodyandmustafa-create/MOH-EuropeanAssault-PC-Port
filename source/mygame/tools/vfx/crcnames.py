import re, sys, zlib, struct
sys.path.insert(0, r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\59a4f805-b616-4f02-9e26-c1442ad4b200\scratchpad')
from xbestr import xbe, off2va

GEN = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\src\recomp\gen'

# all hex immediates in handler functions
def consts(fname, sub):
    from handler import load_func
    lines = load_func(fname, sub)
    s = '\n'.join(lines)
    return set(int(x.rstrip('uU'), 16) for x in re.findall(r'0x([0-9A-Fa-f]{8})u?\b', s))


def all_strings(minlen=3):
    res = {}
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, xbe):
        s = m.group(0)
        res.setdefault(s, m.start())
        # also substrings split at spaces and punctuation
        for t in re.split(rb'[ :,;=()\[\]\\/\'"]+', s):
            if len(t) >= minlen:
                res.setdefault(t, m.start())
    return res


def crc(s):
    return zlib.crc32(s) & 0xffffffff


if __name__ == '__main__':
    fname, sub = sys.argv[1], sys.argv[2]
    cs = consts(fname, sub)
    print(len(cs), 'consts')
    strs = all_strings()
    print(len(strs), 'strings')
    tab = {}
    for s in strs:
        tab.setdefault(crc(s), s)
        tab.setdefault(crc(s.lower()), s.lower())
    hit = 0
    for c in sorted(cs):
        if c in tab:
            print('%08X %s' % (c, tab[c].decode()))
            hit += 1
    print('hits', hit)
