#!/bin/sh
# Run on the jig host (storage), in ~/n810/mbx-test, once the new system answers on USB
# networking: this unit's local state from the rootfs backup (as ../flash-back/restore.sh
# does, but without replacing /boot/zImage.old, which the new image carries): wifi
# configuration, dropbear host keys, the clock base, seedrng state.
set -e
cd "$(dirname "$0")/.."
B=backup-2026-10-28/rootfs.tar
python3 - "$B" > /tmp/n810-restore.tar <<"PY"
import sys, tarfile
keep = ("./etc/wpa_supplicant.conf", "./etc/wifi-ui.conf", "./etc/dropbear/", "./etc/agent/",
        "./var/lib/clock", "./var/lib/clock.rtc", "./var/lib/seedrng/")
src = tarfile.open(sys.argv[1])
out = tarfile.open(fileobj=sys.stdout.buffer, mode="w|")
for m in src:
    if any(m.name == k.rstrip("/") or m.name.startswith(k) for k in keep):
        out.addfile(m, src.extractfile(m) if m.isfile() else None)
out.close()
PY
tar tf /tmp/n810-restore.tar
sh n810-net.sh "cd / && tar xf - && sync && echo restored" < /tmp/n810-restore.tar
rm -f /tmp/n810-restore.tar
