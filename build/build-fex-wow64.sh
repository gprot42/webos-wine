#!/bin/bash
# FEX's WoW64 CPU backend, libwow64fex.dll: an aarch64 Windows DLL that Wine
# loads to run the x86 code of 32-bit programs. Built with llvm-mingw in the
# winebuild container; output /work/wine/fex-wow64/libwow64fex.dll.
set -euo pipefail
W=/work/wine
export PATH=$W/llvm-mingw/bin:$PATH
cd $W
[ -d FEX ] || git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/FEX-Emu/FEX.git
cd FEX && git log -1 --format='FEX %h %cd'
python3 $W/build/patches/fex-wow64-lto.py .
rm -rf build-wow64 && mkdir build-wow64 && cd build-wow64
cmake -G Ninja .. -DCMAKE_TOOLCHAIN_FILE=../Data/CMake/toolchain_mingw.cmake -DMINGW_TRIPLE=aarch64-w64-mingw32 \
  -DCMAKE_BUILD_TYPE=Release -DTUNE_CPU=cortex-a76 -DENABLE_LTO=ON -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_CCACHE=OFF -DENABLE_OFFLINE_TELEMETRY=OFF > $W/fex-cmake.log 2>&1 \
  || { tail -30 $W/fex-cmake.log; exit 1; }
ninja -j6 wow64fex > $W/fex-build.log 2>&1 || { grep -E "error" $W/fex-build.log | head -20; exit 1; }
mkdir -p $W/fex-wow64 && cp $(find . -name libwow64fex.dll | head -1) $W/fex-wow64/
ls -la $W/fex-wow64
echo FEX-DONE
