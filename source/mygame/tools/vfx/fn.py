import sys, re, glob
GEN = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\src\recomp\gen'


def find(sub):
    for f in sorted(glob.glob(GEN + r'\recomp_*.c')):
        txt = open(f, encoding='utf-8', errors='replace').read()
        m = re.search(r'^void %s\(void\)\n\{' % sub, txt, re.M)
        if m:
            end = txt.find('\n}\n', m.start())
            return f, txt[m.start():end]
    return None, None


for sub in sys.argv[1:]:
    f, body = find(sub)
    print('######', sub, f.split('\\')[-1] if f else None)
    if not body:
        continue
    for l in body.split('\n'):
        s = l.strip()
        if not s or s.startswith('_f') or 'add result' in s or 'sub result' in s or s.startswith('(void)') or s.startswith('int _flags') or s.startswith('uint32_t') or s.startswith('int32_t') or s.startswith('#'):
            continue
        s = re.sub(r'_fa = \(uint32_t\)\((\w+)\) & 0xFFFFFFFFu; _fb = \(uint32_t\)\((\w+)\) & 0xFFFFFFFFu;', r'cmp \1,\2;', s)
        print('  ', s[:150])
