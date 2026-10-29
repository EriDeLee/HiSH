#include "native_ptrace_audit.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

#if defined(__aarch64__)
extern "C" int HiSHPtraceOwnedCode();
extern "C" int HiSHPtraceOwnedArena(int);
__asm__(".pushsection .text\n.balign 8\n.global HiSHPtraceOwnedCode\n.type HiSHPtraceOwnedCode,%function\nHiSHPtraceOwnedCode:\nmov w0,#42\nret\n.size HiSHPtraceOwnedCode,.-HiSHPtraceOwnedCode\n.popsection\n");
__asm__(".pushsection .text\n.balign 4096\n.global HiSHPtraceOwnedArena\n.type HiSHPtraceOwnedArena,%function\nHiSHPtraceOwnedArena:\nmov w0,#42\nret\n.fill 1022,4,0xd503201f\n.size HiSHPtraceOwnedArena,.-HiSHPtraceOwnedArena\n.popsection\n");
#endif

namespace hish {
namespace {
using Log = std::function<void(const std::string&)>;
#if defined(__aarch64__)
void Record(int fd, const char* stage, long result, int error)
{
    dprintf(fd, "PTRACE stage=%s result=%ld errno=%d (%s)\n", stage, result, error, strerror(error));
    fsync(fd);
}
long Trace(bool direct, int request, pid_t pid, void* address = nullptr, void* data = nullptr)
{
    if (!direct) return ptrace(request, pid, address, data);
#if defined(__aarch64__)
    register long x0 __asm__("x0") = request;
    register long x1 __asm__("x1") = pid;
    register long x2 __asm__("x2") = reinterpret_cast<long>(address);
    register long x3 __asm__("x3") = reinterpret_cast<long>(data);
    register long x8 __asm__("x8") = SYS_ptrace;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    if (x0 < 0 && x0 >= -4095) { errno = static_cast<int>(-x0); return -1; }
    return x0;
#else
    errno = ENOSYS; return -1;
#endif
}
long Call(int fd, bool direct, const char* stage, int request, pid_t pid, void* address = nullptr, void* data = nullptr)
{
    dprintf(fd, "PTRACE stage=%s BEGIN\n", stage); fsync(fd);
    errno = 0;
    const long result = Trace(direct, request, pid, address, data);
    const int error = errno;
    Record(fd, stage, result, result < 0 ? error : 0);
    return result;
}
bool WaitStopped(int fd, pid_t child, int& status)
{
    for (int i = 0; i < 120; ++i) {
        const pid_t result = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (result == child) {
            Record(fd, "tracee-wait-status", status, 0);
            return WIFSTOPPED(status);
        }
        if (result < 0 && errno != EINTR) { Record(fd, "tracee-wait", result, errno); return false; }
        usleep(5000);
    }
    Record(fd, "tracee-wait-timeout", -1, ETIMEDOUT); return false;
}
void Reap(pid_t child)
{
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
}
void BindToTracer(pid_t tracer)
{
    // This is the dedicated tracer process's main thread, never a retiring NAPI worker.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != tracer) _exit(90);
}
int Self(int fd, bool direct)
{
    return Call(fd, direct, "TRACEME", PTRACE_TRACEME, 0) == 0 ? 0 : 10;
}
int Attach(int fd, bool direct, bool seize)
{
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) { Record(fd, "tracee-fork", -1, errno); return 11; }
    if (!child) { BindToTracer(tracer); for (;;) pause(); }
    const int request = seize ? PTRACE_SEIZE : PTRACE_ATTACH;
    if (Call(fd, direct, seize ? "SEIZE" : "ATTACH", request, child) < 0) { Reap(child); return 12; }
    if (seize && Call(fd, direct, "INTERRUPT", PTRACE_INTERRUPT, child) < 0) { Reap(child); return 13; }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 14; }
    const long detached = Call(fd, direct, "DETACH", PTRACE_DETACH, child);
    Reap(child); return detached == 0 ? 0 : 15;
}
struct Registers { uint64_t x[31], sp, pc, pstate; };
int Basic(int fd, bool direct)
{
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) { Record(fd, "tracee-fork", -1, errno); return 20; }
    if (!child) {
        BindToTracer(tracer);
        if (Call(fd, direct, "TRACEME", PTRACE_TRACEME, 0) < 0) _exit(21);
        raise(SIGSTOP);
        const long changed = syscall(SYS_getpid);
        Record(fd, "rewritten-getpid", changed, 0);
        _exit(changed == 424242 ? 0 : 22);
    }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 23; }
    const long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC;
    if (Call(fd, direct, "SETOPTIONS", PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(options)) < 0) { Reap(child); return 24; }
    Registers registers {}; iovec vector {&registers, sizeof(registers)};
    if (Call(fd, direct, "GETREGSET", PTRACE_GETREGSET, child, reinterpret_cast<void*>(NT_PRSTATUS), &vector) < 0 || vector.iov_len != sizeof(registers)) { Reap(child); return 25; }
    if (Call(fd, direct, "SETREGSET-unchanged", PTRACE_SETREGSET, child, reinterpret_cast<void*>(NT_PRSTATUS), &vector) < 0) { Reap(child); return 26; }
    int stops = 0; bool changed = false;
    for (int i = 0; i < 200; ++i) {
        if (Trace(direct, PTRACE_SYSCALL, child) < 0) { Record(fd, "SYSCALL-resume", -1, errno); Reap(child); return 27; }
        if (!WaitStopped(fd, child, status)) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && changed && stops >= 2) {
                dprintf(fd, "PTRACE_SYSCALL_REGISTER_REWRITE_OK stops=%d\n", stops); return 0;
            }
            Reap(child); return 28;
        }
        if (WSTOPSIG(status) != (SIGTRAP | 0x80)) { Reap(child); return 29; }
        ++stops; vector.iov_len = sizeof(registers);
        if (Trace(direct, PTRACE_GETREGSET, child, reinterpret_cast<void*>(NT_PRSTATUS), &vector) < 0) { Record(fd, "GETREGSET-stop", -1, errno); Reap(child); return 30; }
        if (!changed && registers.x[8] == SYS_getpid && registers.x[0] == static_cast<uint64_t>(child)) {
            registers.x[0] = 424242;
            if (Call(fd, direct, "SETREGSET-return", PTRACE_SETREGSET, child, reinterpret_cast<void*>(NT_PRSTATUS), &vector) < 0) { Reap(child); return 31; }
            changed = true;
        }
    }
    Reap(child); return 32;
}
int Continued(int fd, pid_t child)
{
    int status = 0;
    if (Call(fd, true, "CONT", PTRACE_CONT, child) < 0) { Reap(child); return 40; }
    for (int i = 0; i < 400; ++i) {
        const pid_t result = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (result == child) {
            Record(fd, "continued-tracee-status", status, 0);
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            if (WIFSTOPPED(status)) { Trace(true, PTRACE_CONT, child, nullptr, reinterpret_cast<void*>(static_cast<long>(WSTOPSIG(status)))); }
        } else if (result < 0 && errno != EINTR) { Record(fd, "continued-wait", -1, errno); return 41; }
        usleep(1000);
    }
    Reap(child); return 42;
}
int Memory(int fd, const std::string& root, bool file)
{
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) return 43;
    if (!child) {
        BindToTracer(tracer);
        if (Call(fd, true, "TRACEME-memory", PTRACE_TRACEME, 0) < 0) _exit(44);
        raise(SIGSTOP);
        const uint32_t code[] = {0x52800540, 0xd65f03c0};
        void* page = MAP_FAILED; int backing = -1;
        const std::string path = root + "/home/.hish-ptrace-code";
        if (file) {
            backing = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0700);
            if (backing < 0 || ftruncate(backing, 4096) || pwrite(backing, code, sizeof(code), 0) != sizeof(code)) _exit(45);
            errno = 0; page = mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, backing, 0);
            Record(fd, "traced-HOME-RX-mmap", page == MAP_FAILED ? -1 : 0, page == MAP_FAILED ? errno : 0);
            close(backing); unlink(path.c_str());
        } else {
            page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page == MAP_FAILED) _exit(46);
            memcpy(page, code, sizeof(code)); errno = 0;
            const int result = mprotect(page, 4096, PROT_READ | PROT_EXEC);
            Record(fd, "traced-anonymous-RW-to-RX", result, result < 0 ? errno : 0);
            if (result) { munmap(page, 4096); _exit(0); }
        }
        if (page != MAP_FAILED) {
            const int value = reinterpret_cast<int (*)()>(page)();
            Record(fd, "traced-new-code-result", value, 0); munmap(page, 4096);
            if (value != 42) _exit(47);
        }
        _exit(0);
    }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 48; }
    return Continued(fd, child);
}
alignas(long) char rewritePath[64] = "/nonexistent/hish-owned-ptrace-path";
int Poke(int fd, bool executable)
{
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) return 50;
    if (!child) {
        BindToTracer(tracer);
        if (executable && HiSHPtraceOwnedCode() != 42) _exit(51);
        if (Call(fd, true, "TRACEME-poke", PTRACE_TRACEME, 0) < 0) _exit(52);
        raise(SIGSTOP);
        if (executable) {
            auto* code = reinterpret_cast<char*>(&HiSHPtraceOwnedCode);
            __builtin___clear_cache(code, code + 8);
            const int value = HiSHPtraceOwnedCode();
            Record(fd, "owned-signed-code-after-poke", value, 0);
            _exit(value == 43 ? 0 : 53);
        }
        const int opened = open(rewritePath, O_RDONLY | O_CLOEXEC);
        Record(fd, "rewritten-path-open", opened, opened < 0 ? errno : 0);
        if (opened < 0) _exit(54);
        close(opened); _exit(0);
    }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 55; }
    if (executable) {
        const uintptr_t code = static_cast<uintptr_t>(0xd65f03c052800560ULL);
        if (Call(fd, true, "POKETEXT-owned-signed-page", PTRACE_POKETEXT, child, reinterpret_cast<void*>(&HiSHPtraceOwnedCode), reinterpret_cast<void*>(code)) < 0) { Reap(child); return 56; }
    } else {
        unsigned long before = 0;
        if (Call(fd, true, "PEEKDATA-owned-path", PTRACE_PEEKDATA, child, rewritePath, &before) < 0 || memcmp(&before, "/nonexis", sizeof(before))) { Reap(child); return 57; }
        const char replacement[16] = "/dev/null";
        for (size_t i = 0; i < sizeof(replacement); i += sizeof(unsigned long)) {
            unsigned long word = 0; memcpy(&word, replacement + i, sizeof(word));
            if (Call(fd, true, "POKEDATA-owned-path", PTRACE_POKEDATA, child, rewritePath + i, reinterpret_cast<void*>(word)) < 0) { Reap(child); return 58; }
        }
    }
    return Continued(fd, child);
}
void* Thread(void*) { return nullptr; }
int Events(int fd)
{
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) return 60;
    if (!child) {
        BindToTracer(tracer);
        if (Call(fd, true, "TRACEME-events", PTRACE_TRACEME, 0) < 0) _exit(61);
        raise(SIGSTOP);
        const pid_t spawned = fork();
        if (spawned < 0) _exit(62);
        if (!spawned) { execl("/system/bin/toybox", "toybox", "true", nullptr); _exit(63); }
        int status = 0; while (waitpid(spawned, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status)) _exit(64);
        pthread_t thread; if (pthread_create(&thread, nullptr, Thread, nullptr) || pthread_join(thread, nullptr)) _exit(65);
        _exit(0);
    }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 66; }
    const long options = PTRACE_O_TRACEFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC;
    if (Call(fd, true, "SETOPTIONS-events", PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(options)) < 0) { Reap(child); return 67; }
    if (Trace(true, PTRACE_CONT, child) < 0) { Reap(child); return 68; }
    int live = 1, rootStatus = -1; bool forked = false, cloned = false, executed = false;
    for (int i = 0; i < 2000 && live; ++i) {
        const pid_t stopped = waitpid(-1, &status, WNOHANG | __WALL);
        if (stopped == 0) { usleep(1000); continue; }
        if (stopped < 0) { if (errno == EINTR) continue; Record(fd, "events-wait", -1, errno); return 69; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) { --live; if (stopped == child) rootStatus = status; continue; }
        const unsigned event = static_cast<unsigned>(status) >> 16;
        Record(fd, "event-stop", event, 0);
        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_CLONE) {
            unsigned long descendant = 0;
            if (Call(fd, true, "GETEVENTMSG", PTRACE_GETEVENTMSG, stopped, nullptr, &descendant) < 0) return 70;
            ++live; forked |= event == PTRACE_EVENT_FORK; cloned |= event == PTRACE_EVENT_CLONE;
        }
        executed |= event == PTRACE_EVENT_EXEC;
        const int signal = WSTOPSIG(status), deliver = signal == SIGSTOP || signal == SIGTRAP ? 0 : signal;
        if (Trace(true, PTRACE_CONT, stopped, nullptr, reinterpret_cast<void*>(static_cast<long>(deliver))) < 0) return 71;
    }
    dprintf(fd, "PTRACE_EVENTS fork=%d clone=%d exec=%d live=%d root_status=%d\n", forked, cloned, executed, live, rootStatus);
    return live == 0 && rootStatus == 0 && forked && cloned && executed ? 0 : 72;
}
int Compiled(int fd, const std::string& root)
{
    const std::string file = root + "/home/.hish-proot-evaluation/function.o";
    std::ifstream stream(file, std::ios::binary);
    const std::vector<unsigned char> bytes {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    auto bounded = [&](uint64_t offset, uint64_t count) { return offset <= bytes.size() && count <= bytes.size() - offset; };
    if (bytes.size() < sizeof(Elf64_Ehdr) || bytes.size() > 65536) { Record(fd, "compiled-object-size", bytes.size(), ENOEXEC); return 81; }
    Elf64_Ehdr header {}; memcpy(&header, bytes.data(), sizeof(header));
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_machine != EM_AARCH64 || header.e_type != ET_REL || header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum > 128 || !bounded(header.e_shoff, header.e_shnum * sizeof(Elf64_Shdr)) || header.e_shstrndx >= header.e_shnum) return 82;
    std::vector<Elf64_Shdr> sections(header.e_shnum); memcpy(sections.data(), bytes.data() + header.e_shoff, sections.size() * sizeof(Elf64_Shdr));
    const auto strings = sections[header.e_shstrndx]; if (!bounded(strings.sh_offset, strings.sh_size)) return 83;
    size_t selected = 0;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto section = sections[i];
        if (section.sh_name < strings.sh_size && memchr(bytes.data() + strings.sh_offset + section.sh_name, 0, strings.sh_size - section.sh_name) && !strcmp(reinterpret_cast<const char*>(bytes.data() + strings.sh_offset + section.sh_name), ".text")) selected = i;
    }
    const auto text = sections[selected];
    if (!selected || text.sh_type != SHT_PROGBITS || !(text.sh_flags & SHF_EXECINSTR) || !text.sh_size || text.sh_size > 4096 || !bounded(text.sh_offset, text.sh_size)) return 84;
    bool entryFound = false;
    for (const auto& section : sections) if (section.sh_type == SHT_SYMTAB && section.sh_entsize == sizeof(Elf64_Sym) && section.sh_link < sections.size() && bounded(section.sh_offset, section.sh_size)) {
        const auto names = sections[section.sh_link]; if (!bounded(names.sh_offset, names.sh_size)) return 96;
        for (size_t at = 0; at + sizeof(Elf64_Sym) <= section.sh_size; at += sizeof(Elf64_Sym)) {
            Elf64_Sym symbol {}; memcpy(&symbol, bytes.data() + section.sh_offset + at, sizeof(symbol));
            if (symbol.st_name < names.sh_size && memchr(bytes.data() + names.sh_offset + symbol.st_name, 0, names.sh_size - symbol.st_name) && !strcmp(reinterpret_cast<const char*>(bytes.data() + names.sh_offset + symbol.st_name), "hish_eval")) entryFound = symbol.st_shndx == selected && symbol.st_value == 0;
        }
    }
    if (!entryFound) return 97;
    for (const auto& section : sections) if ((section.sh_type == SHT_RELA || section.sh_type == SHT_REL) && section.sh_info == selected && section.sh_size) { Record(fd, "compiled-text-relocations-rejected", section.sh_size, ENOTSUP); return 85; }
    Record(fd, "phone-compiled-text-bytes", text.sh_size, 0);
    dprintf(fd, "PTRACE_COMPILED_TEXT_HEX=");
    for (size_t i = 0; i < text.sh_size; ++i) dprintf(fd, "%02x", bytes[text.sh_offset + i]);
    dprintf(fd, "\n"); fsync(fd);
    if (HiSHPtraceOwnedArena(37) != 42) return 86;
    const pid_t tracer = getpid(), child = fork();
    if (child < 0) return 87;
    if (!child) {
        BindToTracer(tracer);
        if (Call(fd, true, "TRACEME-phone-compiled", PTRACE_TRACEME, 0) < 0) _exit(88);
        raise(SIGSTOP);
        auto* start = reinterpret_cast<char*>(&HiSHPtraceOwnedArena); __builtin___clear_cache(start, start + 4096);
        for (const int value : {0, 1, 37, 100}) {
            const int result = HiSHPtraceOwnedArena(value), expected = value * (value + 1) / 2 + 17;
            dprintf(fd, "PTRACE_COMPILED_RESULT input=%d actual=%d expected=%d\n", value, result, expected);
            if (result != expected) _exit(89);
        }
        std::ifstream status("/proc/self/status"); std::string line;
        while (std::getline(status, line)) if (line.rfind("TracerPid:", 0) == 0) dprintf(fd, "PTRACE_COMPILED_%s\n", line.c_str());
        dprintf(fd, "PTRACE_PHONE_COMPILED_FUNCTION_OK\n"); fsync(fd); _exit(0);
    }
    int status = 0;
    if (!WaitStopped(fd, child, status)) { Reap(child); return 91; }
    for (size_t offset = 0; offset < text.sh_size; offset += sizeof(unsigned long)) {
        unsigned long word = 0; const size_t count = std::min(sizeof(word), static_cast<size_t>(text.sh_size) - offset);
        memcpy(&word, bytes.data() + text.sh_offset + offset, count);
        if (Trace(true, PTRACE_POKETEXT, child, reinterpret_cast<char*>(&HiSHPtraceOwnedArena) + offset, reinterpret_cast<void*>(word)) < 0) { Record(fd, "compiled-POKETEXT", -1, errno); Reap(child); return 92; }
    }
    if (Call(fd, true, "DETACH-before-native-computation", PTRACE_DETACH, child) < 0) { Reap(child); return 93; }
    const bool stopped = WaitStopped(fd, child, status);
    if (stopped || !WIFEXITED(status) || WEXITSTATUS(status)) { Reap(child); return 94; }
    const int unchanged = HiSHPtraceOwnedArena(37); Record(fd, "parent-arena-still-original", unchanged, 0);
    return unchanged == 42 ? 0 : 95;
}
void Isolated(const Log& log, int fd, const std::string& name, const std::function<int()>& action)
{
    log("PTRACE CASE " + name + " BEGIN");
    const pid_t child = fork();
    if (!child) {
        if (setpgid(0, 0)) { Record(fd, "owned-process-group", -1, errno); _exit(80); }
        _exit(action());
    }
    if (child < 0) { log("PTRACE CASE " + name + " fork errno=" + std::to_string(errno)); return; }
    // Both sides establish this private group, containing only our own test descendants.
    setpgid(child, child);
    for (int i = 0; i < 600; ++i) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            kill(-child, SIGKILL);
            if (WIFEXITED(status)) log("PTRACE CASE " + name + " exit=" + std::to_string(WEXITSTATUS(status)));
            else if (WIFSIGNALED(status)) log("PTRACE CASE " + name + " signal=" + std::to_string(WTERMSIG(status)));
            return;
        }
        if (result < 0 && errno != EINTR) { kill(-child, SIGKILL); log("PTRACE CASE " + name + " wait errno=" + std::to_string(errno)); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(-child, SIGKILL); kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    log("PTRACE CASE " + name + " TIMEOUT owned group killed");
}
#endif
}
void PtraceAudit(int reportFd, const Log& log)
{
#if defined(__aarch64__)
    log("PTRACE_AUDIT architecture=aarch64 syscall=" + std::to_string(SYS_ptrace) + " uid=" + std::to_string(getuid()));
    for (const bool direct : {false, true}) {
        const std::string mode = direct ? "inline" : "libc";
        Isolated(log, reportFd, mode + "-traceme", [&] { return Self(reportFd, direct); });
        Isolated(log, reportFd, mode + "-attach", [&] { return Attach(reportFd, direct, false); });
        Isolated(log, reportFd, mode + "-syscalls", [&] { return Basic(reportFd, direct); });
    }
    Isolated(log, reportFd, "inline-seize", [&] { return Attach(reportFd, true, true); });
    log("PTRACE_AUDIT_END");
#else
    (void)reportFd; log("PTRACE_AUDIT unsupported architecture");
#endif
}
void PtraceAdvancedAudit(const std::string& root, int reportFd, const Log& log)
{
#if defined(__aarch64__)
    Isolated(log, reportFd, "path-memory-rewrite", [&] { return Poke(reportFd, false); });
    Isolated(log, reportFd, "signed-code-poke", [&] { return Poke(reportFd, true); });
    Isolated(log, reportFd, "traced-HOME-RX", [&] { return Memory(reportFd, root, true); });
    Isolated(log, reportFd, "traced-anonymous-RX", [&] { return Memory(reportFd, root, false); });
    Isolated(log, reportFd, "fork-clone-exec-events", [&] { return Events(reportFd); });
    log("PTRACE_ADVANCED_END");
#else
    (void)root; (void)reportFd; log("PTRACE_ADVANCED unsupported architecture");
#endif
}
void PtraceCompiledAudit(const std::string& root, int reportFd, const Log& log)
{
#if defined(__aarch64__)
    Isolated(log, reportFd, "phone-compiled-native-function", [&] { return Compiled(reportFd, root); });
    log("PTRACE_COMPILED_END");
#else
    (void)root; (void)reportFd; log("PTRACE_COMPILED unsupported architecture");
#endif
}
}
