# Shared settings for the build scripts (run inside the ffbuild container).
APP_ID=com.github.gprot42.wine
APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
W=/work/wine
SYSROOT=$W/sysroot
ROOTFS=$W/rootfs
OUT=$W/app
# The runtime's dynamic loader and library directories, as seen on the TV.
TV_LDSO=$APPDIR/rt/lib/ld-linux-armhf.so.3
TV_RPATH=$APPDIR/rt/lib:$APPDIR/rt/usr/lib
CROSS=arm-linux-gnueabihf
ARMFLAGS="-marm -mcpu=cortex-a76 -mfpu=neon-fp-armv8 -mfloat-abi=hard"
