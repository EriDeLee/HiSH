#ifndef HISH_LINKS_IMPL_H
#define HISH_LINKS_IMPL_H
// The shared part of the emulated symbolic links (see hish_link_core.h for the marker
// format): raw system calls, path preparation and one *_core function per operation.
// Three builds include it and add their own public functions: linux/links.c, preloaded
// into Linux programs (musl and glibc); links_ohos.c with hidden symbols inside
// libhish_shell.so, which captures the libc calls made by HiSH's own shell and Toybox; and
// links_ohos.c with exported symbols inside libhish_exec_bridge.so, which is preloaded into
// the system Toybox applets. The includer defines _GNU_SOURCE first, and HISH_LINKS_STATX
// where struct statx exists.
//
// Everything here talks to the kernel with its own svc wrapper, never through a public
// syscall(), which the includers may replace. HISH_LINKS=0 turns the emulation off.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "hish_link_core.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "emulated symbolic links are implemented for AArch64 only"
#endif
enum {
    HL_GETCWD = 17, HL_MKNODAT = 33, HL_MKDIRAT = 34, HL_UNLINKAT = 35, HL_SYMLINKAT = 36, HL_RENAMEAT = 38,
    HL_TRUNCATE = 45, HL_FACCESSAT = 48, HL_CHDIR = 49, HL_FCHMODAT = 53, HL_FCHOWNAT = 54, HL_OPENAT = 56,
    HL_CLOSE = 57, HL_READ = 63, HL_WRITE = 64, HL_PREAD64 = 67, HL_READLINKAT = 78, HL_NEWFSTATAT = 79, HL_FSTAT = 80,
    HL_UTIMENSAT = 88, HL_EXECVE = 221, HL_RENAMEAT2 = 276, HL_STATX = 291, HL_FACCESSAT2 = 439
};
static inline long raw(long number, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
#define K1(n, a) raw((n), (long)(a), 0, 0, 0, 0, 0)
#define K2(n, a, b) raw((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define K3(n, a, b, c) raw((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define K4(n, a, b, c, d) raw((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define K5(n, a, b, c, d, e) raw((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
// The kernel returns -errno; libc turns that into -1 and errno.
static long finish(long result)
{
    if ((unsigned long)result > (unsigned long)-4096) { errno = (int)-result; return -1; }
    return result;
}

// HISH_LINKS=0 switches the emulation off. The copy exported to system applets
// (HISH_LINKS_START_DISABLED) stays off until the applet launcher turns it on, so that
// the same library loaded in any other process passes every call straight through.
#ifdef HISH_LINKS_START_DISABLED
static int enabled = 0, tracing;
#else
static int enabled = 1, tracing;
#endif
static int links_wanted(void)
{
    const char* value = getenv("HISH_LINKS");
    return !(value && value[0] == '0' && !value[1]);
}
#ifndef HISH_LINKS_START_DISABLED
__attribute__((constructor)) static void links_init(void)
{
    enabled = links_wanted();
    tracing = getenv("HISH_LINKS_TRACE") != NULL;
}
#endif
static void trace(const char* name, const char* path, long result)
{
    if (tracing) dprintf(2, "links: %s %s -> %ld\n", name, path ? path : "(null)", result);
}

static int is_marker_stat(const struct stat* st)
{
    return S_ISREG(st->st_mode) && (st->st_mode & S_ISVTX) && st->st_size > HISH_LINK_MAGIC_LENGTH &&
        st->st_size <= HISH_LINK_MAGIC_LENGTH + HISH_LINK_MAX_TARGET;
}
// The target of the marker file at (dirfd, path): its length, 0 when the bytes are not a marker,
// or -errno. With `target` NULL only the length is wanted.
static long read_marker(int dirfd, const char* path, char* target, size_t capacity)
{
    char bytes[HISH_LINK_MAGIC_LENGTH + HISH_LINK_MAX_TARGET + 1];
    const long fd = K4(HL_OPENAT, dirfd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0);
    if (fd < 0) return fd;
    const long count = K4(HL_PREAD64, fd, bytes, sizeof(bytes), 0);
    K1(HL_CLOSE, fd);
    if (count < 0) return count;
    const char* text;
    size_t length;
    if (!HishLinkParse(bytes, (size_t)count, &text, &length)) return 0;
    if (target) {
        if (length >= capacity) return 0;
        memcpy(target, text, length);
        target[length] = 0;
    }
    return (long)length;
}
static long marker_at(int dirfd, const char* path, char* target, size_t capacity)
{
    struct stat st;
    if (K4(HL_NEWFSTATAT, dirfd, path, &st, AT_SYMLINK_NOFOLLOW) != 0 || !is_marker_stat(&st)) return 0;
    const long length = read_marker(dirfd, path, target, capacity);
    return length > 0 ? length : 0;
}
static int inspect_entry(void* context, const char* path, char* target, size_t capacity, size_t* length)
{
    (void)context;
    const long found = marker_at(AT_FDCWD, path, target, capacity);
    if (found <= 0) return 0;
    *length = (size_t)found;
    return 1;
}
static const HishLinkOps link_ops = {inspect_entry};

// Absolute directory that relative paths against `dirfd` start from.
static int directory_of(int dirfd, char* base)
{
    if (dirfd == AT_FDCWD) {
        const long n = K2(HL_GETCWD, base, PATH_MAX);
        if (n <= 0 || base[0] != '/') return n < 0 ? (int)-n : ENOENT;
        return 0;
    }
    char link[48];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
    const long n = K4(HL_READLINKAT, AT_FDCWD, link, base, PATH_MAX - 1);
    if (n <= 0 || base[0] != '/') return n < 0 ? (int)-n : ENOTDIR;
    base[n] = 0;
    return 0;
}
// Removes every marker from `path`. On success (*use_dirfd, *use_path) is what the kernel can walk.
static int resolve_to(int dirfd, const char* path, int follow_last, char* buffer, int* use_dirfd, const char** use_path)
{
    char base[PATH_MAX];
    if (path[0] != '/') {
        const int error = directory_of(dirfd, base);
        if (error) return error;
    }
    const int error = HishLinkResolve(&link_ops, NULL, path[0] == '/' ? NULL : base, path, follow_last, buffer, PATH_MAX);
    if (error) return error;
    *use_dirfd = AT_FDCWD;
    *use_path = buffer;
    return 0;
}
// For an operation that follows a final link and returns no file mode of its own. Costs one
// extra system call: a look at the final component.
static long prepare_follow(int dirfd, const char* path, char* buffer, int* use_dirfd, const char** use_path)
{
    *use_dirfd = dirfd;
    *use_path = path;
    if (!enabled || !path || !path[0]) return 0;
    struct stat st;
    const long r = K4(HL_NEWFSTATAT, dirfd, path, &st, AT_SYMLINK_NOFOLLOW);
    if (r == 0 ? !is_marker_stat(&st) : r != -ENOTDIR) return 0;
    const int error = resolve_to(dirfd, path, 1, buffer, use_dirfd, use_path);
    return error ? -error : 0;
}

static long openat_core(int dirfd, const char* path, int flags, mode_t mode)
{
    if (!enabled || !path) return K4(HL_OPENAT, dirfd, path, flags, mode);
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    const int creating = (flags & O_CREAT) != 0, exclusive = creating && (flags & O_EXCL);
    // Truncating a marker would destroy the link, so look first.
    if ((flags & O_TRUNC) && !exclusive) {
        const long error = prepare_follow(dirfd, path, buffer, &d, &p);
        return error ? error : K4(HL_OPENAT, d, p, flags, mode);
    }
    long fd = K4(HL_OPENAT, dirfd, path, flags, mode);
    if (fd == -ENOTDIR) {
        const int error = resolve_to(dirfd, path, 1, buffer, &d, &p);
        return error ? -error : K4(HL_OPENAT, d, p, flags, mode);
    }
    if (fd < 0 || exclusive || (flags & O_DIRECTORY)) return fd;
    struct stat st;
    if (K2(HL_FSTAT, fd, &st) != 0 || !is_marker_stat(&st)) return fd;
    if (read_marker(dirfd, path, NULL, 0) <= 0) return fd;
    if ((flags & O_PATH) && (flags & O_NOFOLLOW)) return fd;
    K1(HL_CLOSE, fd);
    if (flags & O_NOFOLLOW) return -ELOOP;
    const int error = resolve_to(dirfd, path, 1, buffer, &d, &p);
    return error ? -error : K4(HL_OPENAT, d, p, flags, mode);
}

// stat of the entry itself first; a marker is reported as a link, or followed.
static long stat_core(int dirfd, const char* path, struct stat* st, int flags)
{
    if (!enabled || !path || ((flags & AT_EMPTY_PATH) && !path[0])) return K4(HL_NEWFSTATAT, dirfd, path, st, flags);
    const int nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
    char first[PATH_MAX], second[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    long r = K4(HL_NEWFSTATAT, d, p, st, flags | AT_SYMLINK_NOFOLLOW);
    if (r == -ENOTDIR) {
        const int error = resolve_to(dirfd, path, nofollow ? 0 : 1, first, &d, &p);
        if (error) return -error;
        r = K4(HL_NEWFSTATAT, d, p, st, flags | AT_SYMLINK_NOFOLLOW);
    }
    if (r < 0) return r;
    if (is_marker_stat(st)) {
        const long length = read_marker(d, p, NULL, 0);
        if (length <= 0) return 0;
        if (nofollow) {
            st->st_mode = S_IFLNK | 0777;
            st->st_size = length;
            return 0;
        }
        const int error = resolve_to(d, p, 1, second, &d, &p);
        return error ? -error : K4(HL_NEWFSTATAT, d, p, st, flags & ~AT_SYMLINK_NOFOLLOW);
    }
    if (!nofollow && S_ISLNK(st->st_mode)) return K4(HL_NEWFSTATAT, d, p, st, flags);
    return 0;
}
#ifdef HISH_LINKS_STATX
static int is_marker_statx(const struct statx* sx)
{
    return S_ISREG(sx->stx_mode) && (sx->stx_mode & S_ISVTX) && sx->stx_size > HISH_LINK_MAGIC_LENGTH &&
        sx->stx_size <= HISH_LINK_MAGIC_LENGTH + HISH_LINK_MAX_TARGET;
}
static long statx_core(int dirfd, const char* path, int flags, unsigned mask, struct statx* sx)
{
    if (!enabled || !path || ((flags & AT_EMPTY_PATH) && !path[0])) return K5(HL_STATX, dirfd, path, flags, mask, sx);
    const int nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
    char first[PATH_MAX], second[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    long r = K5(HL_STATX, d, p, flags | AT_SYMLINK_NOFOLLOW, mask, sx);
    if (r == -ENOTDIR) {
        const int error = resolve_to(dirfd, path, nofollow ? 0 : 1, first, &d, &p);
        if (error) return -error;
        r = K5(HL_STATX, d, p, flags | AT_SYMLINK_NOFOLLOW, mask, sx);
    }
    if (r < 0) return r;
    if (is_marker_statx(sx)) {
        const long length = read_marker(d, p, NULL, 0);
        if (length <= 0) return 0;
        if (nofollow) {
            sx->stx_mode = S_IFLNK | 0777;
            sx->stx_size = (unsigned long long)length;
            return 0;
        }
        const int error = resolve_to(d, p, 1, second, &d, &p);
        return error ? -error : K5(HL_STATX, d, p, flags & ~AT_SYMLINK_NOFOLLOW, mask, sx);
    }
    if (!nofollow && S_ISLNK(sx->stx_mode)) return K5(HL_STATX, d, p, flags, mask, sx);
    return 0;
}
#endif

static long access_core(int dirfd, const char* path, int mode, int flags)
{
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    if (enabled && !(flags & AT_SYMLINK_NOFOLLOW)) {
        const long error = prepare_follow(dirfd, path, buffer, &d, &p);
        if (error) return error;
    }
    long r = flags ? K4(HL_FACCESSAT2, d, p, mode, flags) : K3(HL_FACCESSAT, d, p, mode);
    if (r == -ENOSYS && flags) r = K3(HL_FACCESSAT, d, p, mode);
    return r;
}

static long readlinkat_core(int dirfd, const char* path, char* out, size_t size)
{
    long r = K4(HL_READLINKAT, dirfd, path, out, size);
    if (!enabled || !path) return r;
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    if (r == -ENOTDIR) {
        const int error = resolve_to(dirfd, path, 0, buffer, &d, &p);
        if (error) return -error;
        r = K4(HL_READLINKAT, d, p, out, size);
    }
    if (r != -EINVAL) return r;
    char target[HISH_LINK_MAX_TARGET + 1];
    const long length = marker_at(d, p, target, sizeof(target));
    if (length <= 0) return r;
    const size_t n = (size_t)length < size ? (size_t)length : size;
    memcpy(out, target, n);
    return (long)n;
}

static long symlinkat_core(const char* target, int dirfd, const char* linkpath)
{
    long r = K3(HL_SYMLINKAT, target, dirfd, linkpath);
    if (!enabled) return r;
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = linkpath;
    if (r == -ENOTDIR) {
        const int error = resolve_to(dirfd, linkpath, 0, buffer, &d, &p);
        if (error) return -error;
        r = K3(HL_SYMLINKAT, target, d, p);
    }
    if (r != -EACCES && r != -EPERM && r != -EOPNOTSUPP && r != -ENOSYS) return r;
    // The kernel refuses links here: write the marker instead.
    const size_t length = strlen(target);
    if (!length) return -ENOENT;
    if (length > HISH_LINK_MAX_TARGET) return -ENAMETOOLONG;
    char bytes[HISH_LINK_MAGIC_LENGTH + HISH_LINK_MAX_TARGET];
    const size_t size = HishLinkFormat(target, length, bytes);
    const long fd = K4(HL_OPENAT, d, p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, HISH_LINK_MODE);
    if (fd < 0) return fd;
    size_t done = 0;
    long written = 0;
    while (done < size) {
        written = K3(HL_WRITE, fd, bytes + done, size - done);
        if (written == -EINTR) continue;
        if (written <= 0) break;
        done += (size_t)written;
    }
    K1(HL_CLOSE, fd);
    if (done != size) {
        K3(HL_UNLINKAT, d, p, 0);
        return written < 0 ? written : -EIO;
    }
    return 0;
}

// Operations on the entry itself: only an intermediate marker needs the retry.
static long unlinkat_core(int dirfd, const char* path, int flags)
{
    const long r = K3(HL_UNLINKAT, dirfd, path, flags);
    if (r != -ENOTDIR || !enabled || !path) return r;
    char buffer[PATH_MAX];
    int d;
    const char* p;
    const int error = resolve_to(dirfd, path, 0, buffer, &d, &p);
    return error ? -error : K3(HL_UNLINKAT, d, p, flags);
}
static long mkdirat_core(int dirfd, const char* path, mode_t mode)
{
    const long r = K3(HL_MKDIRAT, dirfd, path, mode);
    if (r != -ENOTDIR || !enabled || !path) return r;
    char buffer[PATH_MAX];
    int d;
    const char* p;
    const int error = resolve_to(dirfd, path, 0, buffer, &d, &p);
    return error ? -error : K3(HL_MKDIRAT, d, p, mode);
}
static long renameat2_core(int olddir, const char* oldpath, int newdir, const char* newpath, unsigned flags)
{
    long r = K5(HL_RENAMEAT2, olddir, oldpath, newdir, newpath, flags);
    if (r == -ENOSYS && !flags) r = K4(HL_RENAMEAT, olddir, oldpath, newdir, newpath);
    if (r != -ENOTDIR || !enabled || !oldpath || !newpath) return r;
    char first[PATH_MAX], second[PATH_MAX];
    int od = olddir, nd = newdir;
    const char *op = oldpath, *np = newpath;
    int error = resolve_to(olddir, oldpath, 0, first, &od, &op);
    if (!error) error = resolve_to(newdir, newpath, 0, second, &nd, &np);
    return error ? -error : K5(HL_RENAMEAT2, od, op, nd, np, flags);
}

// Operations that follow a final link: apply them to the target.
static long fchmodat_core(int dirfd, const char* path, mode_t mode, int flags)
{
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    if (enabled && (flags & AT_SYMLINK_NOFOLLOW)) {
        // Linux cannot change the mode of a link itself.
        struct stat st;
        const long r = K4(HL_NEWFSTATAT, dirfd, path, &st, AT_SYMLINK_NOFOLLOW);
        if (r == 0 && (S_ISLNK(st.st_mode) || is_marker_stat(&st))) return -EOPNOTSUPP;
    } else if (enabled) {
        const long error = prepare_follow(dirfd, path, buffer, &d, &p);
        if (error) return error;
    }
    return K3(HL_FCHMODAT, d, p, mode);
}
static long fchownat_core(int dirfd, const char* path, uid_t owner, gid_t group, int flags)
{
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    if (enabled && !(flags & AT_SYMLINK_NOFOLLOW)) {
        const long error = prepare_follow(dirfd, path, buffer, &d, &p);
        if (error) return error;
    }
    return K5(HL_FCHOWNAT, d, p, owner, group, flags);
}
static long utimensat_core(int dirfd, const char* path, const struct timespec* times, int flags)
{
    char buffer[PATH_MAX];
    int d = dirfd;
    const char* p = path;
    if (enabled && path && !(flags & AT_SYMLINK_NOFOLLOW)) {
        const long error = prepare_follow(dirfd, path, buffer, &d, &p);
        if (error) return error;
    }
    return K4(HL_UTIMENSAT, d, p, times, flags);
}
static long truncate_core(const char* path, long length)
{
    char buffer[PATH_MAX];
    int d;
    const char* p;
    const long error = prepare_follow(AT_FDCWD, path, buffer, &d, &p);
    return error ? error : K2(HL_TRUNCATE, p, length);
}
static long chdir_core(const char* path)
{
    char buffer[PATH_MAX];
    int d;
    const char* p;
    const long error = prepare_follow(AT_FDCWD, path, buffer, &d, &p);
    return error ? error : K1(HL_CHDIR, p);
}
static long execve_core(const char* path, char* const argv[], char* const envp[])
{
    char buffer[PATH_MAX];
    int d;
    const char* p;
    const long error = prepare_follow(AT_FDCWD, path, buffer, &d, &p);
    return error ? error : K3(HL_EXECVE, p, argv, envp);
}
// realpath without dlsym (glibc before 2.34 keeps dlsym in a separate library): replace the
// emulated links, then let the kernel name the file by opening it with O_PATH and reading
// /proc/self/fd/N, which also resolves real links.
static char* links_realpath(const char* path, char* resolved)
{
    if (!path) { errno = EINVAL; return NULL; }
    char buffer[PATH_MAX], name[PATH_MAX], link[48];
    int d;
    const char* p = path;
    if (enabled) {
        const int error = resolve_to(AT_FDCWD, path, 1, buffer, &d, &p);
        if (error) { errno = error; return NULL; }
    }
    const long fd = K4(HL_OPENAT, AT_FDCWD, p, O_PATH | O_CLOEXEC, 0);
    if (fd < 0) { errno = (int)-fd; return NULL; }
    snprintf(link, sizeof(link), "/proc/self/fd/%ld", fd);
    const long n = K4(HL_READLINKAT, AT_FDCWD, link, name, sizeof(name) - 1);
    K1(HL_CLOSE, fd);
    if (n <= 0 || name[0] != '/') { errno = n < 0 ? (int)-n : ENOENT; return NULL; }
    name[n] = 0;
    char* result = resolved ? resolved : (char*)malloc((size_t)n + 1);
    if (!result) return NULL;
    memcpy(result, name, (size_t)n + 1);
    return result;
}
#pragma GCC diagnostic pop
#endif
