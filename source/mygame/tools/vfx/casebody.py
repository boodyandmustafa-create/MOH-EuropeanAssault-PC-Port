import sys, re
sys.path.insert(0, r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\59a4f805-b616-4f02-9e26-c1442ad4b200\scratchpad')
from handler import load_func, cases, body_summary

fname, sub = sys.argv[1], sys.argv[2]
lines = load_func(fname, sub)
labels, res = cases(lines)
tgt = {}
for crc, kind, t, lab in res:
    if kind == 'EQ':
        tgt[crc] = t
for a in sys.argv[3:]:
    crc = int(a, 16)
    if crc not in tgt:
        print(a, 'no EQ case')
        continue
    print('#### %s -> %s' % (a, tgt[crc]))
    for s in body_summary(labels, tgt[crc]):
        s = re.sub(r'\s+', ' ', s)
        if s.startswith('_f') or 'add result' in s or 'sub result' in s:
            continue
        print('   ', s[:140])
