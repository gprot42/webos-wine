#!/usr/bin/env python3
"""Pack an app directory into a webOS IPK with real timestamps and modes.

Usage: pack-ipk.py <app dir> <dist dir>
The version comes from <app dir>/appinfo.json. Symlinks are stored as
symlinks; executable bits are kept. Layout follows ares-package for a native
app (usr/palm/applications/<id>, usr/palm/packages/<id>/packageinfo.json).
"""
import io
import json
import os
import sys
import tarfile
import time
from pathlib import Path

SKIP_TOP = {"home", "env", "wine-tv.log", "wine-tv.log.1"}


def ar_header(name, size):
    h = (name.encode().ljust(16) + str(int(time.time())).encode().ljust(12) + b"0".ljust(6)
         + b"0".ljust(6) + b"100644".ljust(8) + str(size).encode().ljust(10) + b"`\n")
    assert len(h) == 60
    return h


def dir_entry(tar, name, mode=0o755):
    info = tarfile.TarInfo(name)
    info.type = tarfile.DIRTYPE
    info.mode = mode
    info.mtime = int(time.time())
    tar.addfile(info)


def main():
    app, dist = Path(sys.argv[1]), Path(sys.argv[2])
    appinfo = json.loads((app / "appinfo.json").read_text())
    app_id, version = appinfo["id"], appinfo["version"]
    base = f"usr/palm/applications/{app_id}"
    installed = 0
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz", format=tarfile.GNU_FORMAT, compresslevel=6) as tar:
        for d in ("usr", "usr/palm", "usr/palm/applications", "usr/palm/packages",
                  f"usr/palm/packages/{app_id}"):
            dir_entry(tar, d)
        # The app runs as a jail user, not root; it keeps its log and Wine
        # prefix (home/) in its own directory, so that must be writable.
        dir_entry(tar, base, 0o777)
        for root, dirs, files in os.walk(app):
            rel_root = Path(root).relative_to(app)
            if rel_root == Path("."):
                dirs[:] = [d for d in dirs if d not in SKIP_TOP]
                files = [f for f in files if f not in SKIP_TOP]
            dirs.sort()
            for name in sorted(dirs) + sorted(files):
                path = Path(root) / name
                rel = (rel_root / name).as_posix()
                if name == ".DS_Store":
                    continue
                st = path.lstat()
                info = tarfile.TarInfo(f"{base}/{rel}")
                info.mtime = int(st.st_mtime)
                if path.is_symlink():
                    info.type = tarfile.SYMTYPE
                    info.linkname = os.readlink(path)
                    info.mode = 0o777
                    tar.addfile(info)
                elif path.is_dir():
                    info.type = tarfile.DIRTYPE
                    info.mode = 0o755
                    tar.addfile(info)
                else:
                    info.size = st.st_size
                    info.mode = 0o755 if st.st_mode & 0o111 else 0o644
                    installed += st.st_size
                    with path.open("rb") as f:
                        tar.addfile(info, f)
        pkg = (json.dumps({"id": app_id, "version": version, "app": app_id}, indent=2) + "\n").encode()
        info = tarfile.TarInfo(f"usr/palm/packages/{app_id}/packageinfo.json")
        info.size, info.mode, info.mtime = len(pkg), 0o644, int(time.time())
        tar.addfile(info, io.BytesIO(pkg))

    control_text = "\n".join([
        f"Package: {app_id}", f"Version: {version}", "Section: misc", "Priority: optional",
        "Architecture: arm", f"Installed-Size: {installed}",
        "Maintainer: N/A <nobody@example.com>", "Description: This is a webOS application.",
        "webOS-Package-Format-Version: 2", "webOS-Packager-Version: webos-wine", "",
    ]).encode()
    control = io.BytesIO()
    with tarfile.open(fileobj=control, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        info = tarfile.TarInfo("control")
        info.size, info.mode, info.mtime = len(control_text), 0o644, int(time.time())
        tar.addfile(info, io.BytesIO(control_text))

    dist.mkdir(parents=True, exist_ok=True)
    ipk = dist / f"{app_id}_{version}_arm.ipk"
    with ipk.open("wb") as out:
        out.write(b"!<arch>\n")
        for name, blob in (("debian-binary", b"2.0\n"), ("control.tar.gz", control.getvalue()),
                           ("data.tar.gz", data.getvalue())):
            out.write(ar_header(name, len(blob)))
            out.write(blob)
            if len(blob) % 2:
                out.write(b"\n")
    print(f"{ipk} ({ipk.stat().st_size / 1e6:.1f} MB, {installed / 1e6:.0f} MB installed)")


if __name__ == "__main__":
    main()
