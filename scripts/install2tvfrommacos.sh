#!/bin/sh
# Build the Wine app in the ffbuild container, pack the IPK, and install it
# on the rooted TV when it answers.
#   ./scripts/install2tvfrommacos.sh            build, install, launch
#   BUILD=0 ./scripts/install2tvfrommacos.sh    reuse the last app tree
#   LAUNCH=0 ...                                 install only
#   INSTALL=0 ...                                build and pack only
# The first build also needs, once:
#   in ffbuild:   build/rootfs-armhf.sh build/build-xvfb.sh
#   in winebuild: build/build-wine-arm64.sh build/build-fex-wow64.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TV_IP=${TV_IP:-192.168.0.79}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
CONTAINER=${CONTAINER:-ffbuild}
WINE_CONTAINER=${WINE_CONTAINER:-winebuild}
BUILD=${BUILD:-1}
LAUNCH=${LAUNCH:-1}
APP_ID=com.github.gprot42.wine
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -i $SSH_KEY root@$TV_IP"

cd "$ROOT"
# A new version every build: webOS may skip a package whose version is installed.
python3 - <<'PY'
import json, pathlib
p = pathlib.Path("app/appinfo.json")
a = json.loads(p.read_text())
v = a["version"].split(".")
v[-1] = str(int(v[-1]) + 1)
a["version"] = ".".join(v)
p.write_text(json.dumps(a, indent=2) + "\n")
print("version", a["version"])
PY
VERSION=$(python3 -c 'import json; print(json.load(open("app/appinfo.json"))["version"])')

python3 scripts/make-icons.py
podman exec "$CONTAINER" rm -rf /work/wine/src /work/wine/app-src
podman cp src "$CONTAINER":/work/wine/src
podman cp app "$CONTAINER":/work/wine/app-src
podman cp build/. "$CONTAINER":/work/wine/build/
podman cp scripts/pack-ipk.py "$CONTAINER":/work/wine/build/
if [ "$BUILD" = "1" ]; then
    podman exec "$CONTAINER" bash /work/wine/build/assemble.sh
    podman exec "$WINE_CONTAINER" bash /work/wine/build/assemble-wine64.sh
    podman exec "$WINE_CONTAINER" bash /work/wine/build/make-prefix.sh
else
    podman exec "$CONTAINER" cp /work/wine/app-src/appinfo.json /work/wine/app/
fi
podman exec "$CONTAINER" python3 /work/wine/build/pack-ipk.py /work/wine/app /work/wine/dist
mkdir -p dist
IPK=dist/${APP_ID}_${VERSION}_arm.ipk
podman cp "$CONTAINER":/work/wine/$IPK "$IPK"
echo "built $IPK"
[ "${INSTALL:-1}" = "1" ] || exit 0

if ! $SSH true >/dev/null 2>&1; then
    echo "root@$TV_IP did not answer; install later by rerunning with BUILD=0."
    exit 0
fi

REMOTE=/media/developer/temp/$(basename "$IPK")
APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
# webOS unpacks the new version before removing the old one. When the TV has
# no room for both, keep the user's data (home/: Wine prefix and settings)
# aside on the same disk, remove the app, install, and put home/ back.
need_mb=$(( $(stat -f%z "$IPK" 2>/dev/null || stat -c%s "$IPK") * 6 / 1048576 ))
free_mb=$($SSH "df -m /media/developer | awk 'NR==2{print \$4}'")
SAVED_HOME=0
if [ "$free_mb" -lt "$need_mb" ] && $SSH "test -d $APPDIR"; then
    echo "only ${free_mb} MB free (${need_mb} MB needed): reinstalling with home/ kept aside"
    # Kept separate: this command's own text must not match its pattern.
    $SSH "for p in \$(ps -ef | grep -E '[w]ine-tv [{]|[X]vfb :7|[a]arch64-unix/wine|[b]in/wineserver' | awk '{print \$2}'); do kill -9 \$p; done" || true
    $SSH "rm -rf /media/developer/wine-tv-home; test -d $APPDIR/home && mv $APPDIR/home /media/developer/wine-tv-home"
    SAVED_HOME=1
    ssh -tt -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@$TV_IP" \
        "/usr/bin/luna-send-pub -n 5 -w 60000 'luna://com.webos.appInstallService/dev/remove' '{\"id\":\"$APP_ID\",\"subscribe\":true}'" >/dev/null 2>&1 || true
fi
$SSH "mkdir -p /media/developer/temp && rm -f /media/developer/temp/${APP_ID}_*.ipk"
scp -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "$IPK" "root@$TV_IP:$REMOTE"
# The app's home/ (the Wine prefix) is not in the package; it survives reinstalling.
$SSH "pkill -f '[w]ine-tv'; true"
payload=$(printf '{"id":"com.ares.defaultName","ipkUrl":"%s","subscribe":true}' "$REMOTE")
# Old luna-send-pub on this firmware takes the URI and payload as positional
# arguments and needs a tty to print anything.
ssh -tt -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@$TV_IP" \
    "/usr/bin/luna-send-pub -n 60 -w 180000 'luna://com.webos.appInstallService/dev/install' '$payload'" \
    | tr -d '\r' | grep -m1 -E '"state":"(installed|failed)"' || true
$SSH "grep -q '\"version\": *\"$VERSION\"' /media/developer/apps/usr/palm/applications/$APP_ID/appinfo.json" \
    || { echo "install did not reach version $VERSION" >&2; exit 1; }
$SSH "rm -f $REMOTE"
if [ "$SAVED_HOME" = 1 ]; then
    $SSH "test -d /media/developer/wine-tv-home && mv /media/developer/wine-tv-home $APPDIR/home" && echo "home/ restored"
fi
echo "installed $VERSION on $TV_IP"

if [ "$LAUNCH" = "1" ]; then
    ssh -tt -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@$TV_IP" \
        "/usr/bin/luna-send-pub -n 1 -w 15000 'luna://com.webos.applicationManager/launch' '{\"id\":\"$APP_ID\"}'" || true
    echo "log: /media/developer/apps/usr/palm/applications/$APP_ID/wine-tv.log"
fi
