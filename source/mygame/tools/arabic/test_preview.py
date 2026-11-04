import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
from vivlib import read_viv
from xfnlib import Xfn
import arfont, arshape, preview

root = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
ttf = r"C:\Windows\Fonts\tahomabd.ttf"
out = sys.argv[1]
_, _, _, _, fl = read_viv(os.path.join(root, "SHARED/UI/FE/realfont.viv"))
d = dict(fl)
_, _, _, _, fl2 = read_viv(os.path.join(root, "SHARED/UI/FE/realftFE.viv"))
d2 = dict(fl2)
tests = ["الخيارات", "اضغط [$ACTION] للمواجهة", "ابدأ لعبة جديدة", "المرحلة 3: لا تتوقف! Level", "الحياة والذخيرة 100"]
rows = []
for label, src, name in (("ExtBd18", d, "Futura Std Condensed ExtBd_18.xfn"), ("Futura24", d2, "Futura Std Condensed_24.xfn"),
                         ("Impact24", d2, "Impact_24.xfn"), ("TwCen36", d2, "Tw Cen MT Condensed_36.xfn")):
    x = Xfn(src[name])
    info = arfont.add_arabic_auto(x, ttf)
    for t in tests:
        v = arshape.visual(t)
        rows.append(preview.draw_text(x, v, line_h=30 if "24" in label or "36" in label else 24, scale=2))
W = max(r.width for r in rows)
H = sum(r.height for r in rows)
sheet = Image.new("RGB", (W, H), (0, 0, 0))
y = 0
for r in rows:
    sheet.paste(r, (0, y))
    y += r.height
sheet.save(os.path.join(out, "preview.png"))
print(sheet.size)
