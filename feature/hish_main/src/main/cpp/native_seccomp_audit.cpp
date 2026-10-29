#include "native_seccomp_audit.h"
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace hish {
namespace {
void Stage(int fd, const char* name, long value, int error = 0)
{
    dprintf(fd, "SECCOMP stage=%s result=%ld errno=%d (%s)\n", name, value, error, strerror(error)); fsync(fd);
}
long Call(int fd, const char* name, unsigned operation, unsigned flags, void* data)
{
    dprintf(fd, "SECCOMP stage=%s BEGIN\n", name); fsync(fd); errno = 0;
    const long result = syscall(SYS_seccomp, operation, flags, data);
    Stage(fd, name, result, result < 0 ? errno : 0); return result;
}
bool SendListener(int socket, int listener)
{
    char tag = 'L'; iovec io {&tag, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    msghdr message {}; message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    auto* header = CMSG_FIRSTHDR(&message); header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(header), &listener, sizeof(listener));
    return sendmsg(socket, &message, MSG_NOSIGNAL) == 1;
}
int ReceiveListener(int socket)
{
    pollfd ready {socket, POLLIN, 0};
    if (poll(&ready, 1, 1500) != 1) { errno = ETIMEDOUT; return -1; }
    char tag = 0; iovec io {&tag, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    msghdr message {}; message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    const ssize_t count = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    auto* header = CMSG_FIRSTHDR(&message);
    if (count != 1 || tag != 'L' || (message.msg_flags & MSG_CTRUNC) || !header ||
        header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int))) {
        errno = EPROTO; return -1;
    }
    int listener; memcpy(&listener, CMSG_DATA(header), sizeof(listener)); return listener;
}
int RoundTrip(int fd)
{
    seccomp_notif_sizes sizes {};
    if (Call(fd, "GET_NOTIF_SIZES", SECCOMP_GET_NOTIF_SIZES, 0, &sizes)) return 20;
    if (sizes.seccomp_notif < sizeof(seccomp_notif) || sizes.seccomp_notif_resp < sizeof(seccomp_notif_resp) ||
        sizes.seccomp_notif > 4096 || sizes.seccomp_notif_resp > 4096) return 21;
    int sockets[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return 22;
    const pid_t parent = getpid(), target = fork();
    if (target < 0) { close(sockets[0]); close(sockets[1]); return 23; }
    if (!target) {
        close(sockets[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(24);
        const int listener = CreateQueryListener(fd);
        if (listener < 0) _exit(27);
        if (!SendListener(sockets[1], listener)) _exit(28);
        close(listener); close(sockets[1]);
        const long value = syscall(SYS_getppid); Stage(fd, "mediated-getppid", value);
        _exit(value == 424242 ? 0 : 29);
    }
    close(sockets[1]); const int listener = ReceiveListener(sockets[0]); close(sockets[0]);
    int outcome = 30;
    if (listener >= 0) {
        pollfd ready {listener, POLLIN, 0};
        if (poll(&ready, 1, 1500) == 1) {
            // vector storage is suitably aligned for these non-overaligned UAPI structs.
            std::vector<unsigned char> requestBytes(sizes.seccomp_notif), responseBytes(sizes.seccomp_notif_resp);
            auto* request = reinterpret_cast<seccomp_notif*>(requestBytes.data());
            auto* response = reinterpret_cast<seccomp_notif_resp*>(responseBytes.data());
            dprintf(fd, "SECCOMP stage=NOTIF_RECV BEGIN\n"); fsync(fd);
            if (!ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, request) && request->pid == static_cast<unsigned>(target) && request->data.nr == SYS_getppid) {
                response->id = request->id; response->val = 424242; response->error = 0; response->flags = 0;
                if (!ioctl(listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &request->id) && !ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, response)) outcome = 0;
            }
            Stage(fd, "notification-response", outcome, outcome ? errno : 0);
        } else Stage(fd, "notification-poll", -1, ETIMEDOUT);
        close(listener);
    } else Stage(fd, "listener-receive", -1, errno);
    if (outcome) kill(target, SIGKILL);
    int status = 0; pid_t waited;
    do { waited = waitpid(target, &status, 0); } while (waited < 0 && errno == EINTR);
    Stage(fd, "target-status", status, waited < 0 ? errno : 0);
    if (!outcome && waited == target && WIFEXITED(status) && !WEXITSTATUS(status)) {
        dprintf(fd, "SECCOMP_NOTIFICATION_ROUNDTRIP_OK\n"); fsync(fd); return 0;
    }
    return outcome ? outcome : 31;
}
}
int CreateQueryListener(int fd)
{
    dprintf(fd, "SECCOMP stage=NO_NEW_PRIVS BEGIN\n"); fsync(fd);
    const int result = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    Stage(fd, "NO_NEW_PRIVS", result, result ? errno : 0); if (result) return -1;
#if defined(__aarch64__)
    constexpr unsigned architecture = AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
    constexpr unsigned architecture = AUDIT_ARCH_X86_64;
#else
    errno = ENOTSUP; return -1;
#endif
    sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    sock_fprog program {static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
    return Call(fd, "NEW_LISTENER", SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &program);
}
bool SendQueryListener(int socket, int listener) { return SendListener(socket, listener); }
int ReceiveQueryListener(int socket) { return ReceiveListener(socket); }
bool ContinueQueryNotification(int listener)
{
    seccomp_notif_sizes sizes {};
    if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) ||
        sizes.seccomp_notif < sizeof(seccomp_notif) || sizes.seccomp_notif_resp < sizeof(seccomp_notif_resp) ||
        sizes.seccomp_notif > 4096 || sizes.seccomp_notif_resp > 4096) { errno = ENOTSUP; return false; }
    std::vector<unsigned char> input(sizes.seccomp_notif), output(sizes.seccomp_notif_resp);
    auto* request = reinterpret_cast<seccomp_notif*>(input.data());
    auto* response = reinterpret_cast<seccomp_notif_resp*>(output.data());
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, request)) return false;
    if (request->data.nr != SYS_getppid) { errno = EPROTO; return false; }
    response->id = request->id; response->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &request->id)) return errno == ENOENT;
    // Scalar query: there is no user-pointer TOCTOU window or filesystem emulation here.
    if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, response)) return errno == ENOENT;
    return true;
}
void SeccompAudit(int reportFd, const std::function<void(const std::string&)>& log)
{
    for (int kind = 0; kind < 3; ++kind) {
        const std::string name = kind == 0 ? "action-availability" : kind == 1 ? "notification-sizes" : "notification-roundtrip";
        log("SECCOMP CASE " + name + " BEGIN");
        const pid_t child = fork();
        if (!child) {
            if (setpgid(0, 0)) _exit(90);
            if (kind == 2) _exit(RoundTrip(reportFd));
            unsigned action = SECCOMP_RET_USER_NOTIF; seccomp_notif_sizes sizes {};
            _exit(Call(reportFd, kind ? "GET_NOTIF_SIZES" : "GET_ACTION_AVAIL", kind ? SECCOMP_GET_NOTIF_SIZES : SECCOMP_GET_ACTION_AVAIL,
                0, kind ? static_cast<void*>(&sizes) : static_cast<void*>(&action)) == 0 ? 0 : 10);
        }
        if (child < 0) { log("SECCOMP CASE " + name + " fork errno=" + std::to_string(errno)); continue; }
        const bool ownedGroup = setpgid(child, child) == 0;
        bool complete = false;
        for (int n = 0; n < 1200; ++n) {
            int status = 0; const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                if (ownedGroup) kill(-child, SIGKILL);
                log("SECCOMP CASE " + name + (WIFEXITED(status) ? " exit=" + std::to_string(WEXITSTATUS(status)) : " signal=" + std::to_string(WTERMSIG(status))));
                complete = true; break;
            }
            if (result < 0 && errno != EINTR) { log("SECCOMP CASE " + name + " wait errno=" + std::to_string(errno)); break; }
            usleep(5000);
        }
        if (!complete) {
            if (ownedGroup) kill(-child, SIGKILL); kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            log("SECCOMP CASE " + name + " TIMEOUT owned children killed");
        }
    }
    log("SECCOMP_AUDIT_END");
}
}
