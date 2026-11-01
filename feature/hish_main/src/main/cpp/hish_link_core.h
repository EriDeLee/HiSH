#ifndef HISH_LINK_CORE_H
#define HISH_LINK_CORE_H
// Emulated symbolic links. This device refuses symlink(2) in every directory an
// app can write, so a link is stored as a small regular file with the sticky bit
// set (that bit survives cp, mv, rewrite and chmod on this storage). Its bytes are
// HISH_LINK_MAGIC followed by the target path. Programs that go through HiSH's
// libc layer see an ordinary symlink; other programs see a small plain file.
//
// Portable C, no system calls: the file system is reached through HishLinkOps, so
// the same code is unit-tested on the host and used by the device libraries.
#include <errno.h>
#include <stddef.h>
#include <string.h>

#define HISH_LINK_MAGIC "!<hish-symlink>"
#define HISH_LINK_MAGIC_LENGTH 15
#define HISH_LINK_MAX_TARGET 4095
#define HISH_LINK_MAX_FOLLOW 40
#define HISH_LINK_MODE 01600
#define HISH_LINK_WORK 8192

// Whole file content -> target. Returns 1 and points into `bytes` for a valid marker.
static inline int HishLinkParse(const char* bytes, size_t count, const char** target, size_t* length)
{
    if (count <= HISH_LINK_MAGIC_LENGTH || count - HISH_LINK_MAGIC_LENGTH > HISH_LINK_MAX_TARGET) return 0;
    if (memcmp(bytes, HISH_LINK_MAGIC, HISH_LINK_MAGIC_LENGTH)) return 0;
    if (memchr(bytes + HISH_LINK_MAGIC_LENGTH, 0, count - HISH_LINK_MAGIC_LENGTH)) return 0;
    *target = bytes + HISH_LINK_MAGIC_LENGTH;
    *length = count - HISH_LINK_MAGIC_LENGTH;
    return 1;
}
// Marker bytes for `target`; `out` needs HISH_LINK_MAGIC_LENGTH + length bytes. Returns the size, 0 if unusable.
static inline size_t HishLinkFormat(const char* target, size_t length, char* out)
{
    if (!length || length > HISH_LINK_MAX_TARGET || memchr(target, 0, length)) return 0;
    memcpy(out, HISH_LINK_MAGIC, HISH_LINK_MAGIC_LENGTH);
    memcpy(out + HISH_LINK_MAGIC_LENGTH, target, length);
    return HISH_LINK_MAGIC_LENGTH + length;
}

typedef struct HishLinkOps {
    // Looks at the entry at an absolute path without following a final link. Returns 1 and
    // copies the target when it is an emulated link (never NUL-terminated beyond `*length`),
    // 0 for anything else, including a missing entry.
    int (*inspect)(void* context, const char* path, char* target, size_t capacity, size_t* length);
} HishLinkOps;

// Replaces every emulated link in `path` by its target. `base` is the absolute directory
// that relative paths start from. `out` receives an absolute path; it still ends in a
// link when `follow_last` is 0. Real links the file system itself knows are left for the
// kernel. Returns 0 or a positive errno: ELOOP after HISH_LINK_MAX_FOLLOW links,
// ENAMETOOLONG, ENOENT for an empty path, EINVAL for a relative path without a base.
static inline int HishLinkResolve(const HishLinkOps* ops, void* context, const char* base, const char* path,
    int follow_last, char* out, size_t capacity)
{
    char work[HISH_LINK_WORK], target[HISH_LINK_MAX_TARGET + 1];
    size_t length, used = 0, at = 0, follows = 0;
    const size_t path_length = strlen(path);
    if (!path_length) return ENOENT;
    if (path_length >= sizeof(work) || capacity < 2) return ENAMETOOLONG;
    memcpy(work, path, path_length + 1);
    if (path[0] == '/') {
        out[0] = '/'; out[1] = 0; used = 1;
    } else {
        if (!base || base[0] != '/') return EINVAL;
        used = strlen(base);
        if (used >= capacity) return ENAMETOOLONG;
        memcpy(out, base, used + 1);
        while (used > 1 && out[used - 1] == '/') out[--used] = 0;
    }
    for (;;) {
        while (work[at] == '/') at++;
        if (!work[at]) break;
        const size_t start = at;
        while (work[at] && work[at] != '/') at++;
        const size_t part = at - start;
        size_t after = at;
        while (work[after] == '/') after++;
        const int last = !work[after], slash = work[at] == '/';
        if (part == 1 && work[start] == '.') continue;
        if (part == 2 && work[start] == '.' && work[start + 1] == '.') {
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
            out[used] = 0;
            continue;
        }
        const size_t before = used;
        if (used + part + 2 > capacity) return ENAMETOOLONG;
        if (used > 1) out[used++] = '/';
        memcpy(out + used, work + start, part);
        used += part;
        out[used] = 0;
        if ((!last || follow_last || slash) && ops->inspect(context, out, target, sizeof(target), &length) && length) {
            if (++follows > HISH_LINK_MAX_FOLLOW) return ELOOP;
            const size_t rest = strlen(work + at);
            if (length + rest + 1 > sizeof(work)) return ENAMETOOLONG;
            memmove(work + length, work + at, rest + 1);
            memcpy(work, target, length);
            at = 0;
            used = before;
            if (target[0] == '/') used = 1;
            out[used] = 0;
        }
    }
    if (path[path_length - 1] == '/' && used > 1 && out[used - 1] != '/') {
        if (used + 2 > capacity) return ENAMETOOLONG;
        out[used++] = '/';
        out[used] = 0;
    }
    return 0;
}
#endif
