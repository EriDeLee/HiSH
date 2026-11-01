#include "native_elf.h"
#include "toybox_exec_path.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Toybox starts every other program in xexec(). The build renames the embedded
// copy's execvp to this function, and libhish_exec_bridge.so exports it as execvp
// for the system copy. The OHOS execvp cannot start a Linux ELF, so look the name
// up the way execvp does and enter the loader entry that mksh uses. Called only
// in a command process; success never returns.
int HiSHToyboxExecvp(const char *file, char *const argv[])
{
    char path[PATH_MAX];
    if (!file || !*file) { errno = ENOENT; return -1; }
    if (!strchr(file, '/')) {
        if (HishResolveCommand(file, getenv("PATH"), path, sizeof(path))) return -1;
        file = path;
    }
    return HiSHExecFile(file, argv);
}
