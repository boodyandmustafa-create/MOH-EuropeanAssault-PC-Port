import re, sys
GEN = r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\src\recomp\gen'


def load_func(fname, sub):
    lines = open(GEN + '\\' + fname, encoding='utf-8', errors='replace').read().split('\n')
    start = None
    for i, l in enumerate(lines):
        if l.startswith('void %s(void)' % sub):
            start = i
            break
    end = start + 1
    while not lines[end].startswith('void sub_') and not lines[end].startswith('#undef RC_SYNC_OUT'):
        end += 1
    return lines[start:end]


def cases(lines):
    # map label -> body lines
    labels = {}
    cur = None
    order = []
    for l in lines:
        m = re.match(r'^(loc_[0-9A-F]+): ;', l)
        if m:
            cur = m.group(1)
            labels[cur] = []
            order.append(cur)
        elif cur:
            labels[cur].append(l)
    res = []
    for oi, lab in enumerate(order):
        body = '\n'.join(labels[lab])
        m = re.search(r'cmp eax, (0x[0-9A-Fa-f]+)u? \(32-bit\)', body)
        if m:
            t = re.search(r'if \(CMP_(EQ|NE)\(_fa, _fb\)\) goto (loc_[0-9A-F]+)', body)
            if not t and oi + 1 < len(order):
                # shared flags: jg/jl here, je in the next label
                t = re.search(r'if \(CMP_(EQ|NE)\(_fa, _fb\)\) goto (loc_[0-9A-F]+)', '\n'.join(labels[order[oi + 1]]))
            if t:
                res.append((int(m.group(1).rstrip('u'), 16), t.group(1), t.group(2), lab))
    return labels, res


def body_summary(labels, lab):
    out = []
    seen = 0
    cur = lab
    txt = []
    # follow fallthrough bodies linearly until ret
    keys = list(labels.keys())
    idx = keys.index(lab)
    for k in keys[idx:idx + 6]:
        for l in labels[k]:
            txt.append(l.strip())
        if any('RC_SYNC_OUT(); return' in l for l in labels[k]):
            break
    return txt


if __name__ == '__main__':
    fname, sub = sys.argv[1], sys.argv[2]
    lines = load_func(fname, sub)
    labels, res = cases(lines)
    for crc, kind, tgt, lab in res:
        if kind == 'EQ':
            summ = body_summary(labels, tgt)
            interesting = [s for s in summ if re.search(r'MEM(32|F|8|16)?\((esi|ebx|ebp)[^)]*\)|RC_CALL', s)]
            short = ' | '.join(re.sub(r'\s+', ' ', s)[:90] for s in interesting[:7])
            print('%08X -> %s : %s' % (crc, tgt, short))
        else:
            print('%08X NE (default at %s)' % (crc, tgt))
