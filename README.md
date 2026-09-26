# Wine for webOS TV

Wine runs natively on the TV and runs 32-bit Windows programs, started from an app icon and shown fullscreen on the TV. The target is the OLED55C56LB: webOS TV 25, four Cortex-A76 cores, an aarch64 kernel with a 32-bit ARM (softfp) userspace, and root through Homebrew Channel. `../webos-firefox/findings.md` covers the platform.

App id `com.github.gprot42.wine`. Title "Wine".

## How it works

```
webOS launcher ──> wine-tv (armhf, owns the fullscreen window)
                    ├─ Xvfb :7          X server; its screen is /tmp/wine-tv/Xvfb_screen0
                    ├─ wine explorer /desktop=shell,1280x720
                    │     Wine 11.0 built for ARM64: wineserver, ntdll, the X11 driver and
                    │     Wine's own programs run natively on the Cortex-A76.
                    │     32-bit x86 programs run under Wine's WoW64 layer; only their x86
                    │     code is translated, by FEX (libwow64fex.dll, installed as xtajit.dll).
                    ├─ wine-tv-kbd.exe  native ARM64 Windows helper inside Wine (see below)
                    └─ Wayland client of LSM (wl_shell + wl_webos_shell)
                          buffer = Xvfb's screen file, shared with the compositor, no copy
                          Magic Remote read from evdev -> XTEST; webOS keyboard via text_model
```

- **Two runtimes, none from the TV.** `wine-tv`, Xvfb and xkbcomp are armhf and use `rt/lib`. Wine and the bootstrap tools are aarch64 and use `rt64/lib` (Debian bookworm). Each has its own glibc loader with the app's path as its interpreter. Both loaders ignore the TV's `/etc/ld.so.preload`; the aarch64 one reads `/tmp/wine-tv/prelo` instead (`build/patches/ldso-no-preload.py`).
- **Built for aarch64 and i386.** Wine is built with `--enable-archs=aarch64,i386`. It runs 32-bit x86 Windows programs only. 64-bit x86 programs would need an ARM64EC build of Wine plus FEX's ARM64EC module.
- **The C: drive is built with the app.** `build/make-prefix.sh` boots the prefix in the build container as the TV jail's user (`prisoner`) and turns every system file identical to Wine's own into a symlink into `wine/`. The result, `prefix-template/`, is about 45 MB instead of about 750 MB. `wine-tv` copies it to `home/wine64` on first start.
- **Copy-on-write.** `rt64/lib/libwine-tv-cow.so` is preloaded into every aarch64 program. When an installer overwrites one of those symlinks (d3dx9, vcrun...), it first becomes a private file in the prefix; Wine's own files are root-owned and never touched. Wine's automatic prefix update is disabled (`.update-timestamp` = `disable`); run with copy-on-write, it would copy every DLL.
- **Input.** The compositor sends a native app no Wayland pointer or key events on this TV. So the Magic Remote (pointer, OK, wheel, keys) is read from `/dev/input`, as in the Firefox port. Clicks aimed at the open webOS keyboard stay with the keyboard.
- **Automatic on-screen keyboard.** Clicking while X shows the text I-beam cursor opens the webOS keyboard, which covers Qt programs such as VLC too. `wine-tv-kbd.exe` reports a focused native text field's caret, for fields reached with Tab. Clicking elsewhere closes it.
- **Repainting.** Wine's X11 driver paints the desktop through the windows above it. So `wine-tv-kbd.exe` repaints the desktop and then every window whenever windows or desktop icons change. `wine-tv` also sends the TV one more frame after changes stop, because the compositor reads the shared buffer asynchronously.

## Using it

| Remote | Does |
|---|---|
| Pointer, OK/click | mouse, left button |
| Wheel | scroll wheel |
| Back | Escape (closes the on-screen keyboard if it is open) |
| Red | open/close the webOS keyboard by hand |
| Green | right-click at the pointer |
| Yellow | Alt+Tab |
| Blue | Windows key (Start menu) |
| Arrows, number keys, a USB keyboard | the matching PC keys |

- **Leaving:** Start → Exit desktop, or the TV's Home/Exit button.
- **Drives:** C: is the prefix. D: is `/tmp/usb`, where USB sticks appear as `D:\sda\sda1`, and E: is `/media/internal`. Z: is the whole filesystem.
- **Settings** live in `home/wine-tv.conf`: `size=1280x720` (the desktop size, scaled to the panel by the TV) and `program=...` (opened with the desktop).
- **Launching with a program:**
  ```sh
  luna-send-pub -n 1 'luna://com.webos.applicationManager/launch' \
    '{"id":"com.github.gprot42.wine","params":{"exe":"D:\\sda\\sda1\\setup.exe"}}'
  ```

