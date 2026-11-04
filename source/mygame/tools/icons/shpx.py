import struct, sys
from PIL import Image

def rgb565(v):
    r=(v>>11)&31; g=(v>>5)&63; b=v&31
    return ((r<<3)|(r>>2), (g<<2)|(g>>4), (b<<3)|(b>>2))

def dxt_block(blk, kind):
    if kind=='dxt1':
        ab=None; cb=blk
    else:
        ab=blk[:8]; cb=blk[8:]
    c0,c1=struct.unpack('<HH',cb[:4]); idx=struct.unpack('<I',cb[4:])[0]
    p0=rgb565(c0); p1=rgb565(c1)
    if c0>c1 or kind!='dxt1':
        pal=[p0,p1,tuple((2*a+b)//3 for a,b in zip(p0,p1)),tuple((a+2*b)//3 for a,b in zip(p0,p1))]
    else:
        pal=[p0,p1,tuple((a+b)//2 for a,b in zip(p0,p1)),(0,0,0)]
    out=[]
    for i in range(16):
        c=pal[(idx>>(2*i))&3]
        if kind=='dxt3':
            a=(struct.unpack('<Q',ab)[0]>>(4*i))&15; a=a*17
        elif kind=='dxt5':
            a0,a1=ab[0],ab[1]; bits=int.from_bytes(ab[2:],'little'); ai=(bits>>(3*i))&7
            if ai==0:a=a0
            elif ai==1:a=a1
            elif a0>a1:a=((8-ai)*a0+(ai-1)*a1)//7
            else:
                a=[a0,a1,(4*a0+a1)//5,(3*a0+2*a1)//5,(2*a0+3*a1)//5,(a0+4*a1)//5,0,255][ai]
        else: a=255
        out.append(c+(a,))
    return out

def decode(data,w,h,kind):
    bs=8 if kind=='dxt1' else 16
    img=Image.new('RGBA',(w,h))
    px=img.load()
    bw=(w+3)//4
    for by in range(0,h,4):
        for bx in range(0,w,4):
            o=((by//4)*bw+(bx//4))*bs
            for i,c in enumerate(dxt_block(data[o:o+bs],kind)):
                x=bx+(i&3);y=by+(i>>2)
                if x<w and y<h: px[x,y]=c
    return img
