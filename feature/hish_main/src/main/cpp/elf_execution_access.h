#ifndef HISH_ELF_EXECUTION_ACCESS_H
#define HISH_ELF_EXECUTION_ACCESS_H
#include "native_tools.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

// HAP installs its signed native tools as 0644. They already have deliberate
// command adapters, so preserve absolute-path/fd/self-exec for those exact
// installed inodes. A HOME copy has a different inode and still needs X_OK.
static inline int HishELFExecutionAccess(const char* path, const char* libraries)
{
    if (!access(path, X_OK)) return 0;
    const int denied = errno;
    if (denied != EACCES || !libraries || libraries[0] != '/') return -1;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { errno = denied; return -1; }
    struct stat target;
    const int valid = !fstat(fd, &target) && S_ISREG(target.st_mode);
    close(fd);
    if (valid) {
        static const char* files[] = {
#define TOOL(name, file, runtime) file,
            HISH_NATIVE_TOOLS(TOOL)
#undef TOOL
            NULL
        };
        for (size_t i = 0; files[i]; i++) {
            char installed[PATH_MAX]; struct stat known;
            const int length = snprintf(installed, sizeof(installed), "%s/%s", libraries, files[i]);
            if (length > 0 && (size_t)length < sizeof(installed) && !stat(installed, &known) &&
                target.st_dev == known.st_dev && target.st_ino == known.st_ino) return 0;
        }
    }
    errno = denied; return -1;
}
#endif
