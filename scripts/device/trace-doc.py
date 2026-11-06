#!/usr/bin/env python3
"""Build one readable hex trace (docs/n810-trace.txt) from the files scripts/device/take-trace.sh gathers:
the driver's register writes and TA words on the unit for two jobs (the first after loading the module),
the registers before and after, the parameter buffer, the region array and the picture; and the same
job on the emulator."""
import re, struct, sys, os
from collections import Counter

d, out = sys.argv[1], sys.argv[2]
L = []
def p(s=""): L.append(s)

def read(n, mode='r'):
    return open(os.path.join(d, n), mode, **({} if 'b' in mode else {'errors': 'replace'})).read()

def writes(text):
    """The driver's log: ('W', reg, val) and ('T'|'C', word) in order."""
    r = []
    for line in text.splitlines():
        m = re.search(r'mbx: (W) ([0-9a-f]{4}) ([0-9a-f]{8})', line)
        if m: r.append(('W', int(m.group(2), 16), int(m.group(3), 16))); continue
        m = re.search(r'mbx: ([TC]) ([0-9a-f]{8})', line)
        if m: r.append((m.group(1), int(m.group(2), 16))); continue
        m = re.search(r'mbx: R before the kick: (.*)$', line)
        if m: r.append(('R', m.group(1))); continue
        m = re.search(r'mbx: S (.*) ([0-9a-f]{8})$', line)
        if m: r.append(('S', m.group(1), int(m.group(2), 16))); continue
        m = re.search(r'mbx: I status ([0-9a-f]{8}) enable ([0-9a-f]{8}) handled ([0-9a-f]{8})', line)
        if m: r.append(('I', int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16))); continue
    return r

def regs(n):
    b = open(os.path.join(d, n), 'rb').read()
    return struct.unpack('<%dI' % (len(b) // 4), b)

def pbwords(n):
    b = open(os.path.join(d, n), 'rb').read()
    return struct.unpack('<16I', b[:64]), struct.unpack('<%dI' % ((len(b) - 64) // 4), b[64:])

p("N810 triangle trace (scripts/device/take-trace.sh): what our driver does for one frame, in hex")
p("=" * 100)
p(read('uname.txt').strip())
p("One job = a 64x64 ARGB8888 target, one Gouraud triangle (mbx/tools/tri_frame.h). The module was freshly loaded")
p("before job 1; job 2 is the next one. The target was filled with 0xff202020 by the CPU before each job.")
p()

for job in ('1', '2'):
    w = writes(read('trace%s.txt' % job))
    p("=" * 100)
    p("JOB %s: register writes and TA words, in order (W reg value / T = TA port word / C = TA control word)" % job)
    p("=" * 100)
    k = 0
    for e in w:
        if e[0] == 'W':
            p("  W %04x <- %08x" % (e[1], e[2]))
        elif e[0] == 'S':
            p("  ---- INT_STATUS %s: %08x" % (e[1], e[2]))
        elif e[0] == 'R':
            p("  ---- read back before the kick: " + e[1])
        elif e[0] == 'I':
            p("  ---- interrupt: INT_STATUS %08x, INT_ENABLE %08x, handled %08x" % (e[1], e[2], e[3]))
        else:
            p("  %s %3d    %08x" % (e[0], k, e[1])); k += 1
    p()
    b, a = regs('j%s.before' % job), regs('j%s.after' % job)
    p("-" * 100)
    p("JOB %s: registers that the job changed (offset: before -> after), of the first 16 KB of the window" % job)
    p("-" * 100)
    for i in range(len(a)):
        if a[i] != b[i]:
            p("  %04x: %08x -> %08x" % (i * 4, b[i], a[i]))
    p()
    p("JOB %s: registers non-zero after the job" % job)
    row = []
    for i in range(len(a)):
        if a[i]:
            row.append("%04x=%08x" % (i * 4, a[i]))
    for i in range(0, len(row), 6): p("  " + "  ".join(row[i:i + 6]))
    p()
    p("-" * 100)
    p("JOB %s: tool output (regions after the job, as the CPU reads them)" % job)
    p("-" * 100)
    p(read('j%s.txt' % job).rstrip())
    p()
    tab, pages = pbwords('pb%s.bin' % job)
    p("JOB %s: parameter buffer (device 0x1c00000..): non-zero rows of 8 words; page 0 holds the background object" % job)
    for pg in range(8):
        w8 = pages[pg * 1024:(pg + 1) * 1024]
        rows = [(r, w8[r:r + 8]) for r in range(0, 1024, 8) if any(w8[r:r + 8])]
        if not rows: continue
        p("  page %d (device %#x): %d non-zero rows" % (pg, 0x1c00000 + pg * 4096, len(rows)))
        for r, x in rows[:40]:
            p("    +%04x  " % (r * 4) + " ".join("%08x" % v for v in x))
        if len(rows) > 40: p("    ...")
    ents = [x for v in tab for x in (v & 0xffff, v >> 16)]
    p("  free-list table, 16-bit entries 0..31 (entry p holds the page after p; entry 0 the first page): " + " ".join("%04x" % x for x in ents))
    p()
    img = open(os.path.join(d, 'j%s.ppm' % job), 'rb').read().split(b'\n', 3)[3]
    c = Counter(img[i:i + 3] for i in range(0, len(img), 3))
    p("JOB %s: picture: %d distinct colours, %d pure white, %d clear grey (0x202020); per 16x16 tile, pixels that are not the clear colour:"
      % (job, len(c), c[b'\xff\xff\xff'], c[b'\x20\x20\x20']))
    for ty in range(4):
        p("   " + " ".join("%4d" % sum(1 for y in range(ty * 16, ty * 16 + 16) for x in range(tx * 16, tx * 16 + 16)
                                      if img[(y * 64 + x) * 3:(y * 64 + x) * 3 + 3] != b'\x20\x20\x20') for tx in range(4)))
    p()

p("=" * 100)
p("THE EMULATOR, same job (host shim, strict mode): register writes, the end of the TA, the render")
p("=" * 100)
sim = [l.replace('MESA: error: mbxemu: ', '') for l in read('sim.txt').splitlines()]
for l in sim:
    if l.startswith('write') or l.startswith('TA ') or l.startswith('render') or 'violation' in l or l.startswith('contract'):
        p("  " + l)
tab, pages = pbwords('sim-pb.bin')
p()
p("emulator: parameter buffer, non-zero rows (page 0 = the background object, page 1.. = object blocks and lists)")
for pg in range(4):
    w8 = pages[pg * 1024:(pg + 1) * 1024]
    rows = [(r, w8[r:r + 8]) for r in range(0, 1024, 8) if any(w8[r:r + 8])]
    if not rows: continue
    p("  page %d: %d non-zero rows" % (pg, len(rows)))
    for r, x in rows[:30]:
        p("    +%04x  " % (r * 4) + " ".join("%08x" % v for v in x))
open(out, 'w').write("\n".join(L) + "\n")
