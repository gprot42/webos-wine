#!/bin/bash
# Xvfb for the TV (armhf). Its xkbcomp and keyboard data live in the app
# directory, so both paths are compiled in; compiled keymaps go to /tmp.
set -euo pipefail
. /work/wine/build/env.sh
V=${XSERVER_VERSION:-21.1.16}
cd $W
[ -f xorg-server-$V.tar.xz ] || curl -sfLO https://www.x.org/releases/individual/xserver/xorg-server-$V.tar.xz
rm -rf xorg-server-$V && tar xf xorg-server-$V.tar.xz && cd xorg-server-$V
cat > cross.ini <<INI
[binaries]
c = '$CROSS-gcc'
cpp = '$CROSS-g++'
ar = '$CROSS-ar'
strip = '$CROSS-strip'
pkg-config = 'pkg-config'
[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = '$SYSROOT/usr/lib/arm-linux-gnueabihf/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/lib/pkgconfig'
[built-in options]
c_args = ['--sysroot=$SYSROOT', '-marm', '-mcpu=cortex-a76', '-mfpu=neon-fp-armv8', '-mfloat-abi=hard']
c_link_args = ['--sysroot=$SYSROOT', '-Wl,--dynamic-linker=$TV_LDSO', '-Wl,--disable-new-dtags', '-Wl,-rpath,$TV_RPATH']
[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7l'
endian = 'little'
INI
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
meson setup build --cross-file cross.ini --buildtype=release \
  -Dxorg=false -Dxephyr=false -Dxnest=false -Dxvfb=true -Dxquartz=false -Dxwin=false \
  -Dglamor=false -Dglx=false -Ddri1=false -Ddri2=false -Ddri3=false -Ddrm=false \
  -Dudev=false -Dudev_kms=false -Dsystemd_logind=false -Dhal=false -Dpciaccess=false \
  -Dxdmcp=false -Dxdm-auth-1=false -Dsecure-rpc=false -Dlisten_tcp=false -Dlisten_local=true \
  -Dsha1=libgcrypt -Dlibunwind=false -Dlinux_apm=false -Dlinux_acpi=false -Ddocs=false -Ddevel-docs=false \
  -Dxkb_bin_dir=$APPDIR/bin -Dxkb_dir=$APPDIR/rt/usr/share/X11/xkb -Dxkb_output_dir=/tmp/wine-xkb \
  -Ddefault_font_path=built-ins >/tmp/xvfb-meson.log 2>&1 || { tail -30 /tmp/xvfb-meson.log; exit 1; }
ninja -C build hw/vfb/Xvfb 2>&1 | grep -E "error|FAILED" | head -20 || true
file build/hw/vfb/Xvfb
