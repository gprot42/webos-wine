#!/bin/bash
# Native ARM64 Wine for the TV, built in the "winebuild" container
# (Debian bookworm arm64, the same glibc as the runtime shipped in rt64/).
#   Unix side: aarch64, built natively.
#   PE side:   aarch64 DLLs, plus i386 DLLs for 32-bit programs under WoW64
#              (--enable-archs=aarch64,i386), cross-built with llvm-mingw.
# Output: /work/wine/wine-arm64 (prefix /opt/wine, relocatable).
set -euo pipefail
W=/work/wine
V=${WINE_VERSION:-11.0}
export DEBIAN_FRONTEND=noninteractive
if [ ! -f /.deps-done ]; then
  apt-get update -qq
  apt-get install -y -qq build-essential flex bison gettext pkg-config curl ca-certificates xz-utils git \
    python3 cmake ninja-build patchelf file cabextract p7zip-full curl unzip gawk \
    libx11-dev libxext-dev libxrender-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
    libxcomposite-dev libxfixes-dev libxxf86vm-dev libfreetype-dev libfontconfig-dev libgnutls28-dev \
    libasound2-dev libpulse-dev libdbus-1-dev libunwind-dev >/dev/null
  touch /.deps-done
fi
# llvm-mingw: clang cross-compilers for the aarch64 and i686 Windows DLLs.
if [ ! -x $W/llvm-mingw/bin/aarch64-w64-mingw32-clang ]; then
  url=$(curl -sfL https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest \
        | grep -o 'https://[^"]*ucrt-ubuntu-22.04-aarch64.tar.xz' | head -1)
  echo "llvm-mingw: $url"
  rm -rf $W/llvm-mingw && mkdir -p $W/llvm-mingw
  curl -sfL "$url" | tar xJ -C $W/llvm-mingw --strip-components=1
fi
export PATH=$W/llvm-mingw/bin:$PATH
cd $W
[ -f wine-$V.tar.xz ] || curl -sfLO https://dl.winehq.org/wine/source/${V%%.*}.0/wine-$V.tar.xz
rm -rf wine-$V-src && mkdir wine-$V-src && tar xf wine-$V.tar.xz -C wine-$V-src --strip-components=1
mkdir -p wine-arm64-build && cd wine-arm64-build && rm -rf ./*
../wine-$V-src/configure --prefix=/opt/wine --enable-archs=aarch64,i386 --disable-tests \
  --without-wayland --without-vulkan --without-opengl --without-oss --without-cups --without-sane \
  --without-gphoto --without-v4l2 --without-krb5 --without-netapi --without-capi --without-pcap \
  --without-usb --without-gstreamer --without-opencl --without-coreaudio --without-sdl \
  --without-udev --without-xinput2 > $W/wine-arm64-configure.log 2>&1 \
  || { tail -30 $W/wine-arm64-configure.log; exit 1; }
grep -E "^configure: (WARNING|error)" $W/wine-arm64-configure.log | head -20 || true
make -j12 > $W/wine-arm64-make.log 2>&1 || { grep -E "error" $W/wine-arm64-make.log | head -20; exit 1; }
rm -rf $W/wine-arm64 && make install DESTDIR=$W/wine-arm64 > /dev/null
ls $W/wine-arm64/opt/wine/lib/wine
du -sh $W/wine-arm64
echo WINE-ARM64-DONE
