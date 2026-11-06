#!/usr/bin/env python3
# List or extract a file from a JFFS2 image without mounting it: jffs2x.py IMAGE ls | jffs2x.py IMAGE /path (to stdout).
# Reads dirent and inode nodes (no, zlib and rtime compression), little-endian images only.
import struct, sys, zlib
img = open(sys.argv[1], 'rb').read()
dirents = {}   # (pino,name) -> (version, ino, type)
inodes = {}    # ino -> {'mode':..,'isize':..,'nodes':[(version, offset, dsize, compr, data)]}
def rtime(data, dsize):
    out = bytearray(dsize); pos = [0]*256; i = 0; o = 0
    while o < dsize:
        v = data[i]; i += 1; out[o] = v; o += 1
        r = data[i]; i += 1; p = pos[v]; pos[v] = o
        if p:
            for _ in range(r):
                out[o] = out[p]; o += 1; p += 1
        else:
            o += r
    return bytes(out)
i = 0; n = len(img)
while i + 12 <= n:
    magic, typ, totlen = struct.unpack_from('<HHI', img, i)
    if magic != 0x1985 or totlen < 12 or i + totlen > n:
        i += 4; continue
    if typ == 0xE001:
        pino, ver, ino, mctime, nsize, dtype = struct.unpack_from('<IIIIBB', img, i + 12)
        name = img[i + 40:i + 40 + nsize].decode('latin1')
        k = (pino, name)
        if k not in dirents or dirents[k][0] < ver:
            dirents[k] = (ver, ino, dtype)
    elif typ == 0xE002:
        ino, ver, mode, uid, gid, isize, at, mt, ct, off, csize, dsize, comp, ucomp, flags = struct.unpack_from('<IIIHHIIIIIIIBBH', img, i + 12)
        data = img[i + 68:i + 68 + csize]
        if comp == 6: data = zlib.decompress(data)
        elif comp == 2: data = rtime(data, dsize)
        elif comp == 1: data = b'\0' * dsize
        d = inodes.setdefault(ino, {'mode': 0, 'isize': 0, 'ver': -1, 'nodes': []})
        if ver > d['ver']: d['ver'] = ver; d['mode'] = mode; d['isize'] = isize
        d['nodes'].append((ver, off, dsize, data))
    i += (totlen + 3) & ~3
def content(ino):
    d = inodes[ino]; buf = bytearray(d['isize'])
    for ver, off, dsize, data in sorted(d['nodes']):
        buf[off:off + len(data)] = data[:max(0, d['isize'] - off)]
    return bytes(buf)
def walk(ino, path, depth=0):
    for (pino, name), (ver, cino, dtype) in sorted(dirents.items()):
        if pino != ino or cino == 0: continue
        p = path + '/' + name
        mode = inodes.get(cino, {}).get('mode', 0)
        print(oct(mode), p, inodes.get(cino, {}).get('isize', ''))
        if (mode & 0o170000) == 0o040000 and depth < 6: walk(cino, p, depth + 1)
if sys.argv[2] == 'ls':
    walk(1, '')
else:
    target = sys.argv[2].strip('/').split('/')
    ino = 1
    for part in target:
        ino = dirents[(ino, part)][1]
    sys.stdout.buffer.write(content(ino))
