#!/usr/bin/env python3
"""Point a bundled glibc loader away from /etc/ld.so.preload.

The TV's /etc/ld.so.preload names LG's 32-bit /lib/libSegFault.so. Our own
loaders cannot load it and print an "ERROR: ld.so: object ... cannot be
preloaded" line for every program they start, which buried the winetricks
log. The path is replaced in place by one of the same length (18 bytes):
  --preload FILE   use FILE instead (the aarch64 loader reads
                   /tmp/wine-tv/prelo, which wine-tv writes; see
                   src/cow-preload.c)
  otherwise        /etc/ld.so.nopreld, which does not exist
Usage: ldso-no-preload.py [--preload FILE] LOADER...
"""
import sys
from pathlib import Path

OLD = b"/etc/ld.so.preload"
args = sys.argv[1:]
new = b"/etc/ld.so.nopreld"
if args[:1] == ["--preload"]:
    new = args[1].encode()
    args = args[2:]
if len(new) != len(OLD):
    sys.exit(f"replacement must be {len(OLD)} bytes: {new!r}")
for name in args:
    f = Path(name)
    data = f.read_bytes()
    if OLD in data:
        f.write_bytes(data.replace(OLD, new))
        print(f"{name}: preload file is now {new.decode()}")
    elif new not in data:
        sys.exit(f"{name}: /etc/ld.so.preload not found")
