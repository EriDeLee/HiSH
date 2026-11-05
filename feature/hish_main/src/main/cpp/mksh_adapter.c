#define _GNU_SOURCE
#include "native_elf.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern char **environ;
extern int HiSHMkshMain(int argc, const char *argv[]);

// Called only in a command process. Keep mksh's exported environment when
// entering a real ELF or application applet, including `exec` and assignments.
int HiSHMkshExecve(const char *path, char *const argv[], char *const envp[])
{
    char **old = environ;
    environ = (char **)envp;
    int result = HiSHExecFile(path, argv);
    int saved = errno;
    const char *stage = HiSHExecError();
    if (result < 0 && stage && *stage) dprintf(2, "HiSH: cannot start %s: %s\n", path, stage);
    environ = old;
    errno = saved;
    return result;
}

int HiSHDefaultShellMain(int argc, char **argv)
{
    const char *home = getenv("HOME"), *prefix = getenv("PREFIX");
    char value[4096];
    if (home && !getenv("HISTFILE") && snprintf(value, sizeof(value), "%s/.mksh_history", home) < sizeof(value))
        setenv("HISTFILE", value, 1);
    if (prefix && snprintf(value, sizeof(value), "%s/bin/mksh", prefix) < sizeof(value))
        setenv("SHELL", value, 1);
    if (!getenv("LANG")) setenv("LANG", "C.UTF-8", 1);
    return HiSHMkshMain(argc, (const char **)argv);
}
