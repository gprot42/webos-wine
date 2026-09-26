#!/usr/bin/env python3
"""winegcc: link ARM64 PE modules with 4 KB alignment instead of 64 KB.

Wine aligns ARM64 PE sections (and so file offsets) to 64 KB, for kernels
with 64 KB pages. The TV's kernel uses 4 KB pages, and the padding made the
ARM64 DLLs about 150 MB bigger than their code (488 MB for 758 files).
Idempotent.
"""
import sys
from pathlib import Path

f = Path(sys.argv[1]) / "tools/winegcc/winegcc.c"
t = f.read_text()
old = 'section_align = (target.cpu == CPU_ARM64 || target.cpu == CPU_ARM64EC) ? "0x10000" : "0x1000";'
new = 'section_align = "0x1000";  /* webOS TV: 4 KB pages; see build/patches/wine-arm64-4k-align.py */'
if new not in t:
    if old not in t:
        sys.exit("winegcc alignment line not found")
    f.write_text(t.replace(old, new))
print("winegcc 4 KB alignment patch applied")
