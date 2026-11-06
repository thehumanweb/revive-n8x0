#!/bin/sh
# Boot the guest, start a command in it, and take QEMU screendumps of the panel.
# Usage: scripts/panel-shot.sh OUTDIR "COMMAND" [wait seconds (40)] [shots (2)] [gap seconds (6)]
# Example: scripts/panel-shot.sh build/shots "gbm_tri -n 0 -s 4"
# Writes OUTDIR/panel0.ppm, panel1.ppm, ... (convert with scripts/ppm2png.py) and OUTDIR/guest.log.
# Uses the QEMU built from this repo (see run-qemu.sh) and the guest's root login on tcp 5577.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?outdir}; cmd=${2:?command}; wait=${3:-40}; shots=${4:-2}; gap=${5:-6}
mkdir -p "$out"; out=$(cd "$out" && pwd)
rm -f "$out"/panel*.ppm "$here/build/mon.sock"
(MONITOR=unix:$here/build/mon.sock,server,nowait PORT=5577 "$here/scripts/run-qemu.sh" > "$out/qemu.log" 2>&1 &)
sleep 3
python3 "$here/scripts/guest.py" 5577 "($cmd > /tmp/run.log 2>&1 &) ; sleep $wait; tail -5 /tmp/run.log" > "$out/guest.log" 2>&1 || true
python3 - "$here/build/mon.sock" "$out" "$shots" "$gap" <<'P'
import socket, sys, time
sock_path, out, shots, gap = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); s.settimeout(5)
time.sleep(0.5)
try: s.recv(4096)
except Exception: pass
for i in range(shots):
    if i: time.sleep(gap)
    s.send(("screendump %s/panel%d.ppm\n" % (out, i)).encode()); time.sleep(1.5)
P
pgrep -x qemu-system-arm | xargs -r kill
ls -la "$out"/panel*.ppm
