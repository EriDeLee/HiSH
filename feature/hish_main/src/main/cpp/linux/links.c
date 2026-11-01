// Emulated symbolic links for Linux programs, musl and glibc. This preload library
// replaces the libc functions that take a path, so symlink() creates a marker and every
// other call sees it as a link; the shared logic is in hish_links_impl.h. Calls that libc
// makes to itself (fopen's open, the dynamic linker) are not seen, which is why fopen,
// opendir and realpath are replaced below, and scandir, glob, nftw, ftw and freopen in
// hish_links_libc.h. Programs that issue raw svc instructions bypass all of this and see
// a small plain file.
#define _GNU_SOURCE
#define HISH_LINKS_STATX 1
#include "hish_links_impl.h"

// The exec layer hands a path to the loader itself, so it needs the followed path too.
__attribute__((visibility("default"))) const char* HishLinkExecutablePath(const char* path, char* buffer, size_t capacity)
{
    int d;
    const char* p;
    if (!enabled || !path || capacity < PATH_MAX || prepare_follow(AT_FDCWD, path, buffer, &d, &p)) return path;
    return p;
}

// epoll.c (the same library) fixes epoll for programs that call it through syscall(), which is how Bun registers its polls. It
// is asked first and does not depend on the link emulation being enabled.
extern int hish_epoll_syscall(long number, long a, long b, long c, long d, long e, long f, long* result) __attribute__((weak));

// Raw syscall() callers (Bun, Rust's libc crate) reach the same code. Anything else goes straight through.
long syscall(long number, ...)
{
    va_list arguments;
    va_start(arguments, number);
    const long a = va_arg(arguments, long), b = va_arg(arguments, long), c = va_arg(arguments, long);
    const long d = va_arg(arguments, long), e = va_arg(arguments, long), f = va_arg(arguments, long);
    va_end(arguments);
    long r;
    if (hish_epoll_syscall && hish_epoll_syscall(number, a, b, c, d, e, f, &r)) return finish(r);
    if (!enabled) return finish(raw(number, a, b, c, d, e, f));
    switch (number) {
    case HL_OPENAT: r = openat_core((int)a, (const char*)b, (int)c, (mode_t)d); break;
    case HL_NEWFSTATAT: r = stat_core((int)a, (const char*)b, (struct stat*)c, (int)d); break;
    case HL_STATX: r = statx_core((int)a, (const char*)b, (int)c, (unsigned)d, (struct statx*)e); break;
    case HL_FACCESSAT: r = access_core((int)a, (const char*)b, (int)c, 0); break;
    case HL_FACCESSAT2: r = access_core((int)a, (const char*)b, (int)c, (int)d); break;
    case HL_READLINKAT: r = readlinkat_core((int)a, (const char*)b, (char*)c, (size_t)d); break;
    case HL_SYMLINKAT: r = symlinkat_core((const char*)a, (int)b, (const char*)c); break;
    case HL_UNLINKAT: r = unlinkat_core((int)a, (const char*)b, (int)c); break;
    case HL_MKDIRAT: r = mkdirat_core((int)a, (const char*)b, (mode_t)c); break;
    case HL_RENAMEAT: r = renameat2_core((int)a, (const char*)b, (int)c, (const char*)d, 0); break;
    case HL_RENAMEAT2: r = renameat2_core((int)a, (const char*)b, (int)c, (const char*)d, (unsigned)e); break;
    case HL_FCHMODAT: r = fchmodat_core((int)a, (const char*)b, (mode_t)c, 0); break;
    case HL_FCHOWNAT: r = fchownat_core((int)a, (const char*)b, (uid_t)c, (gid_t)d, (int)e); break;
    case HL_UTIMENSAT: r = utimensat_core((int)a, (const char*)b, (const struct timespec*)c, (int)d); break;
    case HL_TRUNCATE: r = truncate_core((const char*)a, b); break;
    case HL_CHDIR: r = chdir_core((const char*)a); break;
    case HL_EXECVE: r = execve_core((const char*)a, (char* const*)b, (char* const*)c); break;
    default: r = raw(number, a, b, c, d, e, f);
    }
    return finish(r);
}

