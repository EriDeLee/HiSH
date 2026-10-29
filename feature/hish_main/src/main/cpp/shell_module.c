#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include "native_elf.h"
extern char **environ;
#include <unistd.h>
struct toy_list;
extern struct toy_list *toy_find(char *name);
extern int HiSHDefaultShellMain(int argc, char **argv);
extern int HiSHToyboxMain(int argc, char **argv);
int HiSHPipe(int fds[2]) { return pipe2(fds, O_CLOEXEC); }
static char system_applets[32768];
static int applets_discovered;
static int system_applet(const char *name)
{
    if (strchr(name, '/')) return 0;
    const char *p = system_applets;
    while (*p) {
        size_t len = strcspn(p, " \t\r\n");
        const char *base = p;
        for (size_t i = 0; i < len; ++i) if (p[i] == '/') base = p + i + 1;
        if (strlen(name) == (size_t)(p + len - base) && !memcmp(name, base, p + len - base)) return 1;
        p += len;
        while (*p && strchr(" \t\r\n", *p)) ++p;
    }
    return 0;
}
static void discover_system_applets(void)
{
    if (applets_discovered) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK)) return;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {"toybox", "--long", NULL};
    // A Linux caller can enter this fresh OHOS bootstrap with its original
    // environment restored. Never preload Linux ABI libraries into toybox.
    char *clean[4097]; size_t count = 0;
    for(size_t i=0;environ&&environ[i]&&i<4096;i++){
        if(!strncmp(environ[i],"LD_",3)||!strncmp(environ[i],"GLIBC_TUNABLES=",14)||
            !strncmp(environ[i],"HISH_EXEC_BRIDGE_FD=",20))continue;
        clean[count++]=environ[i];
    }
    clean[count]=NULL;
    pid_t pid = -1;
    int result = posix_spawn(&pid, "/system/bin/toybox", &actions, NULL, argv, clean);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (result) { close(fds[0]); return; }
    size_t used = 0; int finished = 0;
    for (int n = 0; n < 100 && used < sizeof(system_applets) - 1; ++n) {
        struct pollfd ready = {fds[0], POLLIN, 0};
        poll(&ready, 1, 20);
        ssize_t count = read(fds[0], system_applets + used, sizeof(system_applets) - used - 1);
        if (count > 0) used += count;
        else if (count == 0) { finished = 1; break; }
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    close(fds[0]);
    int status = 0;
    if (!finished) kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    system_applets[finished && WIFEXITED(status) && !WEXITSTATUS(status) ? used : 0] = 0;
    applets_discovered = 1;
}
// Embedded libraries must not run the UI process's inherited atexit handlers.
__attribute__((noreturn)) void HiSHChildExit(int code)
{
    fflush(NULL);
    _exit(code);
}
int HiSHHasApplet(const char *name)
{
    return HiSHHasNativeCommand(name) || !strcmp(name, "sh") || !strcmp(name, "mksh") ||
        (!strchr(name, '/') && toy_find((char *)name) != NULL) || system_applet(name);
}
// Only called inside dedicated shell/command processes, never in the UI.
void HiSHRunApplet(char **argv)
{
    int argc = 0;
    while (argv[argc]) ++argc;
    if (HiSHHasNativeCommand(argv[0])) {
        const int result = HiSHRunNativeCommand(argv);
        if (result < 0) fprintf(stderr, "%s: %s\n", argv[0], HiSHExecError());
        HiSHChildExit(result < 0 ? 126 : result);
    }
    if (!strcmp(argv[0], "sh") || !strcmp(argv[0], "mksh")) HiSHChildExit(HiSHDefaultShellMain(argc, argv));
    if (!toy_find(argv[0]) && system_applet(argv[0])) {
        char **system = calloc(argc + 2, sizeof(char *));
        if (!system) _exit(125);
        system[0] = "toybox";
        for (int i = 0; i < argc; ++i) system[i + 1] = argv[i];
        HiSHExecFile("/system/bin/toybox", system);
        fprintf(stderr, "%s: system toybox exec errno=%d (%s)\n", argv[0], errno, strerror(errno));
        _exit(126);
    }
    HiSHToyboxMain(argc, argv);
    _exit(127);
}
__attribute__((visibility("default"))) int HiSHShellMain(int argc, char **argv)
{
    discover_system_applets();
    return HiSHDefaultShellMain(argc, argv);
}
void HiSHInitializeApplets(void) { discover_system_applets(); }
const char* HiSHSystemApplets(void) { return system_applets; }
