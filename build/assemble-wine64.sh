#!/bin/bash
# Add native ARM64 Wine and its aarch64 runtime (rt64/) to the app tree.
# Runs in the winebuild container (Debian bookworm arm64) after assemble.sh.
set -euo pipefail
. /work/wine/build/env.sh
WA=$W/wine-arm64/opt/wine
LDSO64=$APPDIR/rt64/lib/ld-linux-aarch64.so.1
export PATH=$W/llvm-mingw/bin:$PATH
rm -rf $OUT/wine $OUT/rt64 && mkdir -p $OUT/wine $OUT/rt64/lib $OUT/rt64/share

echo "== wine (aarch64 + i386 PE)"
cp -r $WA/bin $WA/lib $WA/share $OUT/wine/
rm -rf $OUT/wine/share/man $OUT/wine/share/applications $OUT/wine/include
# Only the runtime: no build tools, no import libraries.
( cd $OUT/wine/bin && rm -f function_grep.pl widl winebuild winecpp winedump wineg++ winegcc winemaker wmc wrc msidb )
find $OUT/wine/lib -name '*.a' -delete
# FEX runs the x86 code of 32-bit programs. Wine loads the WoW64 CPU DLL
# named in HKLM\Software\Microsoft\Wow64\x86, or xtajit.dll (the Windows
# name) when that is unset. Installing it as xtajit.dll means x86 works from
# the prefix's very first boot, which is when the syswow64 DLLs are set up.
cp $W/fex-wow64/libwow64fex.dll $OUT/wine/lib/wine/aarch64-windows/xtajit.dll
# PE files carry DWARF; strip it (the "Wine builtin DLL" stub is kept).
find $OUT/wine/lib/wine/*-windows -type f \( -name '*.dll' -o -name '*.exe' -o -name '*.drv' \
  -o -name '*.sys' -o -name '*.ocx' -o -name '*.cpl' -o -name '*.acm' -o -name '*.ax' -o -name '*.tlb' \) \
  ! -name xtajit.dll -print0 | xargs -0 -P12 -n16 llvm-strip --strip-debug 2>/dev/null || true
find $OUT/wine/lib/wine/aarch64-unix -type f -name '*.so' -exec strip --strip-unneeded {} +
# Executables use the app's own aarch64 loader and libraries.
for f in $(find $OUT/wine/bin $OUT/wine/lib/wine/aarch64-unix -type f); do
  if file "$f" | grep -q "dynamically linked, interpreter"; then
    strip "$f" 2>/dev/null || true
    patchelf --set-interpreter $LDSO64 --force-rpath --set-rpath $APPDIR/rt64/lib "$f"
    echo "  patched $(basename $f)"
  fi
done

echo "== bootstrap tools (winetricks)"
# winetricks and what it runs, aarch64 from this Debian: bash, GNU text tools
# (BusyBox's differ), GNU cp (an aarch64 cp gets the copy-on-write preload;
# the TV's 32-bit BusyBox one does not), cabextract, unzip, 7z, curl + CA bundle.
mkdir -p $OUT/tools/bin $OUT/tools/etc
for b in /usr/bin/bash /usr/bin/cabextract /usr/bin/unzip /usr/bin/curl /usr/bin/cp \
         /usr/bin/sed /usr/bin/grep /usr/bin/gawk; do
  cp $b $OUT/tools/bin/
done
ln -sf gawk $OUT/tools/bin/awk
# p7zip finds 7z.so next to the path it was started by; started through PATH
# that is the current directory ("Codec Load Error: ./7z.so"). Run it by its
# full path from a wrapper, as Debian's /usr/bin/7z does.
mkdir -p $OUT/tools/lib/p7zip
cp /usr/lib/p7zip/7z /usr/lib/p7zip/7z.so $OUT/tools/lib/p7zip/
printf '#!/bin/sh\nexec %s/tools/lib/p7zip/7z "$@"\n' "$APPDIR" > $OUT/tools/bin/7z
cp /etc/ssl/certs/ca-certificates.crt $OUT/tools/etc/
cp $W/winetricks-20260125 $OUT/tools/bin/winetricks
cp $W/src/wine-tv-install $OUT/tools/bin/
# The Get Apps catalog (entry -> program to start) and our own winetricks
# recipes (build/verbs), for wine-tv-install.
cp $W/build/catalog.tsv $OUT/tools/etc/
# Reports text-field focus to wine-tv (src/wine-tv-kbd.c): aarch64 Windows.
aarch64-w64-mingw32-clang -municode -mwindows -O2 -Wall -o $OUT/tools/wine-tv-kbd.exe $W/src/wine-tv-kbd.c -lshell32
rm -rf $OUT/tools/verbs && cp -r $W/build/verbs $OUT/tools/verbs
sed -i "1s|.*|#!$APPDIR/tools/bin/bash|" $OUT/tools/bin/wine-tv-install
chmod 755 $OUT/tools/bin/*
for f in $OUT/tools/bin/* $OUT/tools/lib/p7zip/*; do
  if [ ! -L "$f" ] && file "$f" | grep -q "dynamically linked, interpreter"; then
    strip "$f" 2>/dev/null || true
    patchelf --set-interpreter $LDSO64 --force-rpath --set-rpath $APPDIR/rt64/lib "$f"
  fi
done

echo "== aarch64 libraries"
LIBDIRS="/lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu/pulseaudio"
# Wine's own binaries, plus what its drivers open with dlopen.
python3 $W/build/libclosure.py $OUT/rt64/lib "$LIBDIRS" \
  $(find $OUT/wine/bin $OUT/wine/lib/wine/aarch64-unix $OUT/tools -type f) \
  libX11.so.6 libXext.so.6 libXrender.so.1 libXrandr.so.2 libXi.so.6 libXcursor.so.1 \
  libXinerama.so.1 libXcomposite.so.1 libXfixes.so.3 libXxf86vm.so.1 \
  libfreetype.so.6 libfontconfig.so.1 libasound.so.2 libpulse.so.0 libgnutls.so.30 libdbus-1.so.3 \
  libgcc_s.so.1 libunwind.so.8 libnss_files.so.2 libnss_dns.so.2 libresolv.so.2 libutil.so.1
cp -L /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 $OUT/rt64/lib/
python3 $W/build/patches/ldso-no-preload.py --preload /tmp/wine-tv/prelo $OUT/rt64/lib/ld-linux-aarch64.so.1
# Copy-on-write for the prefix's symlinked system files (src/cow-preload.c),
# preloaded into every aarch64 program through /tmp/wine-tv/prelo.
gcc -shared -fPIC -O2 -Wall -o $OUT/rt64/lib/libwine-tv-cow.so $W/src/cow-preload.c -ldl
# Debian libraries carry their own search paths (libpulse: .../pulseaudio);
# a RUNPATH hides the app's RPATH from their dependencies. Everything is in
# rt64/lib, so drop them.
for f in $OUT/rt64/lib/*.so*; do
  case $f in *ld-linux*) continue ;; esac
  patchelf --remove-rpath "$f" 2>/dev/null || true
done
find $OUT/rt64/lib -name '*.so*' ! -name 'ld-linux*' -exec strip --strip-unneeded {} + 2>/dev/null || true
cp -r /usr/share/alsa $OUT/rt64/share/ 2>/dev/null || true
du -sh $OUT $OUT/wine $OUT/rt64