// Public functions programs call by name. open, openat, readlink and readlinkat live in
// terminal.c and reach the code above through syscall().
#undef stat64
#undef lstat64
#undef fstatat64
#undef open64
#undef openat64
#undef creat64
#undef fopen64
int stat(const char* path, struct stat* st) { trace("stat", path, 0); return (int)finish(stat_core(AT_FDCWD, path, st, 0)); }
int lstat(const char* path, struct stat* st) { trace("lstat", path, 0); return (int)finish(stat_core(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW)); }
int fstatat(int dirfd, const char* path, struct stat* st, int flags) { trace("fstatat", path, 0); return (int)finish(stat_core(dirfd, path, st, flags)); }
#ifdef __GLIBC__
// struct stat64 has the layout of struct stat on 64-bit Linux, but glibc declares it as a separate type.
int stat64(const char* path, struct stat64* st) { return (int)finish(stat_core(AT_FDCWD, path, (struct stat*)st, 0)); }
int lstat64(const char* path, struct stat64* st) { return (int)finish(stat_core(AT_FDCWD, path, (struct stat*)st, AT_SYMLINK_NOFOLLOW)); }
int fstatat64(int dirfd, const char* path, struct stat64* st, int flags) { return (int)finish(stat_core(dirfd, path, (struct stat*)st, flags)); }
#else
int stat64(const char* path, struct stat* st) __attribute__((alias("stat")));
int lstat64(const char* path, struct stat* st) __attribute__((alias("lstat")));
int fstatat64(int dirfd, const char* path, struct stat* st, int flags) __attribute__((alias("fstatat")));
#endif
int statx(int dirfd, const char* path, int flags, unsigned mask, struct statx* sx) { trace("statx", path, 0); return (int)finish(statx_core(dirfd, path, flags, mask, sx)); }
int access(const char* path, int mode) { return (int)finish(access_core(AT_FDCWD, path, mode, 0)); }
int faccessat(int dirfd, const char* path, int mode, int flags) { return (int)finish(access_core(dirfd, path, mode, flags)); }
int eaccess(const char* path, int mode) { return (int)finish(access_core(AT_FDCWD, path, mode, AT_EACCESS)); }
int euidaccess(const char* path, int mode) __attribute__((alias("eaccess")));
int symlink(const char* target, const char* linkpath) { trace("symlink", linkpath, 0); return (int)finish(symlinkat_core(target, AT_FDCWD, linkpath)); }
int symlinkat(const char* target, int dirfd, const char* linkpath) { trace("symlinkat", linkpath, 0); return (int)finish(symlinkat_core(target, dirfd, linkpath)); }
int unlink(const char* path) { return (int)finish(unlinkat_core(AT_FDCWD, path, 0)); }
int unlinkat(int dirfd, const char* path, int flags) { return (int)finish(unlinkat_core(dirfd, path, flags)); }
int rmdir(const char* path) { return (int)finish(unlinkat_core(AT_FDCWD, path, AT_REMOVEDIR)); }
int mkdir(const char* path, mode_t mode) { return (int)finish(mkdirat_core(AT_FDCWD, path, mode)); }
int mkdirat(int dirfd, const char* path, mode_t mode) { return (int)finish(mkdirat_core(dirfd, path, mode)); }
int rename(const char* from, const char* to) { return (int)finish(renameat2_core(AT_FDCWD, from, AT_FDCWD, to, 0)); }
int renameat(int olddir, const char* from, int newdir, const char* to) { return (int)finish(renameat2_core(olddir, from, newdir, to, 0)); }
int renameat2(int olddir, const char* from, int newdir, const char* to, unsigned flags) { return (int)finish(renameat2_core(olddir, from, newdir, to, flags)); }
int chmod(const char* path, mode_t mode) { return (int)finish(fchmodat_core(AT_FDCWD, path, mode, 0)); }
int fchmodat(int dirfd, const char* path, mode_t mode, int flags) { return (int)finish(fchmodat_core(dirfd, path, mode, flags)); }
int chown(const char* path, uid_t owner, gid_t group) { return (int)finish(fchownat_core(AT_FDCWD, path, owner, group, 0)); }
int lchown(const char* path, uid_t owner, gid_t group) { return (int)finish(fchownat_core(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW)); }
int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags) { return (int)finish(fchownat_core(dirfd, path, owner, group, flags)); }
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) { return (int)finish(utimensat_core(dirfd, path, times, flags)); }
int utimes(const char* path, const struct timeval times[2])
{
    struct timespec spec[2];
    if (times) {
        for (int i = 0; i < 2; i++) {
            if (times[i].tv_usec < 0 || times[i].tv_usec >= 1000000) { errno = EINVAL; return -1; }
            spec[i].tv_sec = times[i].tv_sec;
            spec[i].tv_nsec = times[i].tv_usec * 1000;
        }
    }
    return (int)finish(utimensat_core(AT_FDCWD, path, times ? spec : NULL, 0));
}
int truncate(const char* path, off_t length) { return (int)finish(truncate_core(path, length)); }
int chdir(const char* path) { return (int)finish(chdir_core(path)); }

