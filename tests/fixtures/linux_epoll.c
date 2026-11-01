// Device check for the epoll fixes (docs/epoll.md), built once for musl and once for glibc and run on the phone.
// Usage: linux_epoll        (PASS or FAIL per check, a total line, exit status 1 on any failure)
// Run it as it is, then with HISH_EPOLL=0 as the control: the one-shot checks must fail, the others must not.
// The epoll_pwait2 checks run in a child process, because the app's seccomp filter kills a caller of the real system call.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif
// glibc declares it; musl 1.2.5 does not, and the preload library supplies it either way.
extern int epoll_pwait2(int, struct epoll_event*, int, const struct timespec*, const sigset_t*) __attribute__((weak));

static int pass, fail;
static void check(int ok, const char* name)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) pass++; else fail++;
}

static int digit(int n) { return n > 9 ? 9 : n < 0 ? 9 : n; }
// Three immediate waits as one number: 100 means one event, then none, then none.
static int waits3(int ep)
{
    struct epoll_event out[8];
    const int a = digit(epoll_wait(ep, out, 8, 0));
    const int b = digit(epoll_wait(ep, out, 8, 0));
    const int c = digit(epoll_wait(ep, out, 8, 0));
    return a * 100 + b * 10 + c;
}
static int add(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u64 = data;
    return epoll_ctl(ep, op, fd, &ev);
}

static long elapsed_ms(const struct timespec* from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - from->tv_sec) * 1000 + (now.tv_nsec - from->tv_nsec) / 1000000;
}

static volatile int thread_failures;
static void* worker(void* unused)
{
    (void)unused;
    int p[2];
    struct epoll_event out[4];
    if (pipe(p)) { thread_failures++; return NULL; }
    const int ep = epoll_create1(0);
    const uint64_t tag = (uint64_t)(uintptr_t)&p[0];
    if (add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, tag)) thread_failures++;
    for (int i = 0; i < 4000; i++) {
        const int first = epoll_wait(ep, out, 4, 1000);
        const int second = epoll_wait(ep, out, 4, 0);
        if (first != 1 || second != 0 || out[0].data.u64 == 0) thread_failures++;
        if (add(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT | EPOLLONESHOT, tag)) thread_failures++;
    }
    close(ep);
    close(p[0]);
    close(p[1]);
    return NULL;
}

