#define _GNU_SOURCE

/* Copy-on-write for the prefix's symlinked system files.
 *
 * The prefix template (build/make-prefix.sh) makes every system DLL and EXE
 * in C:\windows a symlink into the app's wine/ directory, which is read-only
 * to the app's user. Installers replace some of those files (d3dx9, vcrun,
 * xact...). Opening such a symlink for writing would follow it into wine/ and
 * fail, so this library, preloaded into Wine and the bootstrap tools, first
 * turns the link into a file of the prefix's own: removed when the file is
 * being truncated, a copy of the target otherwise.
 *
 * Only links that point into <app>/wine/ are touched. The aarch64 loader
 * finds this library through /tmp/wine-tv/prelo, written by wine-tv
 * (build/patches/ldso-no-preload.py).
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char app_wine[] = "/media/developer/apps/usr/palm/applications/com.github.gprot42.wine/wine/";

/* Returns 1 if path was a link into wine/ and is now a plain file (or gone,
 * when truncate is set), 0 if it was left alone. */
static int break_link(int dirfd, const char *path, int truncate)
{
    char target[PATH_MAX], tmp[PATH_MAX];
    struct stat st;
    ssize_t len;
    int in, out, saved = errno;

    if (!path || fstatat(dirfd, path, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode))
        goto keep;
    len = readlinkat(dirfd, path, target, sizeof target - 1);
    if (len <= 0)
        goto keep;
    target[len] = '\0';
    if (strncmp(target, app_wine, sizeof app_wine - 1) != 0)
        goto keep;
    if (truncate) {
        unlinkat(dirfd, path, 0);
        errno = saved;
        return 1;
    }
    /* Keep the content: copy the target next to the link, then replace it. */
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.cow%d", path, (int)getpid()) >= sizeof tmp)
        goto keep;
    in = open(target, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        goto keep;
    out = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        goto keep;
    }
    for (;;) {
        char buf[65536];
        ssize_t n = read(in, buf, sizeof buf);
        if (n <= 0)
            break;
        if (write(out, buf, (size_t)n) != n) {
            n = -1;
            break;
        }
    }
    close(in);
    close(out);
    if (renameat(dirfd, tmp, dirfd, path) != 0)
        unlinkat(dirfd, tmp, 0);
    errno = saved;
    return 1;
keep:
    errno = saved;
    return 0;
}

static int writes(int flags)
{
    return (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
}

/* A truncating open of a removed link must create the file again. */
static int fix_flags(int dirfd, const char *path, int flags)
{
    if (writes(flags) && break_link(dirfd, path, flags & O_TRUNC) && (flags & O_TRUNC))
        flags |= O_CREAT;
    return flags;
}

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*creat_fn)(const char *, mode_t);
typedef FILE *(*fopen_fn)(const char *, const char *);

#define REAL(name, type) \
    static type real_##name; \
    if (!real_##name) real_##name = (type)dlsym(RTLD_NEXT, #name)

int open(const char *path, int flags, ...)
{
    mode_t mode = 0644;
    REAL(open, open_fn);

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    flags = fix_flags(AT_FDCWD, path, flags);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0644;
    REAL(open64, open_fn);

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    flags = fix_flags(AT_FDCWD, path, flags);
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0644;
    REAL(openat, openat_fn);

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    flags = fix_flags(dirfd, path, flags);
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0644;
    REAL(openat64, openat_fn);

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    flags = fix_flags(dirfd, path, flags);
    return real_openat64(dirfd, path, flags, mode);
}

int creat(const char *path, mode_t mode)
{
    REAL(creat, creat_fn);

    break_link(AT_FDCWD, path, 1);
    return real_creat(path, mode);
}

static void fopen_mode(const char *path, const char *mode)
{
    if (!mode)
        return;
    if (mode[0] == 'w')
        break_link(AT_FDCWD, path, 1);
    else if (mode[0] == 'a' || strchr(mode, '+'))
        break_link(AT_FDCWD, path, 0);
}

FILE *fopen(const char *path, const char *mode)
{
    REAL(fopen, fopen_fn);

    fopen_mode(path, mode);
    return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    REAL(fopen64, fopen_fn);

    fopen_mode(path, mode);
    return real_fopen64(path, mode);
}