## Get Apps

The **Get Apps** icon on the desktop (also in Start → Programs → Get Apps) lists runtimes and apps.

- **Clicking an app** installs it the first time and starts it. Once it is installed, clicking just starts it; installers also add their own desktop and Start menu icons.
- **While installing,** a console window shows progress. The whole log is `C:\winetricks.log`, and it opens in Notepad for runtimes and on failure.
- **How a click reaches winetricks:** Wine cannot start a Unix program from a Windows shortcut. Each shortcut drops a request file into `/tmp/wine-tv/req/`, and `wine-tv` runs `tools/bin/wine-tv-install`, which calls winetricks with the bundled bash, curl, cabextract, 7z and GNU text tools.

| Runtimes | Apps (all 32-bit x86, tested on the TV) |
|---|---|
| Core fonts, Visual C++ 2015-2022, DirectX 9, XACT, .NET 4.8 | 7-Zip, Notepad++, VLC, IrfanView, Media Player Classic, foobar2000, Winamp, PuTTY, WinSCP, Internet Explorer (Wine Gecko) |

**Adding an app** takes one line in `build/catalog.tsv`: folder, name, winetricks verb, program to start, and optionally a file that shows it is installed. Where winetricks has no recipe, or picks a 64-bit or outdated build, add `build/verbs/<verb>.verb` with the download URL, SHA-256 and silent-install switch (see `7zip32.verb`). The next build puts it in Get Apps, and `wine-tv` refreshes Get Apps in existing prefixes on start.

**Internet Explorer** is Wine's own, rendering with Wine Gecko 2.47.4, a 2016-era engine. It suits simple pages and programs that embed IE. Sites behind Cloudflare's browser challenge (winehq.org among them) answer 403. For browsing, use the native Firefox port.

## Debugging

- The log is `wine-tv.log` in the app directory.
- `KEY=VALUE` lines in `<app dir>/env` apply to one run, for example `WINEDEBUG=err+all`. Delete the file afterwards.
- **Running a Windows program as the app user from SSH:** `setpriv --reuid 5845 --regid 5000 --clear-groups` with `WINEPREFIX=<app>/home/wine64 DISPLAY=:7`.
- **A screenshot** is `/tmp/wine-tv/Xvfb_screen0` (XWD format).

## Building

Two podman containers share the `ffbuild` volume at `/work`:
- `ffbuild` (Ubuntu 24.04 arm64) builds the armhf part.
- `winebuild` (Debian bookworm arm64) builds Wine natively and supplies the aarch64 runtime.

First time only:

```sh
podman exec ffbuild   bash /work/wine/build/rootfs-armhf.sh      # Debian armhf runtime + sysroot
podman exec ffbuild   bash /work/wine/build/build-xvfb.sh        # Xvfb
podman exec winebuild bash /work/wine/build/build-wine-arm64.sh  # Wine 11.0, aarch64 + i386 PE (~40 min)
podman exec winebuild bash /work/wine/build/build-fex-wow64.sh   # FEX libwow64fex.dll
```

After that:

```sh
./scripts/install2tvfrommacos.sh            # build, pack, install on root@192.168.0.79, launch
INSTALL=0 ./scripts/install2tvfrommacos.sh  # build and pack only
BUILD=0 ./scripts/install2tvfrommacos.sh    # install the last build
./scripts/push-wine-tv.sh                   # replace only wine-tv on the TV and relaunch
```

The package is about 170 MB, and about 850 MB installed. When the TV lacks room to upgrade in place, the install script moves `home/` (the prefix, with everything installed in it) aside on the same disk, reinstalls, and moves it back.

## Not done yet

- **OpenGL / Direct3D.** Xvfb has no GLX, so 3D programs and games do not run. The TV's Mali driver is softfp and 32-bit.
- **64-bit x86 programs** need an ARM64EC build of Wine and FEX.
- **Sound** goes to the TV's PulseAudio socket. It is untested.

## License

MIT, see [LICENSE](LICENSE). This covers this repository's own code. The two protocol descriptions in `src/protocol/` keep their own notices (Apache 2.0 and an MIT-style notice from Intel and LG). Wine, FEX, winetricks and the Debian runtime that the build downloads and packages are under their own licenses (Wine: LGPL 2.1+).
