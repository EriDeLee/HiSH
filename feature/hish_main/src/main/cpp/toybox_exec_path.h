#ifndef HISH_TOYBOX_EXEC_PATH_H
#define HISH_TOYBOX_EXEC_PATH_H
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
// The host that runs the unit test has no execute bit, so the test replaces this check.
#ifndef HISH_EXECUTABLE_FILE
#define HISH_EXECUTABLE_FILE(path) access(path, X_OK)
#endif

// Embedded Toybox applets that start another program (xargs) run in the OHOS
// bootstrap, whose own execvp cannot start a Linux program. They look a bare
// command name up here and hand the resolved path to HiSHExecFile, which is
// also what mksh uses.
//
// Search order is execvp's: each PATH entry in turn, an empty entry meaning the
// current directory, the first executable regular file wins. Returns 0 and
// fills `out`. Otherwise -1 with errno like execvp: EACCES when a candidate
// exists but is not executable and nothing better turned up, ENAMETOOLONG for
// an over-long name or a result that does not fit `out`, ENOENT otherwise.
static inline int HishResolveCommand(const char* name, const char* search, char* out, size_t capacity)
{
    if (!name || !*name || !out || !capacity) { errno = ENOENT; return -1; }
    if (strchr(name, '/')) { errno = EINVAL; return -1; }
    if (strlen(name) > NAME_MAX) { errno = ENAMETOOLONG; return -1; }
    int denied = 0;
    const char* at = search && *search ? search : "/bin:/usr/bin";
    for (;;) {
        const char* end = strchr(at, ':');
        const size_t directory = end ? (size_t)(end - at) : strlen(at);
        char candidate[PATH_MAX];
        const int written = directory
            ? snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)directory, at, name)
            : snprintf(candidate, sizeof(candidate), "%s", name);
        if (written > 0 && (size_t)written < sizeof(candidate)) {
            struct stat info;
            if (!stat(candidate, &info) && S_ISREG(info.st_mode)) {
                if (!HISH_EXECUTABLE_FILE(candidate)) {
                    if ((size_t)written >= capacity) { errno = ENAMETOOLONG; return -1; }
                    memcpy(out, candidate, (size_t)written + 1);
                    return 0;
                }
                if (errno == EACCES) denied = 1;
            }
        }
        if (!end) break;
        at = end + 1;
    }
    errno = denied ? EACCES : ENOENT;
    return -1;
}
#endif
