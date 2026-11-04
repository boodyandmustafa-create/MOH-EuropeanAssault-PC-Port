import struct, sys, re, zlib
sys.path.insert(0, r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\59a4f805-b616-4f02-9e26-c1442ad4b200\scratchpad')
from xdd import find_defs, parse, fmt, LEVEL

NAMES = {}
for line in open(r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\59a4f805-b616-4f02-9e26-c1442ad4b200\scratchpad\names.txt'):
    p = line.split()
    if len(p) == 2:
        NAMES[int(p[0], 16)] = p[1]

d = open(LEVEL, 'rb').read()
want = sys.argv[1:]
for name, s, e in find_defs(d):
    if want and not any(w in name for w in want):
        continue
    recs, end = parse(d, s - 12)
    print('==', name, 'recs', len(recs), 'end', hex(end))
    depth = 0
    for pos, crc, typ, val in recs:
        nm = NAMES.get(crc, '%08X' % crc)
        print('  %s%-22s t%d %s' % ('  ' * depth, nm, typ, fmt(typ, val)))
