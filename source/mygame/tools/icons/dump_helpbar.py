import struct, sys
sys.path.insert(0, r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\52e4b675-681f-4674-9762-57677d5870c9\scratchpad')
from shpx import *
from PIL import Image
d=open(r'C:\Users\nsr\Downloads\xboxrecomp-0.16.0\mygame\game\DATA\SHARED\UI\FE\mohfe.viv','rb').read()
sheets={'helpBar':(28316564,115584),'cafela_helpbar':(457388,110720)}
which=sys.argv[1]
kind=sys.argv[2]
hdr=int(sys.argv[3],0)
off,sz=sheets[which]
b=d[off:off+sz]
# find xsh inside big
cnt=struct.unpack('>I',b[8:12])[0]
p=16;ent={}
for i in range(cnt):
    o,s=struct.unpack('>II',b[p:p+8]); e=b.index(b'\0',p+8); ent[b[p+8:e].decode()]=(o,s); p=e+1
xo,xs=[v for k,v in ent.items() if k.endswith('.xsh')][0]
x=b[xo:xo+xs]
n=struct.unpack('<I',x[8:12])[0]
names=[];offs=[]
for i in range(n):
    q=16+i*8
    names.append(x[q:q+4].decode().strip()); offs.append(struct.unpack('<I',x[q+4:q+8])[0])
print(n,names)
sheet=Image.new('RGBA',(8*34,((n+7)//8)*34),(60,60,60,255))
for i,(nm,o) in enumerate(zip(names,offs)):
    t=x[o]; size=int.from_bytes(x[o+1:o+4],'little'); w,h=struct.unpack('<HH',x[o+4:o+8])
    print(nm,hex(t),size,w,h)
    img=decode(x[o+hdr:o+hdr+w*h],w,h,kind)
    sheet.paste(img,((i%8)*34+1,(i//8)*34+1),img)
sheet=sheet.resize((sheet.width*3,sheet.height*3),Image.NEAREST)
sheet.save(r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\52e4b675-681f-4674-9762-57677d5870c9\scratchpad\%s_%s.png'%(which,kind))
