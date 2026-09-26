#!/usr/bin/env python3
"""Copy the transitive shared-library closure of some ELF files and sonames
from an armhf tree into one flat directory (real files, no symlinks)."""
import os, shutil, subprocess, sys

out, libdirs, roots = sys.argv[1], sys.argv[2].split(), sys.argv[3:]
os.makedirs(out, exist_ok=True)

def needed(path):
    txt = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return [l.split("[", 1)[1].split("]", 1)[0] for l in txt.splitlines() if "(NEEDED)" in l]

def find(soname):
    for d in libdirs:
        p = os.path.join(d, soname)
        if os.path.exists(p):
            return p
    return None

seen, queue, missing = set(), [], []
for r in roots:
    if os.path.isfile(r):
        queue += needed(r)
    else:
        queue.append(r)
while queue:
    so = queue.pop()
    if so in seen or so.startswith("ld-linux"):
        continue
    seen.add(so)
    p = find(so)
    if not p:
        missing.append(so)
        continue
    shutil.copyfile(p, os.path.join(out, so))
    queue += needed(p)
print(f"{len(seen) - len(missing)} libraries copied")
if missing:
    print("missing:", " ".join(sorted(missing)))