int main(void)
{
    struct epoll_event out[8];
    int p[2], q[2], ep;

    // The pipe write end is always writable, which is what Bun registers for its stdout.
    pipe(p);
    ep = epoll_create1(0);
    check(add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 0x1122334455667788ull) == 0, "ADD with EPOLLONESHOT succeeds");
    int n = epoll_wait(ep, out, 8, 0);
    check(n == 1 && out[0].data.u64 == 0x1122334455667788ull && (out[0].events & EPOLLOUT), "the first wait reports it with its user data");
    check(waits3(ep) == 0, "later waits are silent");
    check(add(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT | EPOLLONESHOT, 0x1122334455667788ull) == 0 && waits3(ep) == 100, "MOD re-arms it: one report, then silence");
    check(add(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT | EPOLLONESHOT, 5) == 0 && epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 5, "MOD can change the user data");
    check(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL) == 0, "DEL of a disarmed registration succeeds");
    errno = 0;
    check(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL) == -1 && errno == ENOENT, "a second DEL is ENOENT");
    errno = 0;
    check(add(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT | EPOLLONESHOT, 5) == -1 && errno == ENOENT, "MOD after DEL is ENOENT");
    check(add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 6) == 0 && waits3(ep) == 100, "ADD after DEL registers again");
    close(ep);
    close(p[0]);
    close(p[1]);

    // The read end: data that arrives while the registration is disarmed waits for the re-arm.
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLONESHOT, 1);
    check(waits3(ep) == 0, "nothing to read, nothing reported");
    (void)!write(p[1], "x", 1);
    check(waits3(ep) == 100, "readable: reported once");
    (void)!write(p[1], "y", 1);
    check(waits3(ep) == 0, "more data while disarmed is not reported");
    check(add(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 1) == 0 && waits3(ep) == 100, "re-arming reports the data that is waiting");
    close(ep);
    close(p[0]);
    close(p[1]);

    // Ordinary registrations are untouched.
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 1);
    (void)!write(p[1], "x", 1);
    check(waits3(ep) == 111, "level-triggered: reported every time");
    close(ep);
    close(p[0]);
    close(p[1]);
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLET, 1);
    check(waits3(ep) == 100, "edge-triggered: reported once");
    close(ep);
    close(p[0]);
    close(p[1]);

    // Other kinds of descriptor.
    int s[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, s[0], EPOLLOUT | EPOLLONESHOT, 1);
    check(waits3(ep) == 100, "socket: reported once");
    close(ep);
    close(s[0]);
    close(s[1]);
    const int efd = eventfd(0, EFD_NONBLOCK);
    const uint64_t one = 1;
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, efd, EPOLLIN | EPOLLONESHOT, 1);
    (void)!write(efd, &one, sizeof(one));
    check(waits3(ep) == 100, "eventfd: reported once");
    close(ep);
    close(efd);

    // A fired registration stays silent when the other end goes away; re-arming reports the error.
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 1);
    epoll_wait(ep, out, 8, 0);
    close(p[0]);
    check(waits3(ep) == 0, "a disarmed registration is silent even when the read end closes");
    check(add(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT | EPOLLONESHOT, 1) == 0 && epoll_wait(ep, out, 8, 0) == 1 && (out[0].events & EPOLLERR), "re-arming reports the error");
    close(ep);
    close(p[1]);

    // Two registrations: each one fires on its own.
    pipe(p);
    pipe(q);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 1);
    add(ep, EPOLL_CTL_ADD, q[1], EPOLLOUT | EPOLLONESHOT, 2);
    check(epoll_wait(ep, out, 8, 0) == 2 && waits3(ep) == 0, "two one-shot registrations: both reported, then silence");
    add(ep, EPOLL_CTL_MOD, q[1], EPOLLOUT | EPOLLONESHOT, 2);
    check(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 2 && waits3(ep) == 0, "re-arming one leaves the other silent");
    close(ep);
    close(p[0]);
    close(p[1]);
    close(q[0]);
    close(q[1]);

    // The same through syscall(), which is how Bun's Rust code calls epoll_ctl.
    pipe(p);
    ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLONESHOT;
    ev.data.u64 = 77;
    check(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, p[1], &ev) == 0, "syscall(epoll_ctl) ADD");
    const long a = syscall(SYS_epoll_pwait, ep, out, 8, 0, NULL, 8), b = syscall(SYS_epoll_pwait, ep, out, 8, 0, NULL, 8);
    check(a == 1 && b == 0, "syscall(epoll_pwait): one report, then silence");
    check(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_MOD, p[1], &ev) == 0 && syscall(SYS_epoll_pwait, ep, out, 8, 0, NULL, 8) == 1, "syscall(epoll_ctl) MOD re-arms");
    close(ep);
    close(p[0]);
    close(p[1]);

    // epoll_pwait with a signal mask.
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 1);
    sigset_t mask;
    sigemptyset(&mask);
    const int pw1 = epoll_pwait(ep, out, 8, 0, &mask), pw2 = epoll_pwait(ep, out, 8, 0, &mask);
    check(pw1 == 1 && pw2 == 0, "epoll_pwait with a signal mask: one report, then silence");
    close(ep);
    close(p[0]);
    close(p[1]);

    // A new epoll descriptor that reuses a number does not inherit records of the old one.
    pipe(p);
    ep = epoll_create1(0);
    add(ep, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 1);
    epoll_wait(ep, out, 8, 0);
    close(ep);
    const int again = epoll_create1(0);
    check(add(again, EPOLL_CTL_ADD, p[1], EPOLLOUT | EPOLLONESHOT, 2) == 0 && waits3(again) == 100, "a new epoll set starts clean");
    printf("INFO the new epoll descriptor %s the old number (%d, %d)\n", again == ep ? "reused" : "did not reuse", ep, again);
    close(again);
    close(p[0]);
    close(p[1]);

    // epoll_pwait2 must not kill the caller. Each call is made in a child.
    for (int variant = 0; variant < 3; variant++) {
        if (variant == 2 && !epoll_pwait2) continue;
        const pid_t child = fork();
        if (child == 0) {
            int c[2];
            if (pipe(c)) _exit(20);
            const int set = epoll_create1(0);
            add(set, EPOLL_CTL_ADD, c[1], EPOLLOUT, 1);
            struct timespec zero = {0, 0};
            long r;
            if (variant == 0) r = syscall(SYS_epoll_pwait2, set, out, 8, &zero, NULL, 8);
            else if (variant == 1) { struct timespec tiny = {0, 500000}; r = syscall(SYS_epoll_pwait2, set, out, 8, &tiny, NULL, 8); }
            else r = epoll_pwait2(set, out, 8, &zero, NULL);
            _exit(r == 1 ? 0 : 21);
        }
        int status = 0;
        const char* names[] = {"syscall(epoll_pwait2) returns the ready event", "syscall(epoll_pwait2) with a sub-millisecond timeout works", "epoll_pwait2() returns the ready event"};
        check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, names[variant]);
        if (child > 0 && WIFSIGNALED(status)) printf("INFO killed by signal %d (SIGSYS is %d)\n", WTERMSIG(status), SIGSYS);
    }
    {
        pipe(p);
        ep = epoll_create1(0);
        add(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 1);
        struct timespec begin, tiny = {0, 500000};
        clock_gettime(CLOCK_MONOTONIC, &begin);
        const pid_t child = fork();
        if (child == 0) {
            const long r = syscall(SYS_epoll_pwait2, ep, out, 8, &tiny, NULL, 8);
            _exit(r == 0 ? 0 : 22);
        }
        int status = 0;
        waitpid(child, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && elapsed_ms(&begin) < 1000, "an idle epoll_pwait2 with a tiny timeout returns 0 soon");
        close(ep);
        close(p[0]);
        close(p[1]);
    }

    // Several threads, each with its own epoll set.
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    check(thread_failures == 0, "4 threads x 4000 arm-and-fire cycles");
    if (thread_failures) printf("INFO thread failures: %d\n", thread_failures);

    printf("EPOLL_C_TOTAL pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
