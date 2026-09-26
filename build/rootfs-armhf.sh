#!/bin/bash
# Debian bookworm armhf tree (packages extracted, no scripts run).
# /work/wine/rootfs: runtime libraries shipped to the TV.
# /work/wine/sysroot: the same plus headers, for cross-compiling.
set -euo pipefail
W=/work/wine
mkdir -p $W
RUNTIME="libc6,libstdc++6,libgcc-s1,
libx11-6,libxext6,libxrender1,libxrandr2,libxi6,libxcursor1,libxcomposite1,libxinerama1,libxfixes3,libxxf86vm1,libxtst6,libxdamage1,libxkbfile1,libxshmfence1,libxau6,libxdmcp6,libxcb1,libx11-xcb1,libxcb-shm0,libxcb-randr0,libxcb-xfixes0,libxcb-render0,libxcb-dri2-0,libxcb-dri3-0,libxcb-present0,libxcb-sync1,libxcb-glx0,
libfreetype6,libfontconfig1,fontconfig-config,fonts-dejavu-core,libpng16-16,zlib1g,libbz2-1.0,libbrotli1,libexpat1,libuuid1,
libasound2,libpulse0,libsdl2-2.0-0,libdbus-1-3,libgnutls30,
libwayland-client0,libffi8,
libpixman-1-0,libxfont2,libfontenc1,libgcrypt20,libgpg-error0,libsystemd0,libaudit1,libbsd0,libmd0,libunwind8,liblzma5,libzstd1,liblz4-1,libcap2,
x11-xkb-utils,xkb-data,
libgl1,libglx0,libglvnd0,libgl1-mesa-dri,libglx-mesa0,libglapi-mesa,libegl1,libegl-mesa0,libgbm1,libdrm2,libllvm15,libsensors5,libelf1,libedit2,libtinfo6,libxml2,libicu72,
libvulkan1,libosmesa6"
DEV="libc6-dev,linux-libc-dev,libwayland-dev,libx11-dev,libxext-dev,libxtst-dev,libxdamage-dev,libxfixes-dev,libpixman-1-dev,libxfont-dev,libxkbfile-dev,libxshmfence-dev,libxau-dev,libxdmcp-dev,libssl-dev,libpciaccess-dev,libxcvt-dev,x11proto-dev,xtrans-dev,libfontenc-dev,libfreetype-dev,zlib1g-dev,libgcrypt20-dev,libbsd-dev,libmd-dev"
pkgs=$(echo "$RUNTIME" | tr -d '\n ')
if [ ! -f $W/rootfs/.done ]; then
  rm -rf $W/rootfs
  mmdebstrap --variant=extract --architectures=armhf --include="$pkgs" bookworm $W/rootfs http://deb.debian.org/debian
  touch $W/rootfs/.done
fi
if [ ! -f $W/sysroot/.done ]; then
  rm -rf $W/sysroot
  mmdebstrap --variant=extract --architectures=armhf --include="$pkgs,$(echo "$DEV" | tr -d '\n ')" bookworm $W/sysroot http://deb.debian.org/debian
  python3 - <<'PY'
# Absolute symlinks in the sysroot point at the build machine's /lib; make them relative.
import os
root='/work/wine/sysroot'
for d,_,fs in os.walk(root):
    for f in fs:
        p=os.path.join(d,f)
        if os.path.islink(p):
            t=os.readlink(p)
            if t.startswith('/'):
                os.unlink(p); os.symlink(os.path.relpath(root+t, d), p)
PY
  touch $W/sysroot/.done
fi
du -sh $W/rootfs $W/sysroot
