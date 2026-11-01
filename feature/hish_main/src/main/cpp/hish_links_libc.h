#ifndef HISH_LINKS_LIBC_H
#define HISH_LINKS_LIBC_H
// C library functions that reach files through the library's own internal calls, which a
// preload cannot see: scandir, glob, nftw and ftw walk directories, and freopen opens a
// file. They are written here against the same replaced functions as the rest of links.c,
// so a path that goes through an emulated link works for them too. Included by linux/links.c
// after the functions it uses.
//
// scandir, glob, nftw and ftw are adapted from musl libc: src/dirent/scandir.c,
// src/regex/glob.c, src/misc/nftw.c and src/legacy/ftw.c of the musl version HiSH builds.
// Changes: the file system calls go through the helpers below, helper names are prefixed,
// glob also descends into a regular file that is an emulated directory link, and
// HISH_LINKS=0 hands every function here back to the C library. glibc keeps its own glob,
// nftw and ftw (their flags differ from musl's), so only scandir and freopen are replaced
// there.
//
// The musl license, which covers the adapted code:
//
//   Copyright (c) 2005-2020 Rich Felker, et al.
//
//   Permission is hereby granted, free of charge, to any person obtaining
//   a copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions:
//
//   The above copyright notice and this permission notice shall be
//   included in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include <fnmatch.h>
#include <ftw.h>
#include <glob.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>

#undef scandir64
#undef freopen64

// The next definition of a function, for HISH_LINKS=0 and for functions that only need
// their path followed first.
#define LINKS_NEXT(type, name) ({ static type fn_; if (!fn_) fn_ = (type)dlsym(RTLD_NEXT, name); fn_; })
typedef int (*links_scandir_fn)(const char*, struct dirent***, int (*)(const struct dirent*), int (*)(const struct dirent**, const struct dirent**));
typedef FILE* (*links_freopen_fn)(const char*, const char*, FILE*);
typedef int (*links_glob_fn)(const char*, int, int (*)(const char*, int), glob_t*);
typedef void (*links_globfree_fn)(glob_t*);
typedef int (*links_nftw_fn)(const char*, int (*)(const char*, const struct stat*, int, struct FTW*), int, int);

int scandir(const char* path, struct dirent*** res, int (*sel)(const struct dirent*), int (*cmp)(const struct dirent**, const struct dirent**))
{
    if (!enabled) {
        links_scandir_fn next = LINKS_NEXT(links_scandir_fn, "scandir");
        if (next) return next(path, res, sel, cmp);
    }
    DIR* d = links_opendir(path);
    struct dirent *de, **names = 0, **tmp;
    size_t cnt = 0, len = 0;
    int old_errno = errno;

    if (!d) return -1;

    while ((errno = 0), (de = readdir(d))) {
        if (sel && !sel(de)) continue;
        if (cnt >= len) {
            len = 2 * len + 1;
            if (len > SIZE_MAX / sizeof *names) break;
            tmp = realloc(names, len * sizeof *names);
            if (!tmp) break;
            names = tmp;
        }
        names[cnt] = malloc(de->d_reclen);
        if (!names[cnt]) break;
        memcpy(names[cnt++], de, de->d_reclen);
    }

    closedir(d);

    if (errno) {
        if (names) while (cnt-- > 0) free(names[cnt]);
        free(names);
        return -1;
    }
    errno = old_errno;

    if (cmp) qsort(names, cnt, sizeof *names, (int (*)(const void*, const void*))cmp);
    *res = names;
    return (int)cnt;
}
#ifdef __GLIBC__
// glibc declares the 64-bit entry points with struct dirent64, which is struct dirent on 64-bit Linux.
int scandir64(const char* path, struct dirent64*** res, int (*sel)(const struct dirent64*), int (*cmp)(const struct dirent64**, const struct dirent64**))
{
    return scandir(path, (struct dirent***)res, (int (*)(const struct dirent*))sel, (int (*)(const struct dirent**, const struct dirent**))cmp);
}
#else
int scandir64(const char* path, struct dirent*** res, int (*sel)(const struct dirent*), int (*cmp)(const struct dirent**, const struct dirent**)) __attribute__((alias("scandir")));
#endif

