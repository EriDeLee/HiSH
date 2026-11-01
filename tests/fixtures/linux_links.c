// Device check for the emulated symbolic links, built once for musl and once for glibc and
// run on the phone (see docs/symlinks.md). The functions that glibc keeps for itself
// (glob, nftw, ftw) are skipped in the glibc build.
// Usage: linux_links DIR   (DIR must not exist; it is created and left behind)
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
static char D[PATH_MAX];

static void check(int ok, const char* name)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) pass++; else fail++;
}
static char* P(const char* relative)
{
    static char ring[8][PATH_MAX];
    static int next;
    char* out = ring[next++ & 7];
    snprintf(out, PATH_MAX, "%s/%s", D, relative);
    return out;
}
static int write_file(const char* path, const char* text)
{
    FILE* file = fopen(path, "w");
    if (!file) return -1;
    fputs(text, file);
    return fclose(file);
}
static int first_char(const char* path)
{
    FILE* file = fopen(path, "r");
    if (!file) return -1;
    const int c = fgetc(file);
    fclose(file);
    return c;
}
static int no_dots(const struct dirent* entry)
{
    return strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
}
// "a|b|c" for the names scandir returned, then frees them.
static char* scanned(const char* path, int sorted, int* count)
{
    static char text[2048];
    struct dirent** list = NULL;
    text[0] = 0;
    *count = scandir(path, &list, no_dots, sorted ? alphasort : NULL);
    for (int i = 0; i < *count; i++) {
        if (i) strcat(text, "|");
        strcat(text, list[i]->d_name);
        free(list[i]);
    }
    free(list);
    return text;
}

#ifndef __GLIBC__
static char* joined(const glob_t* g)
{
    static char text[4096];
    text[0] = 0;
    for (size_t i = 0; i < g->gl_pathc; i++) {
        if (i) strcat(text, "|");
        strcat(text, g->gl_pathv[g->gl_offs + i]);
    }
    return text;
}
static int walk_count, walk_links, walk_dirs, walk_depth_dirs, walk_last_type;
static void walk_reset(void) { walk_count = walk_links = walk_dirs = walk_depth_dirs = walk_last_type = 0; }
static int on_walk(const char* path, const struct stat* st, int type, struct FTW* position)
{
    (void)path; (void)st; (void)position;
    walk_count++;
    if (type == FTW_SL) walk_links++;
    if (type == FTW_D) walk_dirs++;
    if (type == FTW_DP) walk_depth_dirs++;
    walk_last_type = type;
    return 0;
}
static int on_ftw(const char* path, const struct stat* st, int type) { return on_walk(path, st, type, NULL); }
#endif

