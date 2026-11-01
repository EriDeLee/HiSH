// epoll fixes for Linux programs, musl and glibc (docs/epoll.md). This preload library replaces epoll_ctl, epoll_pwait and
// epoll_wait, and the same calls made through syscall() (Bun and Rust's libc crate do that; linux/links.c passes them on).
//
// 1. This phone's kernel ignores EPOLLONESHOT: a descriptor registered with it is reported again on every epoll_wait, so a
//    program that registers its always-writable stdout that way (Bun does) never sleeps. hish_epoll_core.h emulates the flag.
//    The emulation switches itself off if the kernel turns out to honour the flag, and HISH_EPOLL=0 / HISH_EPOLL=1 force it
//    off / on.
// 2. The app's seccomp filter kills a process that calls epoll_pwait2 (syscall 441). It is turned into epoll_pwait, with the
//    timeout rounded up to whole milliseconds. This part is always on.
//
// Programs that issue raw svc instructions for epoll bypass all of this.
#define _GNU_SOURCE
#include "hish_epoll_core.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>

#if !defined(__aarch64__)
#error "the epoll fixes are implemented for AArch64 only"
#endif

enum {
    EP_SYS_EPOLL_CREATE1 = 20, EP_SYS_EPOLL_CTL = 21, EP_SYS_EPOLL_PWAIT = 22, EP_SYS_CLOSE = 57, EP_SYS_PIPE2 = 59,
    EP_SYS_SCHED_YIELD = 124, EP_SYS_EPOLL_PWAIT2 = 441
};
// The kernel's size of the signal set argument (_NSIG / 8 on AArch64).
#define EP_SIGSET_SIZE 8