// freopen opens its file inside the C library. Follow the links in the path first and
// let the library reopen the stream on the real file.
FILE* freopen(const char* path, const char* mode, FILE* stream)
{
    links_freopen_fn next = LINKS_NEXT(links_freopen_fn, "freopen");
    if (!next) { errno = ENOSYS; return NULL; }
    char buffer[PATH_MAX];
    int d;
    const char* p = path;
    if (path && prepare_follow(AT_FDCWD, path, buffer, &d, &p)) p = path;
    return next(p, mode, stream);
}
FILE* freopen64(const char* path, const char* mode, FILE* stream) __attribute__((alias("freopen")));

#ifndef __GLIBC__
#undef glob64
#undef globfree64
#undef ftw64
#undef nftw64

static int links_stat(const char* path, struct stat* st) { return (int)finish(stat_core(AT_FDCWD, path, st, 0)); }
static int links_lstat(const char* path, struct stat* st) { return (int)finish(stat_core(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW)); }

struct links_match {
    struct links_match* next;
    char name[];
};

static int links_glob_append(struct links_match** tail, const char* name, size_t len, int mark)
{
    struct links_match* added = malloc(sizeof(struct links_match) + len + 2);
    if (!added) return -1;
    (*tail)->next = added;
    added->next = NULL;
    memcpy(added->name, name, len + 1);
    if (mark && len && name[len - 1] != '/') {
        added->name[len] = '/';
        added->name[len + 1] = 0;
    }
    *tail = added;
    return 0;
}

