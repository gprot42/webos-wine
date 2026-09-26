#!/bin/sh
# Rebuild only wine-tv, copy it over the installed app on the TV, relaunch.
# For quick iteration; the next full install replaces it anyway.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TV_IP=${TV_IP:-192.168.0.79}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
APP=/media/developer/apps/usr/palm/applications/com.github.gprot42.wine
TMP=$(mktemp -d)
cd "$ROOT"
podman exec ffbuild rm -rf /work/wine/src
podman cp src ffbuild:/work/wine/src
podman exec ffbuild bash -c '. /work/wine/build/env.sh; G=$W/gen; $CROSS-gcc --sysroot=$SYSROOT $ARMFLAGS -O2 -Wall -Wno-format-truncation -std=gnu11 -I$G -o /work/wine/app/wine-tv $W/src/wine-tv.c $G/webos-shell-protocol.c $G/text-model-protocol.c -Wl,--dynamic-linker=$TV_LDSO -Wl,--disable-new-dtags -Wl,-rpath,$APPDIR/rt/lib -lwayland-client -lX11 -lXtst -lXdamage -lXfixes && cp /work/wine/app/wine-tv /tmp/wine-tv-bin'
podman cp ffbuild:/tmp/wine-tv-bin "$TMP/wine-tv"
ssh -o BatchMode=yes -i "$SSH_KEY" root@$TV_IP "pkill -f '[w]ine-tv'; true"
scp -o BatchMode=yes -i "$SSH_KEY" "$TMP/wine-tv" root@$TV_IP:$APP/wine-tv.new
ssh -tt -o BatchMode=yes -i "$SSH_KEY" root@$TV_IP "chmod 755 $APP/wine-tv.new && mv -f $APP/wine-tv.new $APP/wine-tv; /usr/bin/luna-send-pub -n 1 -w 15000 'luna://com.webos.applicationManager/launch' '{\"id\":\"com.github.gprot42.wine\"}'" | tail -1
rm -rf "$TMP"
