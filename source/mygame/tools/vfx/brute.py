import sys, zlib, itertools, re
sys.path.insert(0, r'C:\Users\nsr\AppData\Local\Temp\claude\C--Users-nsr-Downloads-xboxrecomp-0-16-0\59a4f805-b616-4f02-9e26-c1442ad4b200\scratchpad')
from crcnames import consts

targets = consts('recomp_0022.c', 'sub_000B97F0') | consts('recomp_0022.c', 'sub_000BAC90')
print(len(targets), 'targets')

words = '''start end min max initial final begin life lifetime time size width height scale speed velocity vel accel acceleration
gravity drag friction rate count num number particles particle emit emitter emission burst spread angle cone color colour alpha
red green blue fade in out texture tex rotation rot spin angular random variance var spawn delay duration x y z up down dir direction
position pos offset range radius length thickness stretch blend mode type name file filename mesh model shape flags flag enable enabled
loop looping loops system effect effects sprite sprites bounce bounces collide collision collisions world rate frames frame anim animation
fps u v uv tile tiles rows columns cols row col amount total per second sec max min low high mid base rand speedvar sizevar lifevar
age intensity brightness opacity trail trails sort priority lod distance dist near far cull lights light glow additive subtractive
facing face billboard camera drift wind turbulence noise swirl orbit attract force forces mass weight damping damp air resistance
roll pitch yaw heading normal surface impact hit decal stain splat mist gore bloodsplat spurt spray chunks chunk droplets droplet
emitrate emitcount maxparticles numparticles startsize endsize startalpha endalpha startcolor endcolor startcolour endcolour
lifespan span ttl'''.split()
words = sorted(set(words))
print(len(words), 'words')

found = {}


def check(name):
    b = name.encode()
    c = zlib.crc32(b) & 0xffffffff
    if c in targets:
        found.setdefault(c, name)


def variants(parts):
    yield ''.join(p.capitalize() for p in parts)
    yield parts[0] + ''.join(p.capitalize() for p in parts[1:])
    yield ''.join(parts)
    yield '_'.join(parts)
    yield '_'.join(p.upper() for p in parts)
    yield ''.join(p.upper() for p in parts)
    yield '_'.join(p.capitalize() for p in parts)
    yield ' '.join(parts)
    yield ' '.join(p.capitalize() for p in parts)


for n in (1, 2, 3):
    for combo in itertools.product(words, repeat=n):
        for v in variants(combo):
            check(v)
    print('n=%d done, found %d' % (n, len(found)), flush=True)

for c, n in sorted(found.items()):
    print('%08X %s' % (c, n))
