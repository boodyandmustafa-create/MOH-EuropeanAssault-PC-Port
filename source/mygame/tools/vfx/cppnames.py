import sys, re
sys.path.insert(0, r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\tools\vfx')
import xbestr as x
seen = {}
for m in re.finditer(rb'[A-Za-z0-9_\\]+\.cpp', x.xbe):
    s = m.group().decode('latin1').split('\\')[-1]
    seen[s] = x.off2va(m.start())
kw = sys.argv[1:] or ['xplo', 'renade', 'ocket', 'ine', 'etonat', 'amage', 'last', 'Shell', 'Mortar', 'Artil', 'Bomb', 'Proj', 'Camera', 'Fx', 'Effect', 'Debris', 'Shrap']
print(len(seen), 'cpp names')
for s in sorted(seen):
    if any(k.lower() in s.lower() for k in kw):
        print(s, hex(seen[s]) if seen[s] else None)
