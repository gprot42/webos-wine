#!/usr/bin/env python3
"""FEX libwow64fex.dll: make it link with LTO.

- BTCpuSimulateImpl is called only from inline assembly; LTO cannot see that
  call and dropped the function. Mark it used.
- With LTO, code that the normal build discards calls __clear_cache. The
  compiler runtime's version needs FlushInstructionCache, which FEX defines
  in a static library that the linker no longer extracts after LTO (even
  with --undefined). So FEX gets its own __clear_cache, on the ntdll call it
  already uses.
Idempotent.
"""
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "Source/Windows/WOW64"
m = src / "Module.cpp"
t = m.read_text()
old = 'extern "C" void BTCpuSimulateImpl(CONTEXT* entry_context) {'
new = 'extern "C" __attribute__((used)) void BTCpuSimulateImpl(CONTEXT* entry_context) {'
if new not in t:
    if old not in t:
        sys.exit("BTCpuSimulateImpl definition not found")
    m.write_text(t.replace(old, new))

t = m.read_text()
if "wine-tv: __clear_cache" not in t:
    t += """
// wine-tv: __clear_cache for the LTO build (build/patches/fex-wow64-lto.py).
extern "C" __attribute__((used)) void __clear_cache(void* Begin, void* End) {
  NtFlushInstructionCache(NtCurrentProcess(), Begin, reinterpret_cast<char*>(End) - reinterpret_cast<char*>(Begin));
}
"""
    m.write_text(t)
print("FEX LTO patch applied")
