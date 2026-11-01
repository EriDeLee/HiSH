// Does this kernel honour EPOLLONESHOT and EPOLLET? Bun registers the pipes and sockets behind process.stdout with those flags.
// If a descriptor that is always writable keeps being reported after its one-shot or edge event, Bun's loop never sleeps.
// Every line shows how many events each successive epoll_wait(timeout 0) returned and what Linux does ("expect").
// Usage: epoll_probe LOGFILE   (fd 1 is probed as given: run it with stdout on a terminal, a pipe and a file)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static FILE* log_file;
static int ready(int ep)
{
    struct epoll_event out[4];
    return epoll_wait(ep, out, 4, 0);
}
static void waits(int ep, int count)
{
    for (int i = 0; i < count; i++) fprintf(log_file, " %d", ready(ep));
}
// Add fd with `events`, then wait `count` times without touching the descriptor.
static void repeat(const char* label, int fd, unsigned events, int count, const char* expect)
{
    const int ep = epoll_create1(0);
    struct epoll_event ev = {.events = events, .data = {.fd = fd}};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev)) {
        fprintf(log_file, "%-40s ctl failed errno=%d (%s)\n", label, errno, strerror(errno));
        close(ep);
        return;
    }
    fprintf(log_file, "%-40s waits:", label);
    waits(ep, count);
    fprintf(log_file, "   expect:%s\n", expect);
    close(ep);
}
// A one-shot or edge registration, then re-armed with EPOLL_CTL_MOD.
static void rearm(const char* label, int fd, unsigned events, const char* expect)
{
    const int ep = epoll_create1(0);
    struct epoll_event ev = {.events = events, .data = {.fd = fd}};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev)) {
        fprintf(log_file, "%-40s ctl failed errno=%d (%s)\n", label, errno, strerror(errno));
        close(ep);
        return;
    }
    fprintf(log_file, "%-40s waits:", label);
    waits(ep, 2);
    fprintf(log_file, " | MOD ->");
    epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
    waits(ep, 2);
    fprintf(log_file, "   expect:%s\n", expect);
    close(ep);
}
// Data arrives on a readable descriptor between waits.
static void data(const char* label, int reader, int writer, unsigned events, const char* expect)
{
    const int ep = epoll_create1(0);
    struct epoll_event ev = {.events = events, .data = {.fd = reader}};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, reader, &ev)) {
        fprintf(log_file, "%-40s ctl failed errno=%d (%s)\n", label, errno, strerror(errno));
        close(ep);
        return;
    }
    fprintf(log_file, "%-40s waits:", label);
    waits(ep, 2);
    fprintf(log_file, " | write ->");
    if (write(writer, "x", 1) != 1) fprintf(log_file, " (write failed)");
    waits(ep, 3);
    fprintf(log_file, " | write ->");
    if (write(writer, "y", 1) != 1) fprintf(log_file, " (write failed)");
    waits(ep, 3);
    fprintf(log_file, "   expect:%s\n", expect);
    close(ep);
}

// What the kernel lists for an epoll descriptor in /proc/self/fdinfo, and whether epoll_pwait2 (syscall 441) exists.
static void kernel_facts(void)
{
    int p[2];
    if (pipe(p)) return;
    const int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLONESHOT, .data = {.fd = p[1]}};
    epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
    char path[64], text[1024];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", ep);
    const int fd = open(path, O_RDONLY);
    const ssize_t n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
    if (n >= 0) {
        text[n] = 0;
        fprintf(log_file, "fdinfo of an epoll set holding pipe fd %d with EPOLLOUT|EPOLLONESHOT (0x%x):\n%s", p[1], EPOLLOUT | EPOLLONESHOT, text);
    } else {
        fprintf(log_file, "fdinfo of the epoll set: open=%d errno=%d\n", fd, errno);
    }
    if (fd >= 0) close(fd);
    // A child makes the call: the app's seccomp filter may kill the caller of a syscall it does not allow.
    const pid_t child = fork();
    if (child == 0) {
        struct epoll_event out[4];
        struct timespec zero = {0, 0};
        sigset_t mask;
        sigemptyset(&mask);
        errno = 0;
        const long r = syscall(441, ep, out, 4, &zero, &mask, 8);
        _exit(r < 0 ? (errno == ENOSYS ? 10 : 11) : 12);
    }
    int status = 0;
    if (child > 0 && waitpid(child, &status, 0) == child) {
        if (WIFSIGNALED(status)) fprintf(log_file, "epoll_pwait2 (syscall 441): the caller was killed by signal %d (SIGSYS is %d)\n", WTERMSIG(status), SIGSYS);
        else fprintf(log_file, "epoll_pwait2 (syscall 441): child exit code %d (10 = ENOSYS, 11 = other error, 12 = works)\n", WEXITSTATUS(status));
    }
    close(ep);
    close(p[0]);
    close(p[1]);
}

