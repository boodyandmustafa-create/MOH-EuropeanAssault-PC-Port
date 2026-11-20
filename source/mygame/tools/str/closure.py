import struct, collections
from walk import walk
BASE = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
def load(level):
    grp = level.split("_")[0]
    return walk(BASE + "\%s\%s\level_a.str" % (grp, level))
def closure(d, recs, root_name, skip_types=()):
    byname = collections.defaultdict(list)
    for r in recs: byname[r[1]].append(r)
    seen = {}
    todo = [root_name]
    while todo:
        n = todo.pop()
        for r in byname.get(n, []):
            if r[0] in seen: continue
            seen[r[0]] = r
            body = d[r[0] + 16:r[0] + r[3]]
            for (w,) in struct.iter_unpack("<I", body[:len(body) // 4 * 4]):
                if w in byname and w != n:
                    todo.append(w)
    return sorted(seen.values())
if __name__ == "__main__":
    import sys
    donor, target, name = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    dd, dp, dr, _ = load(donor)
    td, tp, tr, _ = load(target)
    tnames = {(r[1], r[2]) for r in tr}
    cl = closure(dd, dr, name)
    print("closure records", len(cl), "bytes", sum(r[3] for r in cl))
    new = [r for r in cl if (r[1], r[2]) not in tnames]
    print("not in target", len(new), "bytes", sum(r[3] for r in new))
    c = collections.Counter(hex(r[2]) for r in new)
    print(c)
