#include "native_ptrace_loader.h"
#include "native_seccomp_audit.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__aarch64__)
// No global file is modified. POKETEXT dirties only private pages in our fork child.
extern "C" const unsigned char HiSHExecutableTemplate[];
__asm__(".pushsection .text\n.balign 65536\n.global HiSHExecutableTemplate\n.hidden HiSHExecutableTemplate\n"
    "HiSHExecutableTemplate:\n.fill 16384,4,0xd4200000\n.popsection\n");
#endif

namespace hish {
#if defined(__aarch64__)
namespace {
struct Backing { uintptr_t address; uint64_t offset = 0; bool found = false; };
int FindBacking(dl_phdr_info* info, size_t, void* opaque)
{
    auto& backing = *static_cast<Backing*>(opaque);
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& p = info->dlpi_phdr[i];
        const uintptr_t begin = info->dlpi_addr + p.p_vaddr;
        if (p.p_type == PT_LOAD && (p.p_flags & PF_X) && backing.address >= begin &&
            backing.address - begin <= p.p_filesz && 65536 <= p.p_filesz - (backing.address - begin)) {
            backing.offset = p.p_offset + backing.address - begin;
            backing.found = true; return 1;
        }
    }
    return 0;
}
void KillOwned(pid_t child)
{
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
}
int Failure(const char* stage, pid_t child = -1)
{
    const int error = errno;
    dprintf(2, "hish-run: stage=%s errno=%d (%s)\n", stage, error, strerror(error));
    if (child > 0) KillOwned(child);
    errno = error; return -1;
}
struct Descriptor {
    int fd = -1;
    ~Descriptor() { if (fd >= 0) close(fd); }
};
}
#endif
bool MapExecutableTemplate(void* address, size_t length)
{
#if !defined(__aarch64__)
    (void)address; (void)length; errno = ENOTSUP; return false;
#else
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page > 65536 || 65536 % page || !length ||
        reinterpret_cast<uintptr_t>(address) % page || length % page) { errno = EINVAL; return false; }
    Dl_info info {};
    if (!dladdr(HiSHExecutableTemplate, &info) || !info.dli_fname) { errno = ENOENT; return false; }
    Backing backing {reinterpret_cast<uintptr_t>(HiSHExecutableTemplate)};
    dl_iterate_phdr(FindBacking, &backing);
    if (!backing.found || backing.offset % page) { errno = ENOEXEC; return false; }
    const int fd = open(info.dli_fname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    for (size_t at = 0; at < length; at += page) {
        // Repeated immutable backing page; each MAP_PRIVATE page has independent COW state.
        void* target = static_cast<unsigned char*>(address) + at;
        if (mmap(target, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, backing.offset) == MAP_FAILED) {
            const int error = errno; close(fd); errno = error; return false;
        }
    }
    close(fd); return true;
#endif
}
int RunPatchedImage(const std::vector<ExecutablePatch>& patches,
    const std::function<void()>& enter, bool diagnostic, bool notify)
{
#if !defined(__aarch64__)
    (void)patches; (void)enter; (void)diagnostic; (void)notify; errno = ENOTSUP; return -1;
#else
    size_t bytes = 0;
    for (const auto& patch : patches) {
        if (patch.address % sizeof(long) || patch.bytes.empty() || patch.bytes.size() % sizeof(long) ||
            patch.bytes.size() > 16 * 1024 * 1024 - bytes) { errno = E2BIG; return Failure("patch-bounds"); }
        bytes += patch.bytes.size();
    }
    int sockets[2] {-1, -1};
    if (notify && socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return Failure("notification-socket");
    Descriptor receiving {sockets[0]}, sending {sockets[1]}, listener;
    const pid_t parent = getpid(), child = fork();
    if (child < 0) return Failure("fork");
    if (!child) {
        // This supervisor is the dedicated command process, never an expiring NAPI worker.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
        if (notify) {
            close(receiving.fd);
            const int descriptor = CreateQueryListener(2);
            if (descriptor < 0 || !SendQueryListener(sending.fd, descriptor)) { Failure("notification-install-or-send"); _exit(126); }
            close(descriptor); close(sending.fd);
        }
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr)) { Failure("TRACEME"); _exit(126); }
        raise(SIGSTOP);
        if (notify && syscall(SYS_getppid) != parent) { Failure("notification-CONTINUE"); _exit(126); }
        for (const auto& patch : patches) {
            auto* begin = reinterpret_cast<char*>(patch.address);
            __builtin___clear_cache(begin, begin + patch.bytes.size());
        }
        if (diagnostic) dprintf(2, "HISH_RUN_NATIVE_ENTRY pid=%d ppid=%d\n", getpid(), getppid());
        enter(); _exit(127);
    }
    if (notify) {
        close(sending.fd); sending.fd = -1;
        listener.fd = ReceiveQueryListener(receiving.fd);
        if (listener.fd < 0) return Failure("notification-listener-receive", child);
    }
    int status = 0; bool stopped = false;
    for (int n = 0; n < 2000; ++n) {
        const pid_t result = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (result == child) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                dprintf(2,"hish-run: tracee exited before loading stop status=%d\n",status);
                errno=ECHILD;return Failure("tracee-before-stop");
            }
            stopped = WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP; break;
        }
        if (result < 0 && errno != EINTR) return Failure("wait-loading-stop", child);
        usleep(5000);
    }
    if (!stopped) { errno = ETIMEDOUT; return Failure("loading-stop-missing", child); }
    // Only load-time ptrace. No syscall tracing is left on the running program.
    if (ptrace(PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(PTRACE_O_EXITKILL))) return Failure("EXITKILL", child);
    for (const auto& patch : patches) for (size_t at = 0; at < patch.bytes.size(); at += sizeof(long)) {
        unsigned long word = 0; memcpy(&word, patch.bytes.data() + at, sizeof(word));
        if (ptrace(PTRACE_POKETEXT, child, reinterpret_cast<void*>(patch.address + at), reinterpret_cast<void*>(word)))
            return Failure("POKETEXT", child);
    }
    if (ptrace(PTRACE_DETACH, child, nullptr, nullptr)) return Failure("DETACH", child);
    if (diagnostic) dprintf(2, "HISH_RUN_DETACHED child=%d code_bytes=%zu\n", child, bytes);
    pid_t result; unsigned notifications = 0;
    if (notify) {
        for (;;) {
            result = waitpid(child, &status, WNOHANG);
            if (result == child || (result < 0 && errno != EINTR)) break;
            pollfd ready {listener.fd, POLLIN, 0};
            const int polled = poll(&ready, 1, 10);
            if (polled < 0 && errno != EINTR) return Failure("notification-poll", child);
            if (polled > 0 && (ready.revents & POLLIN)) {
                if (!ContinueQueryNotification(listener.fd)) return Failure("notification-CONTINUE-response", child);
                ++notifications;
            }
        }
        if (diagnostic) dprintf(2, "HISH_RUN_NOTIFICATIONS=%u\n", notifications);
    } else {
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    }
    if (result != child) return Failure("wait-runtime", child);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        // Propagate the actual signal where possible, preserving shell signal handling.
        const int signal = WTERMSIG(status);
        struct sigaction action {}; action.sa_handler = SIG_DFL; sigemptyset(&action.sa_mask);
        sigaction(signal, &action, nullptr); sigset_t mask; sigemptyset(&mask); sigaddset(&mask, signal);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr); raise(signal); return 128 + signal;
    }
    errno = ECHILD; return Failure("unexpected-runtime-status");
#endif
}
}