static int links_do_glob(char* buf, size_t pos, int type, char* pat, int flags, int (*errfunc)(const char* path, int err), struct links_match** tail)
{
    /* If GLOB_MARK is unused, we don't care about type. */
    if (!type && !(flags & GLOB_MARK)) type = DT_REG;

    /* Special-case the remaining pattern being all slashes, in
     * which case we can use caller-passed type if it's a dir. */
    if (*pat && type != DT_DIR) type = 0;
    while (pos + 1 < PATH_MAX && *pat == '/') buf[pos++] = *pat++;

    /* Consume maximal [escaped-]literal prefix of pattern, copying
     * and un-escaping it to the running buffer as we go. */
    ptrdiff_t i = 0, j = 0;
    int in_bracket = 0, overflow = 0;
    for (; pat[i] != '*' && pat[i] != '?' && (!in_bracket || pat[i] != ']'); i++) {
        if (!pat[i]) {
            if (overflow) return 0;
            pat += i;
            pos += j;
            i = j = 0;
            break;
        } else if (pat[i] == '[') {
            in_bracket = 1;
        } else if (pat[i] == '\\' && !(flags & GLOB_NOESCAPE)) {
            /* Backslashes inside a bracket are (at least by
             * our interpretation) non-special, so if next
             * char is ']' we have a complete expression. */
            if (in_bracket && pat[i + 1] == ']') break;
            /* Unpaired final backslash never matches. */
            if (!pat[i + 1]) return 0;
            i++;
        }
        if (pat[i] == '/') {
            if (overflow) return 0;
            in_bracket = 0;
            pat += i + 1;
            i = -1;
            pos += j + 1;
            j = -1;
        }
        /* Only store a character if it fits in the buffer, but if
         * a potential bracket expression is open, the overflow
         * must be remembered and handled later only if the bracket
         * is unterminated (and thereby a literal), so as not to
         * disallow long bracket expressions with short matches. */
        if (pos + (j + 1) < PATH_MAX) {
            buf[pos + j++] = pat[i];
        } else if (in_bracket) {
            overflow = 1;
        } else {
            return 0;
        }
        /* If we consume any new components, the caller-passed type
         * or dummy type from above is no longer valid. */
        type = 0;
    }
    buf[pos] = 0;
    if (!*pat) {
        /* If we consumed any components above, or if GLOB_MARK is
         * requested and we don't yet know if the match is a dir,
         * we must confirm the file exists and/or determine its type.
         *
         * If marking dirs, symlink type is inconclusive; we need the
         * type for the symlink target, and therefore must try stat
         * first unless type is known not to be a symlink. Otherwise,
         * or if that fails, use lstat for determining existence to
         * avoid false negatives in the case of broken symlinks. */
        struct stat st;
        if ((flags & GLOB_MARK) && (!type || type == DT_LNK) && !links_stat(buf, &st)) {
            if (S_ISDIR(st.st_mode)) type = DT_DIR;
            else type = DT_REG;
        }
        if (!type && links_lstat(buf, &st)) {
            if (errno != ENOENT && (errfunc(buf, errno) || (flags & GLOB_ERR)))
                return GLOB_ABORTED;
            return 0;
        }
        if (links_glob_append(tail, buf, pos, (flags & GLOB_MARK) && type == DT_DIR))
            return GLOB_NOSPACE;
        return 0;
    }
    char *p2 = strchr(pat, '/'), saved_sep = '/';
    /* Check if the '/' was escaped and, if so, remove the escape char
     * so that it will not be unpaired when passed to fnmatch. */
    if (p2 && !(flags & GLOB_NOESCAPE)) {
        char* p;
        for (p = p2; p > pat && p[-1] == '\\'; p--);
        if ((p2 - p) % 2) {
            p2--;
            saved_sep = '\\';
        }
    }
    DIR* dir = links_opendir(pos ? buf : ".");
    if (!dir) {
        if (errfunc(buf, errno) || (flags & GLOB_ERR))
            return GLOB_ABORTED;
        return 0;
    }
    int old_errno = errno;
    struct dirent* de;
    while (errno = 0, de = readdir(dir)) {
        /* Quickly skip non-directories when there's pattern left. An emulated
         * directory link is a small regular file, so a regular file is only
         * skipped after the look at it below. */
        if (p2 && de->d_type && de->d_type != DT_DIR && de->d_type != DT_LNK && de->d_type != DT_REG)
            continue;

        size_t l = strlen(de->d_name);
        if (l >= PATH_MAX - pos) continue;

        if (p2) *p2 = 0;

        int fnm_flags = ((flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0)
            | ((!(flags & GLOB_PERIOD)) ? FNM_PERIOD : 0);

        if (fnmatch(pat, de->d_name, fnm_flags))
            continue;

        /* With GLOB_PERIOD, don't allow matching . or .. unless
         * fnmatch would match them with FNM_PERIOD rules in effect. */
        if (p2 && (flags & GLOB_PERIOD) && de->d_name[0] == '.'
            && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))
            && fnmatch(pat, de->d_name, fnm_flags | FNM_PERIOD))
            continue;

        memcpy(buf + pos, de->d_name, l + 1);
        if (p2) *p2 = saved_sep;
        int entry_type = de->d_type;
        if (p2 && entry_type == DT_REG) {
            struct stat st;
            if (links_lstat(buf, &st) || !S_ISLNK(st.st_mode)) continue;
            entry_type = DT_LNK;
        }
        int r = links_do_glob(buf, pos + l, entry_type, p2 ? p2 : "", flags, errfunc, tail);
        if (r) {
            closedir(dir);
            return r;
        }
    }
    int readerr = errno;
    if (p2) *p2 = saved_sep;
    closedir(dir);
    if (readerr && (errfunc(buf, errno) || (flags & GLOB_ERR)))
        return GLOB_ABORTED;
    errno = old_errno;
    return 0;
}

static int links_glob_ignore_error(const char* path, int err)
{
    (void)path;
    (void)err;
    return 0;
}

static void links_glob_free_list(struct links_match* head)
{
    struct links_match *match, *next;
    for (match = head->next; match; match = next) {
        next = match->next;
        free(match);
    }
}

