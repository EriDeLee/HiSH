// Emulated symbolic links for code that runs in OpenHarmony processes. The same file is
// compiled twice, both times defining the libc names the code below replaces:
//  - hidden, inside libhish_shell.so (mksh, the embedded Toybox applets, pkg and the
//    loaders). The linker binds every call made inside that one library to the code here
//    and nothing outside it: libc++ and the system libraries still reach libc directly,
//    and no new symbol is exported.
//  - exported (HISH_LINKS_EXPORT), inside libhish_exec_bridge.so, which is preloaded into
//    the system Toybox applets (stat, find, cp, ...) so that they see the links too. That
//    copy stays off until HishLinksEnableForApplet() runs, so the library does nothing in
//    the other processes that load it (the kernel exec bridge for Linux programs).
// The shared logic is in hish_links_impl.h; the counterpart for Linux programs is
// linux/links.c. The emulator ABI (x86_64) is built too; there the file is nearly empty,
// as the layer is AArch64 only.
#if defined(__aarch64__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HISH_LINKS_EXPORT
#define HISH_LINKS_START_DISABLED 1
#define HIDDEN __attribute__((visibility("default")))
#else
#define HIDDEN __attribute__((visibility("hidden")))
#endif
#include "hish_links_impl.h"

HIDDEN int stat(const char* path, struct stat* st) { return (int)finish(stat_core(AT_FDCWD, path, st, 0)); }
HIDDEN int lstat(const char* path, struct stat* st) { return (int)finish(stat_core(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW)); }
HIDDEN int fstatat(int dirfd, const char* path, struct stat* st, int flags) { return (int)finish(stat_core(dirfd, path, st, flags)); }
HIDDEN int access(const char* path, int mode) { return (int)finish(access_core(AT_FDCWD, path, mode, 0)); }
HIDDEN int faccessat(int dirfd, const char* path, int mode, int flags) { return (int)finish(access_core(dirfd, path, mode, flags)); }
HIDDEN int open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return (int)finish(openat_core(AT_FDCWD, path, flags, mode));
}
HIDDEN int openat(int dirfd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return (int)finish(openat_core(dirfd, path, flags, mode));
}
HIDDEN int creat(const char* path, mode_t mode) { return (int)finish(openat_core(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode)); }
HIDDEN ssize_t readlink(const char* path, char* buffer, size_t size) { return (ssize_t)finish(readlinkat_core(AT_FDCWD, path, buffer, size)); }
HIDDEN ssize_t readlinkat(int dirfd, const char* path, char* buffer, size_t size) { return (ssize_t)finish(readlinkat_core(dirfd, path, buffer, size)); }
HIDDEN int symlink(const char* target, const char* linkpath) { return (int)finish(symlinkat_core(target, AT_FDCWD, linkpath)); }
HIDDEN int symlinkat(const char* target, int dirfd, const char* linkpath) { return (int)finish(symlinkat_core(target, dirfd, linkpath)); }
HIDDEN int unlink(const char* path) { return (int)finish(unlinkat_core(AT_FDCWD, path, 0)); }
HIDDEN int unlinkat(int dirfd, const char* path, int flags) { return (int)finish(unlinkat_core(dirfd, path, flags)); }
HIDDEN int rmdir(const char* path) { return (int)finish(unlinkat_core(AT_FDCWD, path, AT_REMOVEDIR)); }
HIDDEN int mkdir(const char* path, mode_t mode) { return (int)finish(mkdirat_core(AT_FDCWD, path, mode)); }
HIDDEN int mkdirat(int dirfd, const char* path, mode_t mode) { return (int)finish(mkdirat_core(dirfd, path, mode)); }
HIDDEN int rename(const char* from, const char* to) { return (int)finish(renameat2_core(AT_FDCWD, from, AT_FDCWD, to, 0)); }
HIDDEN int renameat(int olddir, const char* from, int newdir, const char* to) { return (int)finish(renameat2_core(olddir, from, newdir, to, 0)); }
HIDDEN int chmod(const char* path, mode_t mode) { return (int)finish(fchmodat_core(AT_FDCWD, path, mode, 0)); }
HIDDEN int fchmodat(int dirfd, const char* path, mode_t mode, int flags) { return (int)finish(fchmodat_core(dirfd, path, mode, flags)); }
HIDDEN int chown(const char* path, uid_t owner, gid_t group) { return (int)finish(fchownat_core(AT_FDCWD, path, owner, group, 0)); }
HIDDEN int lchown(const char* path, uid_t owner, gid_t group) { return (int)finish(fchownat_core(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW)); }
HIDDEN int fchownat(int dirfd, const char* path, uid_t owner, gid_t group, int flags) { return (int)finish(fchownat_core(dirfd, path, owner, group, flags)); }
HIDDEN int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) { return (int)finish(utimensat_core(dirfd, path, times, flags)); }
HIDDEN int truncate(const char* path, off_t length) { return (int)finish(truncate_core(path, length)); }
HIDDEN int chdir(const char* path) { return (int)finish(chdir_core(path)); }
HIDDEN FILE* fopen(const char* path, const char* mode)
{
    if (!mode || !*mode || !strchr("rwa", *mode)) { errno = EINVAL; return NULL; }
    int flags = strchr(mode, '+') ? O_RDWR : *mode == 'r' ? O_RDONLY : O_WRONLY;
    if (strchr(mode, 'x')) flags |= O_EXCL;
    if (strchr(mode, 'e')) flags |= O_CLOEXEC;
    if (*mode != 'r') flags |= O_CREAT;
    if (*mode == 'w') flags |= O_TRUNC;
    if (*mode == 'a') flags |= O_APPEND;
    const int fd = (int)finish(openat_core(AT_FDCWD, path, flags, 0666));
    if (fd < 0) return NULL;
    FILE* file = fdopen(fd, mode);
    if (!file) { const int saved = errno; K1(HL_CLOSE, fd); errno = saved; }
    return file;
}
HIDDEN DIR* opendir(const char* path)
{
    const int fd = (int)finish(openat_core(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0));
    if (fd < 0) return NULL;
    DIR* directory = fdopendir(fd);
    if (!directory) { const int saved = errno; K1(HL_CLOSE, fd); errno = saved; }
    return directory;
}
HIDDEN char* realpath(const char* path, char* resolved) { return links_realpath(path, resolved); }
// Code that calls syscall() directly (the TTY layer's open, for one) gets the same treatment.
HIDDEN long syscall(long number, ...)
{
    va_list arguments;
    va_start(arguments, number);
    const long a = va_arg(arguments, long), b = va_arg(arguments, long), c = va_arg(arguments, long);
    const long d = va_arg(arguments, long), e = va_arg(arguments, long), f = va_arg(arguments, long);
    va_end(arguments);
    long r;
    if (!enabled) return finish(raw(number, a, b, c, d, e, f));
    switch (number) {
    case HL_OPENAT: r = openat_core((int)a, (const char*)b, (int)c, (mode_t)d); break;
    case HL_NEWFSTATAT: r = stat_core((int)a, (const char*)b, (struct stat*)c, (int)d); break;
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
    default: r = raw(number, a, b, c, d, e, f);
    }
    return finish(r);
}
#ifdef HISH_LINKS_EXPORT
// Called by the library's constructor once it knows it runs inside a system applet that the
// app started on purpose (see exec_bridge.cpp). HISH_LINKS=0 in the caller's environment still wins.
__attribute__((visibility("default"))) void HishLinksEnableForApplet(void)
{
    enabled = links_wanted();
    tracing = getenv("HISH_LINKS_TRACE") != NULL;
}
#endif
#else
#ifdef HISH_LINKS_EXPORT
__attribute__((visibility("default"))) void HishLinksEnableForApplet(void) {}
#else
typedef int HishLinksOhosAarch64Only;
#endif
#endif
