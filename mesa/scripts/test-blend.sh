#!/bin/sh
# Blending on the host sim: the triangle drawn over the 0x20 grey clear with
# GL_ONE,GL_ONE and with GL_DST_COLOR,GL_ZERO, against the plain picture
# (run test-sim.sh first for build/gles2_tri). Checks every pixel.
set -eu
cd "$(dirname "$0")/../.."
mesa/scripts/run-sim.sh ./build/gles2_tri -a 0 -o build/blend-plain.ppm 2>/dev/null
mesa/scripts/run-sim.sh ./build/gles2_tri -a 0 -b 1 -o build/blend-add.ppm 2>/dev/null
mesa/scripts/run-sim.sh ./build/gles2_tri -a 0 -b 2 -o build/blend-mul.ppm 2>/dev/null
python3 -I - <<'P'
import sys
def ppm(n):
    return open('build/' + n, 'rb').read().split(b'\n', 3)[3]
plain, add, mul = ppm('blend-plain.ppm'), ppm('blend-add.ppm'), ppm('blend-mul.ppm')
bad_add = bad_mul = tri = 0
for i in range(0, len(plain), 3):
    px = plain[i:i + 3]
    if px == bytes([32, 32, 32]):          # not covered (or exactly the clear grey: then the blends agree with it below)
        if add[i:i + 3] != px or mul[i:i + 3] != px:
            bad_add += 1
        continue
    tri += 1
    for c in range(3):
        s = px[c]
        if add[i + c] != min(255, s + 32):
            bad_add += 1
        s256 = 256 if s == 255 else s
        if mul[i + c] != (s256 * 32) >> 8:
            bad_mul += 1
print("%d triangle pixels; additive mismatches %d, modulate mismatches %d" % (tri, bad_add, bad_mul))
sys.exit(1 if bad_add or bad_mul else 0)
P
