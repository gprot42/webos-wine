#!/bin/bash
# Build wine-tv and lay out the armhf part of the app in $OUT (inside ffbuild):
# wine-tv, Xvfb, xkbcomp and their runtime. Wine itself (aarch64) is added by
# assemble-wine64.sh in the winebuild container.
set -euo pipefail
. /work/wine/build/env.sh
SRC=$W/src
LIBDIRS="$ROOTFS/lib/arm-linux-gnueabihf $ROOTFS/usr/lib/arm-linux-gnueabihf $ROOTFS/usr/lib/arm-linux-gnueabihf/pulseaudio $ROOTFS/usr/lib"
rm -rf $OUT && mkdir -p $OUT/bin $OUT/rt/lib

echo "== wine-tv"
GEN=$W/gen && mkdir -p $GEN
for p in webos-shell text-model; do
  wayland-scanner client-header $SRC/protocol/$p.xml $GEN/$p-client-protocol.h
  wayland-scanner private-code $SRC/protocol/$p.xml $GEN/$p-protocol.c
done
$CROSS-gcc --sysroot=$SYSROOT $ARMFLAGS -O2 -Wall -Wextra -Wno-format-truncation -std=gnu11 -I$GEN \
  -o $OUT/wine-tv $SRC/wine-tv.c $GEN/webos-shell-protocol.c $GEN/text-model-protocol.c \
  -Wl,--dynamic-linker=$TV_LDSO -Wl,--disable-new-dtags -Wl,-rpath,$APPDIR/rt/lib \
  -lwayland-client -lX11 -lXtst -lXdamage -lXfixes

echo "== Xvfb, xkbcomp"
$CROSS-strip -o $OUT/bin/Xvfb $W/xorg-server-*/build/hw/vfb/Xvfb
cp $ROOTFS/usr/bin/xkbcomp $OUT/bin/xkbcomp
patchelf --force-rpath --set-interpreter $TV_LDSO --set-rpath $APPDIR/rt/lib $OUT/bin/xkbcomp

echo "== armhf libraries"
python3 $W/build/libclosure.py $OUT/rt/lib "$LIBDIRS" \
  $OUT/wine-tv $OUT/bin/Xvfb $OUT/bin/xkbcomp
cp -L $ROOTFS/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 $OUT/rt/lib/
python3 $W/build/patches/ldso-no-preload.py $OUT/rt/lib/ld-linux-armhf.so.3
find $OUT/rt/lib -name '*.so*' ! -name 'ld-linux*' -exec $CROSS-strip --strip-unneeded {} + 2>/dev/null || true
for f in $OUT/rt/lib/*.so*; do
  case $f in *ld-linux*) continue ;; esac
  patchelf --remove-rpath "$f" 2>/dev/null || true
done

echo "== X data and fonts"
mkdir -p $OUT/rt/usr/share/X11 $OUT/rt/share/X11 $OUT/rt/share/fonts $OUT/rt/etc/fonts
cp -r $ROOTFS/usr/share/X11/xkb $OUT/rt/usr/share/X11/
cp -r $ROOTFS/usr/share/X11/locale $OUT/rt/share/X11/
cp -L $ROOTFS/usr/share/fonts/truetype/dejavu/*.ttf $OUT/rt/share/fonts/
cat > $OUT/rt/etc/fonts/fonts.conf <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$APPDIR/rt/share/fonts</dir>
  <dir>$APPDIR/wine/share/wine/fonts</dir>
  <cachedir>$APPDIR/home/.cache/fontconfig</cachedir>
  <config><rescan><int>0</int></rescan></config>
</fontconfig>
CONF

cp $W/app-src/appinfo.json $W/app-src/*.png $OUT/
du -sh $OUT $OUT/rt $OUT/bin
