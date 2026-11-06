#!/usr/bin/env python3
"""guest.py PORT CMD...: connect to the guest's console (run-qemu.sh serves it on tcp
PORT), log in as root, run each CMD in turn and print its output and exit status."""
import os
import re
import socket
import sys
import time

port = int(sys.argv[1])
cmds = sys.argv[2:]
deadline = time.time() + 600
s = None
while time.time() < deadline:
    try:
        s = socket.create_connection(("127.0.0.1", port))
        break
    except OSError:
        time.sleep(1)
if s is None:
    sys.exit("guest.py: no console on port %d" % port)
s.settimeout(2)
buf = b""


def read_until(pattern, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
        if re.search(pattern, buf.decode(errors="replace")):
            return True
    return False


if not read_until(r"login: ", 420):
    print(buf.decode(errors="replace")[-2000:])
    sys.exit("guest.py: no login prompt")
s.send(b"root\n")
if os.environ.get("GUEST_PASS") is not None:	# an image with a root password (the device rootfs: n810)
    read_until(r"[Pp]assword: ", 10)
    s.send(os.environ["GUEST_PASS"].encode() + b"\n")
read_until(r"# ", 30)
for c in cmds:
    buf = b""
    s.send((c + "; echo __DONE__$?\n").encode())
    read_until(r"__DONE__\d+\r?\n", 120)
    print("$", c)
    print(re.sub(r"__DONE__(\d+).*", r"[exit \1]", buf.decode(errors="replace").split("\n", 1)[-1], flags=re.S))
