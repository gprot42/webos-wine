#!/bin/bash
# Debian bookworm armhf tree (packages extracted, no scripts run).
# /work/wine/rootfs: runtime libraries shipped to the TV.
# /work/wine/sysroot: the same plus headers, for cross-compiling.
set -euo pipefail
W=/work/wine
mkdir -p $W
# What the armhf part needs: wine-tv, Xvfb and xkbcomp (Wine itself is
# aarch64, from the winebuild container), plus keyboard data, X11 locale data
# and the DejaVu fonts that fonts.conf points Wine at.
RUNTIME="libc6,libgcc-s1,libx11-6,libx11-data,libxext6,libxtst6,libxdamage1,libxfixes3,libxfont2,
libxkbfile1,libxau6,libxdmcp6,libxcb1,libpixman-1-0,libbsd0,libmd0,libgcrypt20,libgpg-error0,
libfreetype6,libpng16-16,zlib1g,libbz2-1.0,libbrotli1,libfontenc1,libwayland-client0,libffi8,
x11-xkb-utils,xkb-data,fonts-dejavu-core"
# Headers for building Xvfb and wine-tv.
DEV="libc6-dev,linux-libc-dev,libwayland-dev,libx11-dev,libxext-dev,libxtst-dev,libxdamage-dev,libxfixes-dev,libpixman-1-dev,libxfont-dev,libxkbfile-dev,libxshmfence-dev,libxau-dev,libxdmcp-dev,libxcvt-dev,x11proto-dev,xtrans-dev,libfontenc-dev,libfreetype-dev,zlib1g-dev,libgcrypt20-dev,libbsd-dev,libmd-dev"
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