int creat(const char* path, mode_t mode) { return (int)finish(openat_core(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode)); }
int creat64(const char* path, mode_t mode) __attribute__((alias("creat")));
int open64(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return open(path, flags, mode);
}
int openat64(int dirfd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return openat(dirfd, path, flags, mode);
}
FILE* fopen(const char* path, const char* mode)
{
    if (!mode || !*mode || !strchr("rwa", *mode)) { errno = EINVAL; return NULL; }
    int flags = strchr(mode, '+') ? O_RDWR : *mode == 'r' ? O_RDONLY : O_WRONLY;
    if (strchr(mode, 'x')) flags |= O_EXCL;
    if (strchr(mode, 'e')) flags |= O_CLOEXEC;
    if (*mode != 'r') flags |= O_CREAT;
    if (*mode == 'w') flags |= O_TRUNC;
    if (*mode == 'a') flags |= O_APPEND;
    trace("fopen", path, 0);
    const int fd = (int)finish(openat_core(AT_FDCWD, path, flags, 0666));
    if (fd < 0) return NULL;
    FILE* file = fdopen(fd, mode);
    if (!file) { const int saved = errno; K1(HL_CLOSE, fd); errno = saved; }
    return file;
}
FILE* fopen64(const char* path, const char* mode) __attribute__((alias("fopen")));
static DIR* links_opendir(const char* path)
{
    const int fd = (int)finish(openat_core(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0));
    if (fd < 0) return NULL;
    DIR* directory = fdopendir(fd);
    if (!directory) { const int saved = errno; K1(HL_CLOSE, fd); errno = saved; }
    return directory;
}
DIR* opendir(const char* path) { trace("opendir", path, 0); return links_opendir(path); }
char* realpath(const char* path, char* resolved) { return links_realpath(path, resolved); }

#ifdef __GLIBC__
// glibc before 2.33 reaches stat through these versioned entry points and later versions
// call stat directly; both are covered. The fortified open variants are what a program built
// with _FORTIFY_SOURCE calls instead of open. None of them takes the inline stat of the
// glibc headers, which would call back into __xstat.
int __xstat(int version, const char* path, struct stat* st) { (void)version; return (int)finish(stat_core(AT_FDCWD, path, st, 0)); }
int __lxstat(int version, const char* path, struct stat* st) { (void)version; return (int)finish(stat_core(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW)); }
int __fxstatat(int version, int dirfd, const char* path, struct stat* st, int flags) { (void)version; return (int)finish(stat_core(dirfd, path, st, flags)); }
int __xstat64(int version, const char* path, struct stat* st) __attribute__((alias("__xstat")));
int __lxstat64(int version, const char* path, struct stat* st) __attribute__((alias("__lxstat")));
int __fxstatat64(int version, int dirfd, const char* path, struct stat* st, int flags) __attribute__((alias("__fxstatat")));
int __open_2(const char* path, int flags) { return open(path, flags); }
int __open64_2(const char* path, int flags) { return open(path, flags); }
int __openat_2(int dirfd, const char* path, int flags) { return openat(dirfd, path, flags); }
int __openat64_2(int dirfd, const char* path, int flags) { return openat(dirfd, path, flags); }
#endif

#include "hish_links_libc.h"