// A/B mode: print what scandir, glob, nftw and ftw return on a plain tree (no links) in the current
// directory. Run it with the layer on and with HISH_LINKS=0, which hands these functions back to the
// C library, and compare the two outputs: they must be identical.
static void ab_scandir(const char* path, int sorted)
{
    struct dirent** list = NULL;
    errno = 0;
    const int count = scandir(path, &list, sorted ? no_dots : NULL, sorted ? alphasort : NULL);
    printf("scandir %s sorted=%d => %d errno=%d:", path, sorted, count, count < 0 ? errno : 0);
    for (int i = 0; i < count; i++) { printf(" [%s %d]", list[i]->d_name, list[i]->d_type); free(list[i]); }
    free(list);
    puts("");
}
#ifndef __GLIBC__
static int on_print(const char* path, const struct stat* st, int type, struct FTW* position)
{
    (void)st;
    printf("  type=%d %s level=%d base=%d\n", type, path, position->level, position->base);
    return 0;
}
static int on_print_ftw(const char* path, const struct stat* st, int type)
{
    (void)st;
    printf("  type=%d %s\n", type, path);
    return 0;
}
static void ab_glob(void)
{
    static const char* const patterns[] = {"*", ".*", "*.txt", "?.txt", "[ab].txt", "[!a].txt", "c\\[1\\].txt", "c[[]1].txt", "sub/*", "sub/*.c",
        "*/x.c", "*/*", "*/*/*", "nomatch*", "sub/", "sub//", "sub/*/", "**", "{a,b}.txt", "~", "a.txt/", "a.txt/*", "sub/.*", "./*", "./sub/*",
        "*/", ".", "..", "", "[", "\\", "a\\", "*/deep/n.txt", "s*/d*/*", "d?e.txt", "[a-c]*", "Cap.*", "other", "other/*", "other/", NULL};
    static const int flag_sets[] = {0, GLOB_MARK, GLOB_PERIOD, GLOB_NOESCAPE, GLOB_NOCHECK, GLOB_NOSORT, GLOB_MARK | GLOB_PERIOD, GLOB_ERR,
        GLOB_TILDE, GLOB_TILDE_CHECK, GLOB_NOCHECK | GLOB_MARK};
    for (int p = 0; patterns[p]; p++)
        for (size_t f = 0; f < sizeof(flag_sets) / sizeof(*flag_sets); f++) {
            glob_t g;
            memset(&g, 0, sizeof(g));
            const int rc = glob(patterns[p], flag_sets[f], NULL, &g);
            printf("glob flags=%d [%s] => rc=%d n=%zu:", flag_sets[f], patterns[p], rc, g.gl_pathc);
            for (size_t i = 0; i < g.gl_pathc; i++) printf(" [%s]", g.gl_pathv[i]);
            puts("");
            globfree(&g);
        }
    glob_t g;
    memset(&g, 0, sizeof(g));
    g.gl_offs = 3;
    int rc = glob("*.txt", GLOB_DOOFFS, NULL, &g);
    rc |= glob("sub/*", GLOB_DOOFFS | GLOB_APPEND, NULL, &g);
    printf("glob offsets => rc=%d n=%zu offs=%zu:", rc, g.gl_pathc, g.gl_offs);
    for (size_t i = 0; i < g.gl_offs + g.gl_pathc; i++) printf(" [%s]", g.gl_pathv[i] ? g.gl_pathv[i] : "(null)");
    puts("");
    globfree(&g);
}
static void ab_walk(void)
{
    static const char* const starts[] = {".", "sub", "./sub/", "a.txt", "nomatch", "sub/deep", "other", NULL};
    static const int flag_sets[] = {0, FTW_PHYS, FTW_DEPTH, FTW_PHYS | FTW_DEPTH, FTW_MOUNT, FTW_PHYS | FTW_MOUNT};
    static const int limits[] = {20, 1, 2};
    for (int s = 0; starts[s]; s++)
        for (size_t f = 0; f < sizeof(flag_sets) / sizeof(*flag_sets); f++)
            for (size_t l = 0; l < sizeof(limits) / sizeof(*limits); l++) {
                printf("nftw %s flags=%d limit=%d\n", starts[s], flag_sets[f], limits[l]);
                errno = 0;
                const int rc = nftw(starts[s], on_print, limits[l], flag_sets[f]);
                printf("  rc=%d errno=%d\n", rc, rc ? errno : 0);
            }
    for (int s = 0; starts[s]; s++) {
        printf("ftw %s\n", starts[s]);
        errno = 0;
        const int rc = ftw(starts[s], on_print_ftw, 10);
        printf("  rc=%d errno=%d\n", rc, rc ? errno : 0);
    }
}
#endif
static void ab_run(void)
{
    ab_scandir(".", 0);
    ab_scandir(".", 1);
    ab_scandir("sub", 1);
    ab_scandir("nomatch", 1);
    ab_scandir("a.txt", 1);
#ifndef __GLIBC__
    ab_glob();
    ab_walk();
#endif
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "--ab")) {
        if (chdir(argv[2])) { perror("chdir"); return 2; }
        ab_run();
        return 0;
    }
    if (argc != 2) { fprintf(stderr, "usage: %s DIR | --ab DIR\n", argv[0]); return 2; }
    if (mkdir(argv[1], 0700)) { perror("mkdir"); return 2; }
    if (!realpath(argv[1], D)) { perror("realpath"); return 2; }
    struct stat st;
    char text[PATH_MAX];

    mkdir(P("real"), 0700);
    mkdir(P("real/sub"), 0700);
    write_file(P("real/a.txt"), "A");
    write_file(P("real/b.txt"), "B");
    write_file(P("real/sub/deep.txt"), "D");
    check(symlink("real", P("dl")) == 0, "symlink to a directory");
    check(symlink("real/a.txt", P("fl")) == 0, "symlink to a file");
    check(symlink("nowhere", P("dangling")) == 0, "dangling symlink");
    check(symlink("fl", P("chain")) == 0, "link to a link");
    errno = 0;
    check(symlink("x", P("fl")) == -1 && errno == EEXIST, "symlink onto an existing name is EEXIST");

    check(lstat(P("dl"), &st) == 0 && S_ISLNK(st.st_mode), "lstat reports a directory link as a link");
    check(stat(P("dl"), &st) == 0 && S_ISDIR(st.st_mode), "stat follows a directory link");
    check(lstat(P("fl"), &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == (off_t)strlen("real/a.txt"), "lstat size of a link is the target length");
    check(stat(P("fl"), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 1, "stat follows a file link");
    check(stat(P("chain"), &st) == 0 && S_ISREG(st.st_mode), "stat follows a chain");
    errno = 0;
    check(stat(P("dangling"), &st) == -1 && errno == ENOENT && lstat(P("dangling"), &st) == 0, "dangling: stat is ENOENT, lstat works");
    check(readlink(P("dl"), text, sizeof(text)) == 4 && !strncmp(text, "real", 4), "readlink returns the target");
    check(access(P("fl"), R_OK) == 0 && access(P("dangling"), F_OK) == -1, "access follows the link");

    check(first_char(P("fl")) == 'A', "fopen through a file link");
    check(first_char(P("dl/b.txt")) == 'B', "fopen through a directory link");
    check(open(P("dl/sub/deep.txt"), O_RDONLY) >= 0, "open through a directory link");

    DIR* dir = opendir(P("dl"));
    int entries = 0;
    for (struct dirent* e = dir ? readdir(dir) : NULL; e; e = readdir(dir)) entries += no_dots(e);
    if (dir) closedir(dir);
    check(dir && entries == 3, "opendir and readdir through a directory link");

    int n;
    check(!strcmp(scanned(P("dl"), 1, &n), "a.txt|b.txt|sub") && n == 3, "scandir through a directory link");
    check(!strcmp(scanned(D, 1, &n), "chain|dangling|dl|fl|real") && n == 5, "scandir lists the links themselves");
    struct dirent** none = NULL;
    errno = 0;
    check(scandir(P("nowhere"), &none, no_dots, NULL) == -1 && errno == ENOENT, "scandir of a missing directory is ENOENT");

    FILE* reopened = fopen(P("real/b.txt"), "r");
    if (reopened) reopened = freopen(P("fl"), "r", reopened);
    check(reopened && fgetc(reopened) == 'A', "freopen follows a link");
    if (reopened) fclose(reopened);

    char* resolved = realpath(P("dl/sub"), NULL);
    check(resolved && !strcmp(resolved, P("real/sub")), "realpath replaces a directory link");
    free(resolved);
    resolved = realpath(P("chain"), NULL);
    check(resolved && !strcmp(resolved, P("real/a.txt")), "realpath replaces a chain");
    free(resolved);

    char* here = getcwd(NULL, 0);
    check(chdir(P("dl")) == 0, "chdir into a directory link");
    char* there = getcwd(NULL, 0);
    check(there && !strcmp(there, P("real")), "the working directory is the target");
    check(first_char("b.txt") == 'B', "relative open after chdir");
    if (here) { if (chdir(here)) perror("chdir back"); }
    free(here);
    free(there);

    check(mkdir(P("dl/newdir"), 0700) == 0 && stat(P("real/newdir"), &st) == 0 && S_ISDIR(st.st_mode), "mkdir through a directory link");
    check(rename(P("dl/newdir"), P("dl/renamed")) == 0 && stat(P("real/renamed"), &st) == 0, "rename inside a directory link");
    check(rmdir(P("dl/renamed")) == 0 && stat(P("real/renamed"), &st) == -1, "rmdir inside a directory link");
    check(rename(P("fl"), P("fl2")) == 0 && readlink(P("fl2"), text, sizeof(text)) == 10, "rename a link keeps its target");
    check(rename(P("fl2"), P("fl")) == 0, "rename it back");

#ifndef __GLIBC__
    glob_t g;
    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/dl/*.txt", D);
    char expected[4096];
    snprintf(expected, sizeof(expected), "%s/dl/a.txt|%s/dl/b.txt", D, D);
    check(glob(text, 0, NULL, &g) == 0 && !strcmp(joined(&g), expected), "glob inside a directory link");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/*/a.txt", D);
    snprintf(expected, sizeof(expected), "%s/dl/a.txt|%s/real/a.txt", D, D);
    check(glob(text, 0, NULL, &g) == 0 && !strcmp(joined(&g), expected), "glob descends into a directory link");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/d*", D);
    snprintf(expected, sizeof(expected), "%s/dangling|%s/dl", D, D);
    check(glob(text, 0, NULL, &g) == 0 && !strcmp(joined(&g), expected), "glob lists a dangling link and a directory link");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/dl/*", D);
    snprintf(expected, sizeof(expected), "%s/dl/a.txt|%s/dl/b.txt|%s/dl/sub/", D, D, D);
    check(glob(text, GLOB_MARK, NULL, &g) == 0 && !strcmp(joined(&g), expected), "GLOB_MARK adds a slash to a directory");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/dl", D);
    snprintf(expected, sizeof(expected), "%s/dl/", D);
    check(glob(text, GLOB_MARK, NULL, &g) == 0 && !strcmp(joined(&g), expected), "GLOB_MARK on a directory link itself");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    snprintf(text, sizeof(text), "%s/nomatch*", D);
    check(glob(text, 0, NULL, &g) == GLOB_NOMATCH, "no match is GLOB_NOMATCH");
    check(glob(text, GLOB_NOCHECK, NULL, &g) == 0 && g.gl_pathc == 1 && !strcmp(g.gl_pathv[0], text), "GLOB_NOCHECK returns the pattern");
    globfree(&g);

    memset(&g, 0, sizeof(g));
    g.gl_offs = 2;
    snprintf(text, sizeof(text), "%s/real/a*", D);
    check(glob(text, GLOB_DOOFFS, NULL, &g) == 0 && g.gl_pathc == 1 && !g.gl_pathv[0] && !g.gl_pathv[1], "GLOB_DOOFFS leaves empty slots");
    snprintf(text, sizeof(text), "%s/real/b*", D);
    check(glob(text, GLOB_DOOFFS | GLOB_APPEND, NULL, &g) == 0 && g.gl_pathc == 2 && strstr(g.gl_pathv[3], "b.txt"), "GLOB_APPEND adds to the list");
    globfree(&g);

    walk_reset();
    check(nftw(P("dl"), on_walk, 10, FTW_PHYS) == 0 && walk_count == 1 && walk_links == 1, "nftw with FTW_PHYS reports a directory link as a link");
    walk_reset();
    check(nftw(P("dl"), on_walk, 10, 0) == 0 && walk_count == 5 && walk_dirs == 2, "nftw without FTW_PHYS walks into a directory link");
    walk_reset();
    check(nftw(D, on_walk, 10, FTW_PHYS) == 0 && walk_links == 4, "nftw of a tree counts every link as a link");
    walk_reset();
    check(nftw(D, on_walk, 10, FTW_PHYS | FTW_DEPTH) == 0 && walk_last_type == FTW_DP && walk_depth_dirs == 3, "FTW_DEPTH visits the directory last");
    walk_reset();
    check(ftw(P("real"), on_ftw, 10) == 0 && walk_count == 5, "ftw walks a plain directory");
    errno = 0;
    check(nftw(P("nowhere"), on_walk, 10, 0) == -1 && errno == ENOENT, "nftw of a missing path is ENOENT");
#else
    printf("SKIP glob, nftw and ftw: glibc keeps its own\n");
#endif

    check(unlink(P("dl")) == 0 && stat(P("real/a.txt"), &st) == 0, "unlink removes only the link");

    // Information, not counted.
    dir = opendir(D);
    for (struct dirent* e = dir ? readdir(dir) : NULL; e; e = readdir(dir))
        if (!strcmp(e->d_name, "fl")) printf("INFO readdir d_type of a link: %d (DT_LNK is %d, DT_REG is %d)\n", e->d_type, DT_LNK, DT_REG);
    if (dir) closedir(dir);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 20000; i++) stat(P("real/a.txt"), &st);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("INFO 20000 stat calls: %ld ms\n", (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    printf("LINKS_C_TOTAL pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
