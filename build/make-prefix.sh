#!/bin/bash
# Build the Wine prefix (C: drive) once, at build time, and ship it in the
# app as prefix-template/. Runs in the winebuild container after
# assemble-wine64.sh.
#
# Why: a prefix created on the TV copies every builtin DLL into it (about
# 500 MB, which filled the TV's developer partition), and its first boot is
# slow. Here the prefix is booted with the same Wine and FEX, as the same
# user name the TV's app jail uses ("prisoner"). Then every file identical
# to one in the app's wine/ directory becomes a symlink to it, so the
# template is a few MB and follows app updates. wine-tv copies it into
# home/wine64 on first launch.
set -euo pipefail
. /work/wine/build/env.sh
T=$W/prefix-build
id prisoner >/dev/null 2>&1 || useradd -M -d /tmp/prisoner-home -s /bin/bash prisoner
# The app at its TV path, so the loader paths compiled into Wine resolve.
mkdir -p "$(dirname $APPDIR)" && ln -sfn $OUT $APPDIR
rm -rf $T && mkdir -p $T /tmp/prisoner-home && chown prisoner $T /tmp/prisoner-home
su prisoner -c "export WINEPREFIX=$T/wine64 HOME=/tmp/prisoner-home WINEDEBUG=-all \
  WINEDLLOVERRIDES='mscoree=;winemenubuilder.exe=d' FONTCONFIG_FILE=$OUT/rt/etc/fonts/fonts.conf; \
  cd /tmp && timeout 900 $APPDIR/wine/bin/wine wineboot --init && $APPDIR/wine/bin/wineserver -w" \
  2>&1 | grep -vE "winebth|scmdatabase|winemenubuilder|do_file_copyW|vulkan|nodrv|explorer process" | tail -5 || true
# Bootstrap icons. Each shortcut drops a request file that wine-tv picks up
# and hands to wine-tv-install (winetricks): Wine cannot start a Unix program
# from a Windows shortcut itself. The shortcuts live in the Start menu's
# "Get Apps" folder; a "Get Apps" icon on the desktop opens that folder.
# Also make "shell" the default desktop, so winetricks' installers and Notepad,
# started from outside explorer, open inside the TV's Wine desktop.
python3 - "$W/build/catalog.tsv" > $T/shortcuts.vbs <<'PY'
import sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
print('Set sh = CreateObject("WScript.Shell")')
print('Set fs = CreateObject("Scripting.FileSystemObject")')
print('Sub MkDirs(p)\n  If Not fs.FolderExists(p) Then\n    MkDirs fs.GetParentFolderName(p)\n    fs.CreateFolder p\n  End If\nEnd Sub')
print('Sub Link(dir, name, verb, icon)\n  MkDirs dir\n  Set l = sh.CreateShortcut(dir & "\\" & name & ".lnk")\n'
      '  l.TargetPath = "C:\\windows\\system32\\cmd.exe"\n'
      '  l.Arguments = "/c mkdir Z:\\tmp\\wine-tv\\req 2>nul & echo.>Z:\\tmp\\wine-tv\\req\\" & verb\n'
      '  l.Description = "Install " & name & " (winetricks " & verb & ")"\n'
      '  l.IconLocation = icon\n  l.WindowStyle = 7\n  l.Save\nEnd Sub')
root = 'C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Get Apps'
for folder, name, verb, *_ in rows:
    icon = "C:\\windows\\system32\\appwiz.cpl,0"
    print('Link "%s\\%s", "%s", "%s", "%s"' % (root, folder, name.replace('"', ''), verb, icon))
# The desktop shows only .lnk files placed directly on it (not folders), so
# one icon there opens the Get Apps folder in Wine's Explorer.
q = '""'
print('Set l = sh.CreateShortcut("C:\\users\\Public\\Desktop\\Get Apps.lnk")')
print('l.TargetPath = "C:\\windows\\explorer.exe"')
print('l.Arguments = "/n,' + q + root + q + '"')
print('l.Description = "Install runtimes and apps with winetricks"')
print('l.IconLocation = "C:\\windows\\system32\\appwiz.cpl,0"')
print('l.Save')
PY
su prisoner -c "export WINEPREFIX=$T/wine64 HOME=/tmp/prisoner-home WINEDEBUG=-all; cd /tmp && \
  $APPDIR/wine/bin/wine wscript //B 'Z:$T/shortcuts.vbs' && \
  $APPDIR/wine/bin/wine reg add 'HKCU\\Software\\Wine\\Explorer' /v Desktop /d shell /f && \
  $APPDIR/wine/bin/wine reg add 'HKCU\\Software\\Wine\\Explorer\\Desktops' /v shell /d 1280x720 /f && \
  $APPDIR/wine/bin/wine reg add 'HKCU\\Software\\Microsoft\\Internet Explorer\\Main' /v 'Start Page' /d about:blank /f && \
  $APPDIR/wine/bin/wineserver -w" 2>&1 | grep -vE "^$|nodrv|explorer process" | tail -3 || true
echo "shortcuts: $(find "$T/wine64/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Get Apps" -name '*.lnk' | wc -l) in Get Apps; desktop: $(ls "$T/wine64/drive_c/users/Public/Desktop")"
rm -f $T/shortcuts.vbs
echo "syswow64: $(ls $T/wine64/drive_c/windows/syswow64 | wc -l) files, system32: $(ls $T/wine64/drive_c/windows/system32 | wc -l) files"
[ "$(ls $T/wine64/drive_c/windows/syswow64 | wc -l)" -gt 100 ] || { echo "syswow64 was not set up: x86 support failed"; exit 1; }
du -sh $T/wine64
python3 - "$T/wine64" "$OUT" "$APPDIR" <<'PY'
import filecmp, os, sys
prefix, out, appdir = sys.argv[1:]
libs = {"i386": f"{out}/wine/lib/wine/i386-windows", "aarch64": f"{out}/wine/lib/wine/aarch64-windows"}
fonts = f"{out}/wine/share/wine/fonts"
saved = linked = 0
for root, dirs, files in os.walk(os.path.join(prefix, "drive_c/windows")):
    arch = "i386" if "/syswow64" in root else "aarch64"
    for name in files:
        path = os.path.join(root, name)
        if os.path.islink(path):
            continue
        for src_dir in (libs[arch], fonts):
            src = os.path.join(src_dir, name)
            if os.path.isfile(src) and filecmp.cmp(src, path, shallow=False):
                saved += os.path.getsize(path)
                os.unlink(path)
                os.symlink(appdir + src[len(out):], path)
                linked += 1
                break
print(f"{linked} files became symlinks, {saved / 1e6:.0f} MB saved")
PY
# No automatic prefix update when Wine's wine.inf changes (an app update):
# the template is made for the Wine it ships with, and an update rewrites
# every system DLL, which the copy-on-write preload would turn from symlinks
# into full copies (it filled the TV once).
echo disable > $T/wine64/.update-timestamp
rm -rf $OUT/prefix-template && mv $T/wine64 $OUT/prefix-template
du -sh $OUT/prefix-template