static int links_glob_compare(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static int links_glob_tilde(char** pat, char* buf, size_t* pos)
{
    char* p = *pat + 1;
    size_t i = 0;

    char delim, *name_end = strchrnul(p, '/');
    if ((delim = *name_end)) *name_end++ = 0;
    *pat = name_end;

    char* home = *p ? NULL : getenv("HOME");
    if (!home) {
        struct passwd pw, *res;
        switch (*p ? getpwnam_r(p, &pw, buf, PATH_MAX, &res)
                   : getpwuid_r(getuid(), &pw, buf, PATH_MAX, &res)) {
        case ENOMEM:
            return GLOB_NOSPACE;
        case 0:
            if (!res)
        default:
                return GLOB_NOMATCH;
        }
        home = pw.pw_dir;
    }
    while (i < PATH_MAX - 2 && *home)
        buf[i++] = *home++;
    if (*home)
        return GLOB_NOMATCH;
    if ((buf[i] = delim))
        buf[++i] = 0;
    *pos = i;
    return 0;
}

int glob(const char* restrict pat, int flags, int (*errfunc)(const char* path, int err), glob_t* restrict g)
{
    if (!enabled) {
        links_glob_fn next = LINKS_NEXT(links_glob_fn, "glob");
        if (next) return next(pat, flags, errfunc, g);
    }
    struct links_match head = { .next = NULL }, *tail = &head;
    size_t cnt, i;
    size_t offs = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;
    int error = 0;
    char buf[PATH_MAX];

    if (!errfunc) errfunc = links_glob_ignore_error;

    if (!(flags & GLOB_APPEND)) {
        g->gl_offs = offs;
        g->gl_pathc = 0;
        g->gl_pathv = NULL;
    }

    if (*pat) {
        char* p = strdup(pat);
        if (!p) return GLOB_NOSPACE;
        buf[0] = 0;
        size_t pos = 0;
        char* s = p;
        if ((flags & (GLOB_TILDE | GLOB_TILDE_CHECK)) && *p == '~')
            error = links_glob_tilde(&s, buf, &pos);
        if (!error)
            error = links_do_glob(buf, pos, 0, s, flags, errfunc, &tail);
        free(p);
    }

    if (error == GLOB_NOSPACE) {
        links_glob_free_list(&head);
        return error;
    }

    for (cnt = 0, tail = head.next; tail; tail = tail->next, cnt++);
    if (!cnt) {
        if (flags & GLOB_NOCHECK) {
            tail = &head;
            if (links_glob_append(&tail, pat, strlen(pat), 0))
                return GLOB_NOSPACE;
            cnt++;
        } else if (!error)
            return GLOB_NOMATCH;
    }

    if (flags & GLOB_APPEND) {
        char** pathv = realloc(g->gl_pathv, (offs + g->gl_pathc + cnt + 1) * sizeof(char*));
        if (!pathv) {
            links_glob_free_list(&head);
            return GLOB_NOSPACE;
        }
        g->gl_pathv = pathv;
        offs += g->gl_pathc;
    } else {
        g->gl_pathv = malloc((offs + cnt + 1) * sizeof(char*));
        if (!g->gl_pathv) {
            links_glob_free_list(&head);
            return GLOB_NOSPACE;
        }
        for (i = 0; i < offs; i++)
            g->gl_pathv[i] = NULL;
    }
    for (i = 0, tail = head.next; i < cnt; tail = tail->next, i++)
        g->gl_pathv[offs + i] = tail->name;
    g->gl_pathv[offs + i] = NULL;
    g->gl_pathc += cnt;

    if (!(flags & GLOB_NOSORT))
        qsort(g->gl_pathv + offs, cnt, sizeof(char*), links_glob_compare);

    return error;
}

void globfree(glob_t* g)
{
    if (!enabled) {
        links_globfree_fn next = LINKS_NEXT(links_globfree_fn, "globfree");
        if (next) { next(g); return; }
    }
    size_t i;
    for (i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[g->gl_offs + i] - offsetof(struct links_match, name));
    free(g->gl_pathv);
    g->gl_pathc = 0;
    g->gl_pathv = NULL;
}
int glob64(const char* restrict pat, int flags, int (*errfunc)(const char* path, int err), glob_t* restrict g) __attribute__((alias("glob")));
void globfree64(glob_t* g) __attribute__((alias("globfree")));

struct links_history {
    struct links_history* chain;
    dev_t dev;
    ino_t ino;
    int level;
    int base;
};

static int links_do_nftw(char* path, int (*fn)(const char*, const struct stat*, int, struct FTW*), int fd_limit, int flags, struct links_history* h)
{
    size_t l = strlen(path), j = l && path[l - 1] == '/' ? l - 1 : l;
    struct stat st;
    struct links_history entry;
    int type;
    int r;
    int dfd = -1;
    int err = 0;
    struct FTW lev;

    st.st_dev = st.st_ino = 0;

    if ((flags & FTW_PHYS) ? links_lstat(path, &st) : (links_stat(path, &st) < 0)) {
        if (!(flags & FTW_PHYS) && errno == ENOENT && !links_lstat(path, &st))
            type = FTW_SLN;
        else if (errno != EACCES) return -1;
        else type = FTW_NS;
    } else if (S_ISDIR(st.st_mode)) {
        if (flags & FTW_DEPTH) type = FTW_DP;
        else type = FTW_D;
    } else if (S_ISLNK(st.st_mode)) {
        if (flags & FTW_PHYS) type = FTW_SL;
        else type = FTW_SLN;
    } else {
        type = FTW_F;
    }

    if ((flags & FTW_MOUNT) && h && type != FTW_NS && st.st_dev != h->dev)
        return 0;

    entry.chain = h;
    entry.dev = st.st_dev;
    entry.ino = st.st_ino;
    entry.level = h ? h->level + 1 : 0;
    entry.base = (int)j + 1;

    lev.level = entry.level;
    if (h) {
        lev.base = h->base;
    } else {
        size_t k;
        for (k = j; k && path[k] == '/'; k--);
        for (; k && path[k - 1] != '/'; k--);
        lev.base = (int)k;
    }

    if (type == FTW_D || type == FTW_DP) {
        dfd = (int)finish(openat_core(AT_FDCWD, path, O_RDONLY, 0));
        err = errno;
        if (dfd < 0 && err == EACCES) type = FTW_DNR;
        if (!fd_limit) close(dfd);
    }

    if (!(flags & FTW_DEPTH) && (r = fn(path, &st, type, &lev)))
        return r;

    for (; h; h = h->chain)
        if (h->dev == st.st_dev && h->ino == st.st_ino)
            return 0;

    if ((type == FTW_D || type == FTW_DP) && fd_limit) {
        if (dfd < 0) {
            errno = err;
            return -1;
        }
        DIR* d = fdopendir(dfd);
        if (d) {
            struct dirent* de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.'
                    && (!de->d_name[1]
                        || (de->d_name[1] == '.'
                            && !de->d_name[2]))) continue;
                if (strlen(de->d_name) >= PATH_MAX - l) {
                    errno = ENAMETOOLONG;
                    closedir(d);
                    return -1;
                }
                path[j] = '/';
                strcpy(path + j + 1, de->d_name);
                if ((r = links_do_nftw(path, fn, fd_limit - 1, flags, &entry))) {
                    closedir(d);
                    return r;
                }
            }
            closedir(d);
        } else {
            close(dfd);
            return -1;
        }
    }

    path[l] = 0;
    if ((flags & FTW_DEPTH) && (r = fn(path, &st, type, &lev)))
        return r;

    return 0;
}

int nftw(const char* path, int (*fn)(const char*, const struct stat*, int, struct FTW*), int fd_limit, int flags)
{
    if (!enabled) {
        links_nftw_fn next = LINKS_NEXT(links_nftw_fn, "nftw");
        if (next) return next(path, fn, fd_limit, flags);
    }
    int r, cs;
    size_t l;
    char pathbuf[PATH_MAX + 1];

    if (fd_limit <= 0) return 0;

    l = strlen(path);
    if (l > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(pathbuf, path, l + 1);

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
    r = links_do_nftw(pathbuf, fn, fd_limit, flags, NULL);
    pthread_setcancelstate(cs, 0);
    return r;
}
int nftw64(const char* path, int (*fn)(const char*, const struct stat*, int, struct FTW*), int fd_limit, int flags) __attribute__((alias("nftw")));

int ftw(const char* path, int (*fn)(const char*, const struct stat*, int), int fd_limit)
{
    return nftw(path, (int (*)(const char*, const struct stat*, int, struct FTW*))fn, fd_limit, FTW_PHYS);
}
int ftw64(const char* path, int (*fn)(const char*, const struct stat*, int), int fd_limit) __attribute__((alias("ftw")));
#endif
#endif