static inline long ep_raw(long number, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

// The kernel returns -errno; libc turns that into -1 and errno.
static long ep_finish(long result)
{
    if ((unsigned long)result > (unsigned long)-4096) {
        errno = (int)-result;
        return -1;
    }
    return result;
}

static struct he_table ep_table;
static int ep_lock_word;

static void ep_lock(void)
{
    int spins = 0;
    while (__atomic_exchange_n(&ep_lock_word, 1, __ATOMIC_ACQUIRE)) {
        if (++spins >= 64) {
            ep_raw(EP_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
            spins = 0;
        }
    }
}

static void ep_unlock(void)
{
    __atomic_store_n(&ep_lock_word, 0, __ATOMIC_RELEASE);
}

// Registrations the table remembers (armed or fired), and armed ones. Read without the lock: they only tell a call whether it can
// skip the table.
static int ep_tracked(void)
{
    return __atomic_load_n(&ep_table.armed, __ATOMIC_RELAXED) + __atomic_load_n(&ep_table.fired, __ATOMIC_RELAXED) != 0;
}

static int ep_armed(void)
{
    return __atomic_load_n(&ep_table.armed, __ATOMIC_RELAXED) != 0;
}

static long ep_kernel_ctl(void* context, int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event)
{
    (void)context;
    struct epoll_event event;
    event.events = events;
    event.data.u64 = data;
    return ep_raw(EP_SYS_EPOLL_CTL, epfd, op, fd, (long)(with_event ? &event : NULL), 0, 0);
}

enum { EP_MODE_UNKNOWN = 0, EP_MODE_NATIVE = 1, EP_MODE_EMULATE = 2, EP_MODE_OFF = 3 };
static int ep_mode_value;

// Does the kernel honour EPOLLONESHOT? Register a pipe's write end (always writable) with it and ask twice without waiting.
static int ep_probe_kernel(void)
{
    int pipefd[2];
    int mode = EP_MODE_UNKNOWN;
    if (ep_raw(EP_SYS_PIPE2, (long)pipefd, O_CLOEXEC, 0, 0, 0, 0) < 0) return mode;
    const long set = ep_raw(EP_SYS_EPOLL_CREATE1, EPOLL_CLOEXEC, 0, 0, 0, 0, 0);
    if (set >= 0) {
        struct epoll_event event, out[2];
        event.events = EPOLLOUT | EPOLLONESHOT;
        event.data.u64 = 1;
        if (ep_raw(EP_SYS_EPOLL_CTL, set, EPOLL_CTL_ADD, pipefd[1], (long)&event, 0, 0) == 0) {
            const long first = ep_raw(EP_SYS_EPOLL_PWAIT, set, (long)out, 2, 0, 0, EP_SIGSET_SIZE);
            const long second = ep_raw(EP_SYS_EPOLL_PWAIT, set, (long)out, 2, 0, 0, EP_SIGSET_SIZE);
            if (first == 1) mode = second >= 1 ? EP_MODE_EMULATE : second == 0 ? EP_MODE_NATIVE : EP_MODE_UNKNOWN;
        }
        ep_raw(EP_SYS_CLOSE, set, 0, 0, 0, 0, 0);
    }
    ep_raw(EP_SYS_CLOSE, pipefd[0], 0, 0, 0, 0, 0);
    ep_raw(EP_SYS_CLOSE, pipefd[1], 0, 0, 0, 0, 0);
    return mode;
}

static int ep_decide_mode(void)
{
    const char* value = getenv("HISH_EPOLL");
    int mode;
    if (value && value[0] == '0' && !value[1]) mode = EP_MODE_OFF;
    else if (value && value[0] == '1' && !value[1]) mode = EP_MODE_EMULATE;
    else mode = ep_probe_kernel();
    if (mode != EP_MODE_UNKNOWN) __atomic_store_n(&ep_mode_value, mode, __ATOMIC_RELEASE);
    return mode;
}

// Decided on the first one-shot registration; a probe that could not run is tried again at the next one.
static int ep_emulating(void)
{
    int mode = __atomic_load_n(&ep_mode_value, __ATOMIC_ACQUIRE);
    if (mode == EP_MODE_UNKNOWN) mode = ep_decide_mode();
    return mode == EP_MODE_EMULATE;
}

// The program's epoll_ctl as the kernel would answer it.
static long ep_ctl(int epfd, int op, int fd, const struct epoll_event* event)
{
    const uint32_t events = event ? event->events : 0;
    const int oneshot = event && (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && (events & EPOLLONESHOT);
    if (oneshot ? !ep_emulating() : !ep_tracked()) return ep_raw(EP_SYS_EPOLL_CTL, epfd, op, fd, (long)event, 0, 0);
    ep_lock();
    const long r = he_ctl(&ep_table, ep_kernel_ctl, NULL, epfd, op, fd, events, event ? event->data.u64 : 0, event != NULL);
    ep_unlock();
    return r;
}

static void ep_after_wait(int epfd, const struct epoll_event* events, int count)
{
    ep_lock();
    he_wait_done(&ep_table, ep_kernel_ctl, NULL, epfd, events, sizeof(struct epoll_event), offsetof(struct epoll_event, data), count);
    ep_unlock();
}

static long ep_created(long result)
{
    if (result >= 0 && ep_tracked()) {
        ep_lock();
        he_purge_epfd(&ep_table, (int)result);
        ep_unlock();
    }
    return result;
}

// epoll_pwait2 takes a timespec, epoll_pwait milliseconds. Round up, so a sub-millisecond wait is not a busy poll.
static int ep_timeout_ms(const struct timespec* timeout, int* milliseconds)
{
    if (!timeout) {
        *milliseconds = -1;
        return 0;
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L) return EINVAL;
    if (timeout->tv_sec > (INT_MAX - 1000) / 1000) {
        *milliseconds = INT_MAX;
        return 0;
    }
    *milliseconds = (int)(timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999L) / 1000000L);
    return 0;
}

// For callers of syscall() (linux/links.c asks first). Returns 1 and the kernel-style result when the call is ours.
__attribute__((visibility("default"))) int hish_epoll_syscall(long number, long a, long b, long c, long d, long e, long f, long* result)
{
    switch (number) {
    case EP_SYS_EPOLL_CREATE1:
        *result = ep_created(ep_raw(EP_SYS_EPOLL_CREATE1, a, 0, 0, 0, 0, 0));
        return 1;
    case EP_SYS_EPOLL_CTL:
        *result = ep_ctl((int)a, (int)b, (int)c, (const struct epoll_event*)d);
        return 1;
    case EP_SYS_EPOLL_PWAIT:
        *result = ep_raw(EP_SYS_EPOLL_PWAIT, a, b, c, d, e, f);
        if (*result > 0 && ep_armed()) ep_after_wait((int)a, (const struct epoll_event*)b, (int)*result);
        return 1;
    case EP_SYS_EPOLL_PWAIT2: {
        int milliseconds;
        const int bad = ep_timeout_ms((const struct timespec*)d, &milliseconds);
        if (bad) {
            *result = -bad;
            return 1;
        }
        *result = ep_raw(EP_SYS_EPOLL_PWAIT, a, b, c, milliseconds, e, f);
        if (*result > 0 && ep_armed()) ep_after_wait((int)a, (const struct epoll_event*)b, (int)*result);
        return 1;
    }
    default:
        return 0;
    }
}

int epoll_create1(int flags)
{
    return (int)ep_finish(ep_created(ep_raw(EP_SYS_EPOLL_CREATE1, flags, 0, 0, 0, 0, 0)));
}

int epoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create1(0);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)
{
    return (int)ep_finish(ep_ctl(epfd, op, fd, event));
}

typedef int (*ep_pwait_fn)(int, struct epoll_event*, int, int, const sigset_t*);

// The C library's own epoll_pwait keeps its cancellation and restart behaviour; the raw call is only the fallback.
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask)
{
    static ep_pwait_fn next;
    if (!next) next = (ep_pwait_fn)dlsym(RTLD_NEXT, "epoll_pwait");
    int r;
    if (next) r = next(epfd, events, maxevents, timeout, sigmask);
    else r = (int)ep_finish(ep_raw(EP_SYS_EPOLL_PWAIT, epfd, (long)events, maxevents, timeout, (long)sigmask, EP_SIGSET_SIZE));
    if (r > 0 && ep_armed()) ep_after_wait(epfd, events, r);
    return r;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
    return epoll_pwait(epfd, events, maxevents, timeout, NULL);
}

int epoll_pwait2(int epfd, struct epoll_event* events, int maxevents, const struct timespec* timeout, const sigset_t* sigmask)
{
    int milliseconds;
    const int bad = ep_timeout_ms(timeout, &milliseconds);
    if (bad) {
        errno = bad;
        return -1;
    }
    return epoll_pwait(epfd, events, maxevents, milliseconds, sigmask);
}
