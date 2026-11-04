import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vivlib import read_viv
from xfnlib import Xfn
import arfont

root = r"C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA"
ttf = r"C:\Windows\Fonts\tahomabd.ttf"
outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
for v in ("SHARED/UI/FE/realfont.viv", "SHARED/UI/FE/realftFE.viv"):
    _, ents, _, _, fl = read_viv(os.path.join(root, v))
    for name, d in fl:
        x = Xfn(d)
        info = arfont.add_arabic_auto(x, ttf)
        data = x.serialize()
        # re-parse check
        y = Xfn(data)
        assert y.serialize() == data
        print(v.split("/")[-1], name, len(d), "->", len(data), info)
        arfont.atlas_png(x, os.path.join(outdir, "ar_%s_%s.png" % (v.split("/")[-1][:-4], name[:-4].replace(" ", "_"))))