int main(int argc, char** argv)
{
    if (argc != 2 || !(log_file = fopen(argv[1], "a"))) return 2;
    setvbuf(log_file, NULL, _IONBF, 0);
    const int flags = fcntl(1, F_GETFL);
    struct stat st;
    char target[256] = "?";
    const ssize_t tn = readlink("/proc/self/fd/1", target, sizeof(target) - 1);
    if (tn >= 0) target[tn] = 0;
    fprintf(log_file, "fd 1: flags=0x%x access=%d isatty=%d type=0%o (S_IFIFO is 010000, S_IFSOCK 0140000, S_IFCHR 020000, S_IFREG 0100000) link=%s\n",
        flags, flags & O_ACCMODE, isatty(1), fstat(1, &st) ? 0 : st.st_mode & S_IFMT, target);
    kernel_facts();
    repeat("fd1 EPOLLIN", 1, EPOLLIN, 3, " 0 0 0");
    repeat("fd1 EPOLLOUT", 1, EPOLLOUT, 3, " 1 1 1 (level triggered, pipe or socket)");
    repeat("fd1 EPOLLOUT|EPOLLET", 1, EPOLLOUT | EPOLLET, 3, " 1 0 0");
    repeat("fd1 EPOLLOUT|EPOLLONESHOT", 1, EPOLLOUT | EPOLLONESHOT, 3, " 1 0 0");
    repeat("fd1 EPOLLIN|EPOLLOUT|EPOLLET", 1, EPOLLIN | EPOLLOUT | EPOLLET, 3, " 1 0 0");
    int p[2];
    if (pipe(p) == 0) {
        repeat("pipe write end EPOLLOUT", p[1], EPOLLOUT, 3, " 1 1 1");
        repeat("pipe write end EPOLLOUT|EPOLLET", p[1], EPOLLOUT | EPOLLET, 3, " 1 0 0");
        repeat("pipe write end EPOLLOUT|EPOLLONESHOT", p[1], EPOLLOUT | EPOLLONESHOT, 3, " 1 0 0");
        rearm("pipe write end ONESHOT then MOD", p[1], EPOLLOUT | EPOLLONESHOT, " 1 0 | MOD -> 1 0");
        rearm("pipe write end ET then MOD", p[1], EPOLLOUT | EPOLLET, " 1 0 | MOD -> 1 0");
        data("pipe read end EPOLLIN|EPOLLET", p[0], p[1], EPOLLIN | EPOLLET, " 0 0 | write -> 1 0 0 | write -> 1 0 0");
        close(p[0]);
        close(p[1]);
    }
    if (pipe2(p, O_NONBLOCK) == 0) {
        data("pipe read end EPOLLIN (level)", p[0], p[1], EPOLLIN, " 0 0 | write -> 1 1 1 | write -> 1 1 1");
        close(p[0]);
        close(p[1]);
    }
    if (pipe2(p, O_NONBLOCK) == 0) {
        data("pipe read end EPOLLIN|EPOLLONESHOT", p[0], p[1], EPOLLIN | EPOLLONESHOT, " 0 0 | write -> 1 0 0 | write -> 0 0 0");
        close(p[0]);
        close(p[1]);
    }
    // What a user-space emulation of EPOLLONESHOT could rely on: take the registration out after its event was reported.
    if (pipe(p) == 0) {
        const int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT, .data = {.fd = p[1]}};
        epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
        fprintf(log_file, "%-40s waits:", "disarm by MOD events=0");
        waits(ep, 1);
        struct epoll_event none = {.events = 0, .data = {.fd = p[1]}};
        const int mod = epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &none);
        fprintf(log_file, " | MOD(0) rc=%d errno=%d ->", mod, mod ? errno : 0);
        waits(ep, 3);
        close(p[0]);
        fprintf(log_file, " | read end closed ->");
        waits(ep, 2);
        fprintf(log_file, "   expect: 1 | MOD(0) rc=0 -> 0 0 0 | read end closed -> 1 1 (ERR is always reported)\n");
        close(ep);
        close(p[1]);
    }
    if (pipe(p) == 0) {
        const int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT, .data = {.fd = p[1]}};
        epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
        fprintf(log_file, "%-40s waits:", "disarm by DEL, then ADD again");
        waits(ep, 1);
        const int del = epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL);
        fprintf(log_file, " | DEL rc=%d errno=%d ->", del, del ? errno : 0);
        waits(ep, 3);
        const int add = epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
        fprintf(log_file, " | ADD rc=%d errno=%d ->", add, add ? errno : 0);
        waits(ep, 2);
        const int del2 = epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL);
        const int del3 = epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL);
        fprintf(log_file, " | DEL rc=%d, DEL again rc=%d errno=%d (ENOENT is %d)", del2, del3, del3 ? errno : 0, ENOENT);
        fprintf(log_file, "   expect: 1 | DEL rc=0 -> 0 0 0 | ADD rc=0 -> 1 0 | DEL rc=0, DEL again rc=-1 errno=2\n");
        close(ep);
        close(p[0]);
        close(p[1]);
    }
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
        repeat("socketpair EPOLLOUT", s[0], EPOLLOUT, 3, " 1 1 1");
        repeat("socketpair EPOLLOUT|EPOLLET", s[0], EPOLLOUT | EPOLLET, 3, " 1 0 0");
        repeat("socketpair EPOLLOUT|EPOLLONESHOT", s[0], EPOLLOUT | EPOLLONESHOT, 3, " 1 0 0");
        close(s[0]);
        close(s[1]);
    }
    const int efd = eventfd(0, EFD_NONBLOCK);
    if (efd >= 0) {
        const int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data = {.fd = efd}};
        epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
        const uint64_t one = 1;
        fprintf(log_file, "%-40s waits:", "eventfd EPOLLIN|EPOLLONESHOT");
        waits(ep, 2);
        fprintf(log_file, " | write ->");
        if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one)) fprintf(log_file, " (write failed)");
        waits(ep, 3);
        fprintf(log_file, "   expect: 0 0 | write -> 1 0 0\n");
        close(ep);
        close(efd);
    }
    fprintf(log_file, "\n");
    fclose(log_file);
    return 0;
}
