#include "native_route_audit.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <linux/ashmem.h>
#include <signal.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/shm.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <ucontext.h>
#include <sys/prctl.h>
#include <spawn.h>
#include <sys/personality.h>
#include <sys/uio.h>
extern char** environ;
namespace hish {
namespace {
using Log = std::function<void(const std::string&)>;
long Raw(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return x0;
#else
    (void)number; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -ENOSYS;
#endif
}
void Error(const Log& log, const std::string& stage, int error) { log(stage + " errno=" + std::to_string(error) + " (" + strerror(error) + ")"); }
bool Range(size_t off, size_t size, size_t limit) { return off <= limit && size <= limit - off; }
std::vector<unsigned char> Bytes(const std::string& path)
{
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) return {};
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void Child(const Log& log, int reportFd, const std::string& tag, const std::function<int()>& action)
{
    log("CASE " + tag + " BEGIN");
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(reportFd, STDOUT_FILENO); dup2(reportFd, STDERR_FILENO);
        _exit(action());
    }
    if (pid < 0) { Error(log, tag + " fork", errno); return; }
    for (int i = 0; i < 300; ++i) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) log("CASE " + tag + " exit=" + std::to_string(WEXITSTATUS(status)));
            else if (WIFSIGNALED(status)) log("CASE " + tag + " signal=" + std::to_string(WTERMSIG(status)));
            return;
        }
        if (result < 0 && errno != EINTR) { Error(log, tag + " waitpid", errno); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    log("CASE " + tag + " TIMEOUT owned child killed");
}
bool WriteAll(int fd, const std::vector<unsigned char>& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += count;
    }
    return true;
}
std::string Copy(const Log& log, const std::string& source, const std::string& dest)
{
    const auto bytes = Bytes(source);
    if (bytes.empty()) { log("copy source empty " + source); return {}; }
    const int fd = open(dest.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0700);
    if (fd < 0) { Error(log, "copy open " + dest, errno); return {}; }
    const bool good = WriteAll(fd, bytes) && fchmod(fd, 0700) == 0;
    const int error = errno;
    fsync(fd); close(fd);
    if (!good) { Error(log, "copy write/chmod", error); return {}; }
    log("owned copy bytes=" + std::to_string(bytes.size()) + " path=" + dest);
    return dest;
}
struct Image {
    std::vector<unsigned char> bytes;
    Elf64_Ehdr header {};
    std::vector<Elf64_Phdr> ph;
    bool valid = false;
    explicit Image(const std::string& path) : bytes(Bytes(path))
    {
        if (bytes.size() < sizeof(header)) return;
        memcpy(&header, bytes.data(), sizeof(header));
        if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
            header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_AARCH64 ||
            header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 128 ||
            !Range(header.e_phoff, header.e_phnum * sizeof(Elf64_Phdr), bytes.size())) return;
        ph.resize(header.e_phnum);
        memcpy(ph.data(), bytes.data() + header.e_phoff, ph.size() * sizeof(Elf64_Phdr));
        valid = true;
    }
    uint64_t Symbol(const char* name) const
    {
        if (!valid || header.e_shentsize != sizeof(Elf64_Shdr) ||
            !Range(header.e_shoff, header.e_shnum * sizeof(Elf64_Shdr), bytes.size())) return 0;
        for (size_t i = 0; i < header.e_shnum; ++i) {
            Elf64_Shdr sh {}; memcpy(&sh, bytes.data() + header.e_shoff + i * sizeof(sh), sizeof(sh));
            if ((sh.sh_type != SHT_DYNSYM && sh.sh_type != SHT_SYMTAB) || sh.sh_entsize != sizeof(Elf64_Sym) ||
                sh.sh_link >= header.e_shnum || !Range(sh.sh_offset, sh.sh_size, bytes.size())) continue;
            Elf64_Shdr strings {}; memcpy(&strings, bytes.data() + header.e_shoff + sh.sh_link * sizeof(strings), sizeof(strings));
            if (!Range(strings.sh_offset, strings.sh_size, bytes.size())) continue;
            for (size_t j = 0; j < sh.sh_size / sizeof(Elf64_Sym); ++j) {
                Elf64_Sym sym {}; memcpy(&sym, bytes.data() + sh.sh_offset + j * sizeof(sym), sizeof(sym));
                if (sym.st_name >= strings.sh_size) continue;
                const char* value = reinterpret_cast<const char*>(bytes.data() + strings.sh_offset + sym.st_name);
                if (memchr(value, 0, strings.sh_size - sym.st_name) && strcmp(value, name) == 0) return sym.st_value;
            }
        }
        return 0;
    }
};
void Describe(const Log& log, const std::string& path)
{
    struct stat st {}; const int result = stat(path.c_str(), &st); const int error = errno;
    log("FILE " + path + " stat=" + std::to_string(result) + " errno=" + std::to_string(result ? error : 0) +
        " mode=" + std::to_string(st.st_mode & 07777) + " size=" + std::to_string(st.st_size) + " X_OK=" + std::to_string(access(path.c_str(), X_OK)));
    struct statvfs fs {};
    if (statvfs(path.c_str(), &fs) == 0) log("filesystem flags=" + std::to_string(fs.f_flag));
    if (result != 0 || !S_ISREG(st.st_mode)) return;
    Image image(path);
    if (!image.valid) return;
    log("ELF type=" + std::to_string(image.header.e_type) + " entry=" + std::to_string(image.header.e_entry) + " phnum=" + std::to_string(image.header.e_phnum));
    bool interpreter = false;
    for (const auto& ph : image.ph) {
        if (ph.p_type == PT_INTERP && Range(ph.p_offset, ph.p_filesz, image.bytes.size()) && ph.p_filesz > 0 &&
            memchr(image.bytes.data() + ph.p_offset, 0, ph.p_filesz)) {
            interpreter = true;
            const std::string name(reinterpret_cast<const char*>(image.bytes.data() + ph.p_offset));
            log("PT_INTERP=" + name + " access=" + std::to_string(access(name.c_str(), F_OK)));
        }
    }
    if (!interpreter) log("PT_INTERP=none");
}
std::string Cache(const std::string& root) { return root + "/../../cache/hish-route-audit"; }
std::string Path(const Log& log, const std::string& root, const std::string& libs, const std::string& where, const std::string& name)
{
    const auto source = libs + "/" + name;
    if (where == "bundle") return source;
    return Copy(log, source, (where == "cache" ? Cache(root) : root + "/home") + "/audit-" + name);
}
int MemoryFile(const Log& log, const std::string& source)
{
    const int fd = memfd_create("hish-owned-audit", 0);
    if (fd < 0) { Error(log, "memfd_create", errno); return -1; }
    const auto bytes = Bytes(source);
    if (bytes.empty()) { log("memfd source empty"); close(fd); return -1; }
    if (!WriteAll(fd, bytes)) { Error(log, "memfd write", errno); close(fd); return -1; }
    // Changing a memfd's mode is independently restricted on some firmware.
    // It is not a prerequisite for testing mmap/dlopen or the initial mode.
    if (fchmod(fd, 0700) != 0) Error(log, "memfd fchmod (continuing)", errno);
    struct stat st {};
    if (fstat(fd, &st) != 0) Error(log, "memfd fstat", errno);
    else log("memfd mode=" + std::to_string(st.st_mode & 07777) + " type=" + std::to_string(st.st_mode & S_IFMT));
    char link[256] = {};
    const std::string proc = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t length = readlink(proc.c_str(), link, sizeof(link) - 1);
    if (length >= 0) log("memfd backing=" + std::string(link, length));
    else Error(log, "memfd readlink", errno);
    log("memfd prepared bytes=" + std::to_string(bytes.size()) + " descriptor flags=" + std::to_string(fcntl(fd, F_GETFD)));
    return fd;
}
void Execs(const Log& log, int reportFd, const std::string& root, const std::string& libs, const std::string& where)
{
    for (const std::string name : {"libhish_probe_exec.so", "libhish_probe_bare.so", "libhish_probe_static.so"}) {
        const std::string path = where == "memfd" ? libs + "/" + name : Path(log, root, libs, where, name);
        Describe(log, path);
        for (const std::string method : {"execve", "fexecve-keep", "fexecve-cloexec", "execveat-keep", "execveat-cloexec", "procfd", "inline-execve", "inline-execveat"}) {
            Child(log, reportFd, name + "/" + where + "/" + method, [&]() {
                const int fd = where == "memfd" ? MemoryFile(log, path) : open(path.c_str(), O_RDONLY);
                if (fd < 0) { Error(log, "ELF open", errno); return 70; }
                const std::string proxy = "/proc/self/fd/" + std::to_string(fd);
                if (fcntl(fd, F_SETFD, method.find("cloexec") == std::string::npos ? 0 : FD_CLOEXEC) != 0) Error(log, "set ELF descriptor flags", errno);
                log("ELF descriptor flags=" + std::to_string(fcntl(fd, F_GETFD)));
                char arg0[] = "hish-owned-test"; char* args[] = {arg0, nullptr};
                const std::string target = where == "memfd" || method == "procfd" ? proxy : path;
                if (method == "inline-execve" || method == "inline-execveat") {
                    log("inline SVC exec BEGIN");
                    const long result = method == "inline-execve" ? Raw(SYS_execve, reinterpret_cast<long>(target.c_str()), reinterpret_cast<long>(args), reinterpret_cast<long>(environ)) :
                        Raw(SYS_execveat, fd, reinterpret_cast<long>(""), reinterpret_cast<long>(args), reinterpret_cast<long>(environ), AT_EMPTY_PATH);
                    log("inline SVC exec raw result=" + std::to_string(result)); close(fd); return 71;
                }
                else if (method == "execve" || method == "procfd") execve(target.c_str(), args, environ);
                else if (method.rfind("fexecve", 0) == 0) fexecve(fd, args, environ);
                else syscall(SYS_execveat, fd, "", args, environ, AT_EMPTY_PATH);
                Error(log, "EXEC result", errno); close(fd); return 71;
            });
        }
    }
}
void Loads(const Log& log, int reportFd, const std::string& root, const std::string& libs, const std::string& where)
{
    for (const std::string name : {"libhish_probe_payload.so", "libhish_probe_bare.so"}) {
        const auto path = where == "memfd" ? libs + "/" + name : Path(log, root, libs, where, name);
        for (const std::string mode : {"NOW-path", "LAZY-path", "NOW-procfd", "GLOBAL-path"}) {
            Child(log, reportFd, name + "/" + where + "/" + mode, [&]() {
                int fd = -1; std::string target = path;
                if (where == "memfd" || mode == "NOW-procfd") {
                    fd = where == "memfd" ? MemoryFile(log, path) : open(path.c_str(), O_RDONLY | O_CLOEXEC);
                    if (fd < 0) { Error(log, "module fd", errno); return 72; }
                    target = "/proc/self/fd/" + std::to_string(fd);
                }
                dlerror(); errno = 0;
                void* handle = dlopen(target.c_str(), (mode == "LAZY-path" ? RTLD_LAZY : RTLD_NOW) | (mode == "GLOBAL-path" ? RTLD_GLOBAL : RTLD_LOCAL));
                const int error = errno;
                if (!handle) {
                    const char* message = dlerror();
                    log("DLOPEN FAIL errno=" + std::to_string(error) + " dlerror=" + (message ? message : "none"));
                    if (fd >= 0) close(fd);
                    return 73;
                }
                auto fn = reinterpret_cast<int (*)()>(dlsym(handle, "HiSHAuditConst42"));
                if (!fn) { log("DLSYM FAIL"); dlclose(handle); if (fd >= 0) close(fd); return 74; }
                const int value = fn(); log("DLOPEN_CALL result=" + std::to_string(value));
                dlclose(handle); if (fd >= 0) close(fd); return value;
            });
        }
    }
}
// Private musl extensions are discovered at runtime, never assumed to be public SDK APIs.
// Layouts below match OpenHarmony third_party_musl/porting/linux/user/include.
struct AuditDlExt { int flag; int relro_fd; void* reserved_addr; size_t reserved_size; };
void CallPage(const Log& log, void* memory);
void LoaderApis(const Log& log, int reportFd, const std::string& root, const std::string& libs)
{
    for (const char* name : {"dlopen_ext", "dlopen_ns", "dlopen_ns_ext", "dlns_init", "dlns_create", "dlns_create2", "dlns_get", "fdlopen", "android_dlopen_ext"}) {
        log(std::string("LOADER_SYMBOL ") + name + " present=" + (dlsym(RTLD_DEFAULT, name) ? "1" : "0"));
    }
    for (const std::string origin : {"bundle", "home", "cache"}) {
        const std::string path = Path(log, root, libs, origin, "libhish_probe_payload.so");
        for (const std::string method : {"ext-null", "ext-reserved", "namespace-current", "namespace-owned", "namespace-owned-basic", "namespace-ext-current"}) {
            Child(log, reportFd, "loader-api/" + origin + "/" + method, [&]() {
                using Ext = void* (*)(const char*, int, const AuditDlExt*);
                using NsOpen = void* (*)(Dl_namespace*, const char*, int);
                void* handle = nullptr;
                void* reserve = MAP_FAILED;
                dlerror(); errno = 0;
                if (method.rfind("ext-", 0) == 0) {
                    auto openExt = reinterpret_cast<Ext>(dlsym(RTLD_DEFAULT, "dlopen_ext"));
                    if (!openExt) { log("EXT API unavailable"); return 100; }
                    AuditDlExt ext {};
                    if (method == "ext-reserved") {
                        ext.flag = 0x8; ext.reserved_size = 1024 * 1024;
                        reserve = mmap(nullptr, ext.reserved_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                        if (reserve == MAP_FAILED) { Error(log, "ext reserve", errno); return 101; }
                        ext.reserved_addr = reserve;
                    }
                    handle = openExt(path.c_str(), RTLD_NOW, method == "ext-null" ? nullptr : &ext);
                } else {
                    auto nsOpen = reinterpret_cast<NsOpen>(dlsym(RTLD_DEFAULT, "dlopen_ns"));
                    if (!nsOpen) { log("namespace API unavailable"); return 100; }
                    Dl_namespace ns {};
                    if (method == "namespace-owned" || method == "namespace-owned-basic") {
                        auto init = reinterpret_cast<void (*)(Dl_namespace*, const char*)>(dlsym(RTLD_DEFAULT, "dlns_init"));
                        auto create = reinterpret_cast<int (*)(Dl_namespace*, const char*, int)>(dlsym(RTLD_DEFAULT, "dlns_create2"));
                        if (!init || !create) { log("namespace create unavailable"); return 100; }
                        init(&ns, "hish_owned_audit");
                        const std::string search = path.substr(0, path.find_last_of('/'));
                        auto basic = reinterpret_cast<int (*)(Dl_namespace*, const char*)>(dlsym(RTLD_DEFAULT, "dlns_create"));
                        const int result = method == "namespace-owned-basic" && basic ? basic(&ns, search.c_str()) : create(&ns, search.c_str(), 2);
                        log("owned namespace create result=" + std::to_string(result));
                        if (result != 0) return 102;
                    } else {
                        auto get = reinterpret_cast<int (*)(const char*, Dl_namespace*)>(dlsym(RTLD_DEFAULT, "dlns_get"));
                        if (!get) { log("namespace get unavailable"); return 100; }
                        const int result = get(nullptr, &ns);
                        log("current namespace get result=" + std::to_string(result) + " name=" + ns.name);
                        if (result != 0) return 102;
                    }
                    if (method == "namespace-ext-current") {
                        auto nsExt = reinterpret_cast<void* (*)(Dl_namespace*, const char*, int, const AuditDlExt*)>(dlsym(RTLD_DEFAULT, "dlopen_ns_ext"));
                        if (!nsExt) return 100;
                        handle = nsExt(&ns, path.c_str(), RTLD_NOW, nullptr);
                    } else handle = nsOpen(&ns, path.c_str(), RTLD_NOW);
                }
                if (!handle) { const int error = errno; const char* text = dlerror(); log("EXT_LOAD FAIL errno=" + std::to_string(error) + " text=" + (text ? text : "none")); return 103; }
                auto fn = reinterpret_cast<int (*)()>(dlsym(handle, "HiSHAuditConst42"));
                if (!fn) { log("EXT_LOAD symbol missing"); dlclose(handle); return 104; }
                const int value = fn(); log("EXT_LOAD_CALL result=" + std::to_string(value));
                dlclose(handle); // reserved region belongs solely to this short-lived child
                return value;
            });
        }
    }
}
void SelfCode(const Log& log, int reportFd, const std::string& libs)
{
    const std::string path = libs + "/libhish_probe_bare.so";
    const Image image(path); const auto symbol = image.Symbol("HiSHAuditConst42");
    const size_t page = sysconf(_SC_PAGESIZE); size_t offset = 0; bool found = false;
    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) { offset = ph.p_offset + symbol - ph.p_vaddr; found = true; break; }
    if (!found) return;
    for (const std::string method : {"RX-RW-RX", "proc-self-mem", "process-vm-self", "replace-RX-with-anon"}) {
        Child(log, reportFd, "self-code/" + method, [&]() {
            const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) return 120;
            void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, offset & ~(page - 1)); close(fd);
            if (memory == MAP_FAILED) { Error(log, "self owned map", errno); return 120; }
            auto* entry = static_cast<char*>(memory) + offset % page;
            const uint32_t patch[] = {0x52800560, 0xd65f03c0}; // modify only this child's private test code: return 43
            if (method == "RX-RW-RX") {
                if (mprotect(memory, page, PROT_READ | PROT_WRITE) != 0) { Error(log, "self RX-RW", errno); return 121; }
                memcpy(entry, patch, sizeof(patch));
                if (mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) { Error(log, "self patched RW-RX", errno); return 121; }
            } else if (method == "proc-self-mem") {
                const int self = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
                if (self < 0) { Error(log, "own proc mem open", errno); return 122; }
                const auto count = pwrite(self, patch, sizeof(patch), reinterpret_cast<uintptr_t>(entry)); const int error = errno; close(self);
                log("own proc mem write=" + std::to_string(count) + " errno=" + std::to_string(count < 0 ? error : 0));
                if (count != sizeof(patch)) return 122;
            } else if (method == "process-vm-self") {
                iovec source {const_cast<uint32_t*>(patch), sizeof(patch)}, dest {entry, sizeof(patch)};
                log("process_vm_writev BEGIN target=self private test mapping");
                const auto count = process_vm_writev(getpid(), &source, 1, &dest, 1, 0);
                log("own process vm write=" + std::to_string(count) + " errno=" + std::to_string(count < 0 ? errno : 0));
                if (count != sizeof(patch)) return 123;
            } else {
                void* replaced = mmap(memory, page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (replaced == MAP_FAILED) { Error(log, "self replace own reserved page", errno); return 124; }
                memcpy(entry, patch, sizeof(patch));
                if (mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) { Error(log, "replacement anon RX", errno); return 124; }
            }
            __builtin___clear_cache(entry, entry + sizeof(patch));
            log("SELF_MODIFIED_CALL BEGIN expected=43");
            const int value = reinterpret_cast<int (*)()>(entry)(); log("SELF_MODIFIED_CALL result=" + std::to_string(value)); return value;
        });
    }
}
void MappingVariants(const Log& log, int reportFd, const std::string& libs)
{
    const std::string path = libs + "/libhish_probe_bare.so";
    const Image image(path); const auto symbol = image.Symbol("HiSHAuditConst42");
    const size_t page = sysconf(_SC_PAGESIZE); size_t offset = 0; bool found = false;
    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) { offset = ph.p_offset + symbol - ph.p_vaddr; found = true; break; }
    if (!found) { log("mapping variant symbol missing"); return; }
    for (const std::string method : {"bundle-X", "bundle-RX-R-RX", "bundle-RX-mremap", "bundle-COW-dual", "memfd-dual", "anon-X", "anon-shared-RX", "anon-pkey-RX", "anon-read-implies-exec", "ashmem-exec-mask"}) {
        Child(log, reportFd, "mapping-variant/" + method, [&]() {
            const uint32_t code[] = {0x52800540, 0xd65f03c0};
            if (method.rfind("bundle-", 0) == 0) {
                const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0) { Error(log, "variant open", errno); return 105; }
                void* memory = mmap(nullptr, page, method == "bundle-X" ? PROT_EXEC : PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, offset & ~(page - 1));
                if (memory == MAP_FAILED) { Error(log, "variant map", errno); close(fd); return 106; }
                if (method == "bundle-RX-R-RX") {
                    if (mprotect(memory, page, PROT_READ) != 0 || mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) { Error(log, "variant protection", errno); close(fd); return 107; }
                } else if (method == "bundle-RX-mremap") {
                    void* target = mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    if (target == MAP_FAILED) { Error(log, "variant reserve", errno); close(fd); return 107; }
                    void* moved = mremap(memory, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, target);
                    if (moved == MAP_FAILED) { Error(log, "variant mremap", errno); close(fd); return 107; }
                    memory = moved; log("owned RX mapping moved; original file backing retained");
                } else if (method == "bundle-COW-dual") {
                    void* writable = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, offset & ~(page - 1));
                    if (writable == MAP_FAILED) { Error(log, "COW alias RW", errno); close(fd); return 107; }
                    const uint32_t changed = 0x52800560; // own private COW alias: mov w0,#43
                    memcpy(static_cast<char*>(writable) + offset % page, &changed, sizeof(changed));
                    log("COW writable alias patched; executable view must still return original 42");
                }
                close(fd);
                log("VARIANT_CALL BEGIN"); const int value = reinterpret_cast<int (*)()>(static_cast<char*>(memory) + offset % page)();
                log("VARIANT_CALL result=" + std::to_string(value)); return value;
            }
            if (method == "memfd-dual" || method == "ashmem-exec-mask") {
                const int fd = method == "memfd-dual" ? memfd_create("hish-owned-dual", 0) : open("/dev/ashmem", O_RDWR | O_CLOEXEC);
                if (fd < 0) { Error(log, "dual open", errno); return 108; }
                const int prep = method == "memfd-dual" ? ftruncate(fd, page) : ioctl(fd, ASHMEM_SET_SIZE, page);
                if (prep != 0) { Error(log, "dual size", errno); close(fd); return 108; }
                if (method == "ashmem-exec-mask") { const int result = ioctl(fd, ASHMEM_SET_PROT_MASK, PROT_READ | PROT_WRITE | PROT_EXEC); log("ashmem exec mask result=" + std::to_string(result) + " errno=" + std::to_string(result ? errno : 0)); }
                void* writable = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (writable == MAP_FAILED) { Error(log, "dual RW", errno); close(fd); return 108; }
                memcpy(writable, code, sizeof(code));
                void* executable = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
                const int error = errno; close(fd);
                if (executable == MAP_FAILED) { Error(log, "dual RX", error); return 109; }
                __builtin___clear_cache(static_cast<char*>(executable), static_cast<char*>(executable) + sizeof(code));
                CallPage(log, executable); return 42;
            }
            if (method == "anon-read-implies-exec") {
                const int old = personality(0xffffffffUL); log("personality query=" + std::to_string(old));
                if (old < 0) { Error(log, "personality query", errno); return 110; }
                if (personality(static_cast<unsigned long>(old) | READ_IMPLIES_EXEC) < 0) { Error(log, "personality READ_IMPLIES_EXEC", errno); return 110; }
            }
            const int flags = (method == "anon-shared-RX" ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS;
            void* memory = mmap(nullptr, page, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (memory == MAP_FAILED) { Error(log, "anon variant RW", errno); return 111; }
            memcpy(memory, code, sizeof(code));
            long result;
            if (method == "anon-pkey-RX") {
#ifdef SYS_pkey_mprotect
                log("pkey_mprotect BEGIN key=0"); result = Raw(SYS_pkey_mprotect, reinterpret_cast<long>(memory), page, PROT_READ | PROT_EXEC, 0);
#else
                log("pkey syscall unavailable in SDK"); return 112;
#endif
            } else result = mprotect(memory, page, method == "anon-X" ? PROT_EXEC : method == "anon-read-implies-exec" ? PROT_READ : PROT_READ | PROT_EXEC);
            log("anon variant protection result=" + std::to_string(result) + " errno=" + std::to_string(result == 0 ? 0 : errno));
            if (result != 0) return 112;
            __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + sizeof(code));
            CallPage(log, memory); return 42;
        });
    }
}
void StorageOrigins(const Log& log, int reportFd, const std::string& root, const std::string& libs)
{
    // EL1 paths come from this phone module's createAreaModeContext getters (see driver log).
    const std::vector<std::string> directories {root + "/home", Cache(root), root + "/../../temp", "/data/storage/el1/base/haps/phone/files", "/data/storage/el1/base/haps/phone/cache", "/data/storage/el1/base/haps/phone/temp"};
    for (size_t i = 0; i < directories.size(); ++i) {
        const auto file = directories[i] + "/hish-owned-origin-probe.so";
        const auto copied = Copy(log, libs + "/libhish_probe_bare.so", file);
        for (const std::string method : {"RX", "exec", "hardlink-bundle", "symlink-bundle"}) {
            Child(log, reportFd, "storage/" + std::to_string(i) + "/" + method, [&]() {
                const std::string alias = directories[i] + "/hish-owned-origin-link.so";
                if (method == "hardlink-bundle" || method == "symlink-bundle") {
                    // Never change an existing path; unlink only an alias created successfully here.
                    const auto source = libs + "/libhish_probe_bare.so";
                    const int result = method == "hardlink-bundle" ? link(source.c_str(), alias.c_str()) : symlink(source.c_str(), alias.c_str());
                    if (result != 0) { Error(log, "owned link create", errno); return 113; }
                    const int fd = open(alias.c_str(), O_RDONLY | O_CLOEXEC); const int error = errno; unlink(alias.c_str());
                    if (fd < 0) { Error(log, "owned alias open", error); return 113; }
                    const Image image(source); const auto symbol = image.Symbol("HiSHAuditConst42");
                    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) {
                        const size_t page = sysconf(_SC_PAGESIZE), offset = ph.p_offset + symbol - ph.p_vaddr;
                        void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, offset & ~(page - 1)); close(fd);
                        if (memory == MAP_FAILED) { Error(log, "alias RX", errno); return 114; }
                        const int value = reinterpret_cast<int (*)()>(static_cast<char*>(memory) + offset % page)(); log("ALIAS_CALL original bundle result=" + std::to_string(value)); return value;
                    }
                    close(fd); return 114;
                }
                if (copied.empty()) { log("storage unavailable: copy failed before this execution attempt"); return 115; }
                Describe(log, file);
                if (method == "exec") { char arg[] = "hish-origin"; char* argv[] = {arg, nullptr}; execve(file.c_str(), argv, environ); Error(log, "origin exec", errno); return 116; }
                const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0) { Error(log, "origin open", errno); return 116; }
                const Image image(file); const auto symbol = image.Symbol("HiSHAuditConst42");
                for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) {
                    const size_t page = sysconf(_SC_PAGESIZE), offset = ph.p_offset + symbol - ph.p_vaddr;
                    void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, offset & ~(page - 1)); const int error = errno; close(fd);
                    if (memory == MAP_FAILED) { Error(log, "origin RX", error); return 117; }
                    const int value = reinterpret_cast<int (*)()>(static_cast<char*>(memory) + offset % page)(); log("ORIGIN_CALL result=" + std::to_string(value)); return value;
                }
                close(fd); return 117;
            });
        }
        if (!copied.empty() && unlink(file.c_str()) != 0) Error(log, "origin cleanup", errno);
    }
}
void SystemLoader(const Log& log, int reportFd, const std::string& root, const std::string& libs)
{
    for (const std::string origin : {"bundle", "home"}) {
        const auto file = Path(log, root, libs, origin, "libhish_probe_dynamic_runtime.so");
        for (const std::string loaderPath : {"/lib/ld-musl-aarch64.so.1", "/system/lib/ld-musl-aarch64.so.1", "/system/lib64/ld-musl-aarch64.so.1"}) {
          for (const std::string method : {"execve-loader", "posix-spawn-loader"}) {
            Child(log, reportFd, "system-loader/" + origin + "/" + loaderPath + "/" + method, [&]() {
                setenv("HOME", (root + "/home").c_str(), 1);
                char* loader = const_cast<char*>(loaderPath.c_str());
                char* args[] = {loader, const_cast<char*>(file.c_str()), nullptr};
                if (method == "execve-loader") { execve(loader, args, environ); Error(log, "system loader exec", errno); return 118; }
                pid_t pid = -1; const int result = posix_spawn(&pid, loader, nullptr, nullptr, args, environ);
                log("system loader spawn result=" + std::to_string(result));
                if (result != 0) return 118;
                int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                if (WIFEXITED(status)) { log("system loader child exit=" + std::to_string(WEXITSTATUS(status))); return WEXITSTATUS(status); }
                log("system loader child signal=" + std::to_string(WTERMSIG(status))); return 119;
            });
          }
        }
    }
}
void Maps(const Log& log, int reportFd, const std::string& root, const std::string& libs, const std::string& where)
{
    const std::string name = "libhish_probe_bare.so";
    const std::string path = where == "memfd" || where == "anon" ? libs + "/" + name : Path(log, root, libs, where, name);
    const Image image(path);
    const uint64_t symbol = image.Symbol("HiSHAuditConst42");
    const size_t page = sysconf(_SC_PAGESIZE);
    Elf64_Phdr segment {}; bool found = false;
    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) { segment = ph; found = true; break; }
    if (!found || !symbol) { log("MAP missing owned ELF symbol"); return; }
    const size_t offset = segment.p_offset + symbol - segment.p_vaddr;
    const size_t filePage = offset & ~(page - 1), within = offset - filePage;
    for (const std::string mode : {"private-RX", "shared-RX", "R-to-RX", "RW-to-RX", "RW-patch-to-RX", "private-RWX"}) {
        Child(log, reportFd, where + "/" + mode, [&]() {
            int fd = -1;
            if (where != "anon") {
                fd = where == "memfd" ? MemoryFile(log, path) : open(path.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0) { Error(log, "map open", errno); return 75; }
            }
            const bool transition = mode == "R-to-RX" || mode == "RW-to-RX" || mode == "RW-patch-to-RX";
            int prot = mode == "R-to-RX" ? PROT_READ : (mode == "RW-to-RX" || mode == "RW-patch-to-RX") ? PROT_READ | PROT_WRITE :
                mode == "private-RWX" ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC;
            const int flags = (mode == "shared-RX" ? MAP_SHARED : MAP_PRIVATE) | (where == "anon" ? MAP_ANONYMOUS : 0);
            void* memory = mmap(nullptr, page, prot, flags, fd, where == "anon" ? 0 : filePage);
            if (fd >= 0) close(fd);
            if (memory == MAP_FAILED) { Error(log, "MMAP", errno); return 76; }
            log("MMAP PASS");
            if (where == "anon" && (prot & PROT_WRITE)) {
                const uint32_t instructions[] = {0x52800540, 0xd65f03c0};
                memcpy(memory, instructions, sizeof(instructions));
                __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + sizeof(instructions));
            }
            if (mode == "RW-patch-to-RX") {
                const uint32_t instructions[] = {0x52800560, 0xd65f03c0}; // owned test: return 43
                const size_t at = where == "anon" ? 0 : within;
                memcpy(static_cast<char*>(memory) + at, instructions, sizeof(instructions));
                __builtin___clear_cache(static_cast<char*>(memory) + at, static_cast<char*>(memory) + at + sizeof(instructions));
                log("owned private code patch PASS; expected return=43 if execution allowed");
            }
            if (transition && mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) {
                Error(log, "MPROTECT RX", errno); munmap(memory, page); return 77;
            }
            if (where == "anon" && !(prot & PROT_WRITE)) { log("anonymous no code written; mapping only"); munmap(memory, page); return 0; }
            log("MAPPED_CALL BEGIN");
            const int value = reinterpret_cast<int (*)()>(static_cast<char*>(memory) + (where == "anon" ? 0 : within))();
            log("MAPPED_CALL result=" + std::to_string(value)); munmap(memory, page); return value;
        });
    }
}
void CallPage(const Log& log, void* memory)
{
    log("MEMORY_CALL BEGIN");
    const int value = reinterpret_cast<int (*)()>(memory)();
    log("MEMORY_CALL result=" + std::to_string(value));
}
void OtherMemory(const Log& log, int reportFd)
{
    const size_t page = sysconf(_SC_PAGESIZE);
    const uint32_t code[] = {0x52800540, 0xd65f03c0};
    for (const unsigned flags : {2U, 16U, 18U}) {
        Child(log, reportFd, "memfd-flags/" + std::to_string(flags), [&]() {
            // 16 is Linux UAPI MFD_EXEC, not assumed supported by OHOS.
            const int fd = memfd_create("hish-owned-flags", flags);
            if (fd < 0) { Error(log, "memfd flags create", errno); return 80; }
            const ssize_t count = write(fd, code, sizeof(code));
            log("memfd flags write=" + std::to_string(count));
            if (count != sizeof(code)) { close(fd); return 80; }
            if (flags & 2) {
                if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) Error(log, "memfd seals", errno);
                else log("memfd seals PASS");
            }
            void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
            close(fd);
            if (memory == MAP_FAILED) { Error(log, "sealed/exec memfd RX", errno); return 81; }
            CallPage(log, memory); munmap(memory, page); return 42;
        });
    }
    Child(log, reportFd, "POSIX-shm", [&]() {
        const std::string name = "/hish-owned-" + std::to_string(getpid());
        const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0700);
        if (fd < 0) { Error(log, "shm_open", errno); return 82; }
        shm_unlink(name.c_str()); // immediately remove only this test's name
        if (ftruncate(fd, page) != 0 || write(fd, code, sizeof(code)) != sizeof(code)) { Error(log, "shm prepare", errno); close(fd); return 82; }
        void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0); close(fd);
        if (memory == MAP_FAILED) { Error(log, "POSIX shm RX", errno); return 83; }
        CallPage(log, memory); munmap(memory, page); return 42;
    });
    Child(log, reportFd, "SysV-shm", [&]() {
        log("shmget BEGIN");
        const int id = shmget(IPC_PRIVATE, page, IPC_CREAT | 0700);
        if (id < 0) { Error(log, "shmget", errno); return 84; }
        log("shmget returned owned id=" + std::to_string(id));
        log("shmat RW BEGIN");
        void* writable = shmat(id, nullptr, 0);
        if (writable == reinterpret_cast<void*>(-1)) { Error(log, "shmat RW", errno); shmctl(id, IPC_RMID, nullptr); return 84; }
        memcpy(writable, code, sizeof(code));
        __builtin___clear_cache(static_cast<char*>(writable), static_cast<char*>(writable) + sizeof(code));
        log("shmat RX BEGIN");
        void* memory = shmat(id, nullptr, SHM_RDONLY | SHM_EXEC);
        const int error = errno;
        shmctl(id, IPC_RMID, nullptr); // ensure deletion even if executing faults
        shmdt(writable);
        if (memory == reinterpret_cast<void*>(-1)) { Error(log, "shmat RX", error); return 85; }
        CallPage(log, memory); shmdt(memory); return 42;
    });
    Child(log, reportFd, "ashmem", [&]() {
        const int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
        if (fd < 0) { Error(log, "ashmem open", errno); return 86; }
        if (ioctl(fd, ASHMEM_SET_SIZE, page) != 0) { Error(log, "ashmem size", errno); close(fd); return 86; }
        void* writable = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (writable == MAP_FAILED) { Error(log, "ashmem RW", errno); close(fd); return 86; }
        memcpy(writable, code, sizeof(code));
        __builtin___clear_cache(static_cast<char*>(writable), static_cast<char*>(writable) + sizeof(code));
        void* memory = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        const int error = errno; close(fd); munmap(writable, page);
        if (memory == MAP_FAILED) { Error(log, "ashmem RX", error); return 87; }
        CallPage(log, memory); munmap(memory, page); return 42;
    });
    Child(log, reportFd, "dev-zero", [&]() {
        const int fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
        if (fd < 0) { Error(log, "zero open", errno); return 88; }
        void* memory = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); close(fd);
        if (memory == MAP_FAILED) { Error(log, "zero RW", errno); return 88; }
        memcpy(memory, code, sizeof(code));
        __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + sizeof(code));
        if (mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) { Error(log, "zero RX", errno); munmap(memory, page); return 89; }
        CallPage(log, memory); munmap(memory, page); return 42;
    });
}
void InlineCalls(const Log& log, int reportFd)
{
    Child(log, reportFd, "inline-anonymous-memory", [&]() {
        const size_t page = sysconf(_SC_PAGESIZE);
        log("inline mmap RW BEGIN");
        const long address = Raw(SYS_mmap, 0, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (address < 0 && address >= -4095) { log("inline mmap raw=" + std::to_string(address)); return 92; }
        log("inline mmap RW returned");
        void* memory = reinterpret_cast<void*>(address);
        const uint32_t code[] = {0x52800540, 0xd65f03c0}; memcpy(memory, code, sizeof(code));
        __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + sizeof(code));
        log("inline mprotect RX BEGIN");
        const long result = Raw(SYS_mprotect, address, page, PROT_READ | PROT_EXEC);
        log("inline mprotect raw=" + std::to_string(result));
        if (result == 0) CallPage(log, memory);
        munmap(memory, page); return result == 0 ? 42 : 93;
    });
    Child(log, reportFd, "inline-memfd", [&]() {
        log("inline memfd_create BEGIN");
        const long fd = Raw(SYS_memfd_create, reinterpret_cast<long>("hish-owned-inline"), 0);
        log("inline memfd raw=" + std::to_string(fd));
        if (fd < 0) return 94;
        const uint32_t code[] = {0x52800540, 0xd65f03c0};
        if (write(fd, code, sizeof(code)) != sizeof(code)) { Error(log, "inline memfd write", errno); close(fd); return 94; }
        void* memory = mmap(nullptr, sysconf(_SC_PAGESIZE), PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0); close(fd);
        if (memory == MAP_FAILED) { Error(log, "inline memfd RX", errno); return 95; }
        CallPage(log, memory); munmap(memory, sysconf(_SC_PAGESIZE)); return 42;
    });
    Child(log, reportFd, "inline-pty-open", [&]() {
        log("inline openat /dev/ptmx BEGIN");
        const long fd = Raw(SYS_openat, AT_FDCWD, reinterpret_cast<long>("/dev/ptmx"), O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
        log("inline openat raw=" + std::to_string(fd));
        if (fd >= 0) close(fd);
        return fd >= 0 ? 42 : 96;
    });
}
void Archive(const Log& log, int reportFd, const std::string& libs)
{
    const Image image(libs + "/libhish_probe_bare.so");
    const uint64_t symbol = image.Symbol("HiSHAuditConst42");
    uint64_t inElf = UINT64_MAX;
    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD && symbol >= ph.p_vaddr && symbol - ph.p_vaddr < ph.p_filesz) inElf = ph.p_offset + symbol - ph.p_vaddr;
    for (const char* path : {"/data/storage/el1/bundle/phone.hap", "/data/storage/el1/bundle/entry.hap"}) {
        Describe(log, path);
        const auto bytes = Bytes(path);
        auto u16 = [&](size_t at) { uint16_t v = 0; if (Range(at, sizeof(v), bytes.size())) memcpy(&v, bytes.data() + at, sizeof(v)); return v; };
        auto u32 = [&](size_t at) { uint32_t v = 0; if (Range(at, sizeof(v), bytes.size())) memcpy(&v, bytes.data() + at, sizeof(v)); return v; };
        bool found = false;
        for (size_t at = 0; Range(at, 46, bytes.size()); ++at) {
            if (u32(at) != 0x02014b50) continue;
            const size_t length = u16(at + 28);
            if (!Range(at + 46, length, bytes.size())) continue;
            const std::string name(reinterpret_cast<const char*>(bytes.data() + at + 46), length);
            if (name.find("libhish_probe_bare.so") == std::string::npos) continue;
            found = true;
            const size_t local = u32(at + 42);
            if (!Range(local, 30, bytes.size()) || u32(local) != 0x04034b50) break;
            const size_t start = local + 30 + u16(local + 26) + u16(local + 28);
            const bool stored = u16(at + 10) == 0 && u32(at + 24) == image.bytes.size() &&
                Range(start, image.bytes.size(), bytes.size()) && !memcmp(bytes.data() + start, image.bytes.data(), image.bytes.size());
            log("HAP owned entry=" + name + " stored-identical=" + std::to_string(stored));
            Child(log, reportFd, "HAP-container-RX", [&]() {
                const size_t page = sysconf(_SC_PAGESIZE);
                const size_t offset = stored && inElf != UINT64_MAX ? start + inElf : start;
                const size_t filePage = offset & ~(page - 1);
                const int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0) { Error(log, "HAP open", errno); return 90; }
                void* memory = mmap(nullptr, page * 2, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, filePage); close(fd);
                if (memory == MAP_FAILED) { Error(log, "HAP RX", errno); return 91; }
                if (!stored || inElf == UINT64_MAX) { log("HAP RX mapping only; compressed entry not executed"); munmap(memory, page * 2); return 0; }
                CallPage(log, static_cast<char*>(memory) + offset - filePage);
                munmap(memory, page * 2); return 42;
            });
            break;
        }
        if (!found) log("HAP owned entry unavailable at " + std::string(path));
    }
}
#if defined(__aarch64__)
__attribute__((naked, noreturn)) void Enter(void*, void*)
{
    __asm__ volatile("mov x2, x0\nmov sp, x1\nmov x0, xzr\nbr x2");
}
#endif
struct LoadedImage {
    uintptr_t bias = 0, entry = 0, phdr = 0;
    bool valid = false;
};
LoadedImage MapDynamicImage(const Log& log, const Image& image, const std::string& path)
{
    LoadedImage loaded;
    if (!image.valid || image.header.e_type != ET_DYN) { log("interpreter experiment needs ET_DYN"); return loaded; }
    const size_t page = sysconf(_SC_PAGESIZE);
    uint64_t low = UINT64_MAX, high = 0;
    for (const auto& ph : image.ph) if (ph.p_type == PT_LOAD) {
        if (ph.p_filesz > ph.p_memsz || !Range(ph.p_offset, ph.p_filesz, image.bytes.size()) ||
            ph.p_vaddr > (1ULL << 26) || ph.p_memsz > (1ULL << 26)) return loaded;
        low = std::min(low, ph.p_vaddr & ~(page - 1));
        high = std::max(high, (ph.p_vaddr + ph.p_memsz + page - 1) & ~(page - 1));
    }
    if (high <= low || high - low > (1ULL << 26)) return loaded;
    void* area = mmap(nullptr, high - low, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) { Error(log, "interpreter reserve", errno); return loaded; }
    loaded.bias = reinterpret_cast<uintptr_t>(area) - low;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { Error(log, "interpreter open", errno); return loaded; }
    for (const auto& ph : image.ph) {
        if (ph.p_type != PT_LOAD || !ph.p_memsz) continue;
        const auto start = ph.p_vaddr & ~(page - 1);
        const size_t length = ((ph.p_vaddr + ph.p_memsz + page - 1) & ~(page - 1)) - start;
        if (start < low || length > high - start) { close(fd); return loaded; }
        const int prot = (ph.p_flags & PF_R ? PROT_READ : 0) | (ph.p_flags & PF_W ? PROT_WRITE : 0) | (ph.p_flags & PF_X ? PROT_EXEC : 0);
        void* memory;
        if (ph.p_flags & PF_X) {
            if (ph.p_flags & PF_W || ph.p_memsz != ph.p_filesz || ph.p_offset % page != ph.p_vaddr % page) { close(fd); return loaded; }
            memory = mmap(reinterpret_cast<void*>(loaded.bias + start), length, prot, MAP_PRIVATE | MAP_FIXED, fd, ph.p_offset & ~(page - 1));
        } else {
            memory = mmap(reinterpret_cast<void*>(loaded.bias + start), length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (memory != MAP_FAILED) {
                memcpy(reinterpret_cast<void*>(loaded.bias + ph.p_vaddr), image.bytes.data() + ph.p_offset, ph.p_filesz);
                if (mprotect(memory, length, prot) != 0) { Error(log, "interpreter data protection", errno); close(fd); return loaded; }
            }
        }
        if (memory == MAP_FAILED) { Error(log, "interpreter PT_LOAD " + path, errno); close(fd); return loaded; }
        log("interpreter PT_LOAD flags=" + std::to_string(ph.p_flags));
        if (image.header.e_phoff >= ph.p_offset && image.header.e_phoff - ph.p_offset < ph.p_filesz) loaded.phdr = loaded.bias + ph.p_vaddr + image.header.e_phoff - ph.p_offset;
    }
    close(fd);
    for (const auto& ph : image.ph) if (ph.p_type == PT_PHDR) loaded.phdr = loaded.bias + ph.p_vaddr;
    loaded.entry = loaded.bias + image.header.e_entry;
    loaded.valid = loaded.phdr && image.header.e_entry >= low && image.header.e_entry < high;
    return loaded;
}
__attribute__((no_stack_protector)) void LinuxFault(int signal,siginfo_t* info,void* context)
{
    char buffer[256];size_t length=0;
    auto text=[&](const char* s){while(*s&&length<sizeof(buffer))buffer[length++]=*s++;};
    auto number=[&](uint64_t value,unsigned base){char digits[32];size_t count=0;do{digits[count++]="0123456789abcdef"[value%base];value/=base;}while(value&&count<sizeof(digits));while(count&&length<sizeof(buffer))buffer[length++]=digits[--count];};
    text("LINUX_FAULT signal=");number(signal,10);
    if(signal==SIGSYS){text(" syscall=");number(info->si_syscall,10);}
#if defined(__aarch64__)
    auto* state=static_cast<ucontext_t*>(context);text(" pc=0x");number(state->uc_mcontext.pc,16);
#else
    (void)context;
#endif
    text(" address=0x");number(reinterpret_cast<uintptr_t>(info->si_addr),16);text("\n");
    Raw(SYS_write,2,reinterpret_cast<long>(buffer),length);Raw(SYS_exit_group,128+signal);for(;;){}
}
void Interpreter(const Log& log, int reportFd, const std::string& root, const std::string& libs)
{
    const std::string programPath = libs + "/libhish_probe_dynamic_runtime.so";
    const std::string interpreterPath = "/lib/ld-musl-aarch64.so.1";
    Describe(log, programPath);
    Child(log, reportFd, "manual-system-interpreter", [&]() {
        const Image program(programPath), interpreter(interpreterPath);
        const auto main = MapDynamicImage(log, program, programPath);
        if (!main.valid) return 97;
        log("main ELF mapped; system interpreter mapping BEGIN");
        const auto loader = MapDynamicImage(log, interpreter, interpreterPath);
        if (!loader.valid) return 97;
        const size_t length = 1024 * 1024;
        void* stack = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) { Error(log, "interpreter stack", errno); return 97; }
        uintptr_t top = reinterpret_cast<uintptr_t>(stack) + length;
        const std::string home = "HOME=" + root + "/home";
        const std::string search = "LD_LIBRARY_PATH=" + libs + ":/system/lib64/ndk";
        auto put = [&](const std::string& text) { top -= text.size() + 1; memcpy(reinterpret_cast<void*>(top), text.c_str(), text.size() + 1); return top; };
        const uintptr_t arg = put(programPath), env1 = put(home), env2 = put(search);
        top -= 16; memset(reinterpret_cast<void*>(top), 0x42, 16); const auto random = top;
        const uintptr_t words[] = {1, arg, 0, env1, env2, 0,
            AT_PAGESZ, static_cast<uintptr_t>(sysconf(_SC_PAGESIZE)), AT_PHDR, main.phdr,
            AT_PHENT, sizeof(Elf64_Phdr), AT_PHNUM, program.header.e_phnum,
            AT_ENTRY, main.entry, AT_BASE, loader.bias, AT_UID, getuid(), AT_EUID, geteuid(),
            AT_GID, getgid(), AT_EGID, getegid(), AT_SECURE, 0, AT_EXECFN, arg, AT_RANDOM, random,
            AT_HWCAP, getauxval(AT_HWCAP), AT_HWCAP2, getauxval(AT_HWCAP2),
            AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR), AT_CLKTCK, getauxval(AT_CLKTCK), AT_NULL, 0};
        top = (top - sizeof(words)) & ~static_cast<uintptr_t>(15);
        memcpy(reinterpret_cast<void*>(top), words, sizeof(words));
        log("ENTER system interpreter for owned dynamic C ELF");
#if defined(__aarch64__)
        Enter(reinterpret_cast<void*>(loader.entry), reinterpret_cast<void*>(top));
#else
        return 97;
#endif
    });
}
void LinuxPrograms(const Log& log, int reportFd, const std::string& root, const std::string& libs)
{
    for (const std::string kind : {"musl-static", "musl-dynamic", "glibc-static", "glibc-static-compatible", "glibc-static-fallback", "glibc-dynamic", "glibc-padded", "glibc-separate", "glibc-compatible", "glibc-no-hwcap", "alpine-busybox", "alpine-busybox-compatible"}) {
        Child(log, reportFd, "linux-ABI/" + kind, [&]() {
            const bool busy = kind.rfind("alpine-busybox",0)==0, glibc = kind.rfind("glibc-",0)==0, dynamic = kind.find("static") == std::string::npos;
            const std::string file = libs + "/" + (busy ? "libhish_linux_busybox.so" : kind=="glibc-static-fallback"?"libhish_linux_glibc_static_compat.so":glibc ? dynamic ? "libhish_linux_glibc_dynamic.so" : "libhish_linux_glibc_static.so" : dynamic ? "libhish_linux_musl_dynamic.so" : "libhish_linux_musl_static.so");
            const Image image(file); Describe(log, file);
            const auto main = MapDynamicImage(log, image, file); if (!main.valid) return 130;
            log("Linux main bias="+std::to_string(main.bias));
            LoadedImage loader;
            if (dynamic) {
                const auto name = libs + (glibc ? "/libhish_linux_glibc_loader.so" : "/libhish_linux_musl_loader.so");
                loader = MapDynamicImage(log, Image(name), name); if (!loader.valid) return 131;
            }
            const size_t length = 1024 * 1024;
            void* stack = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (stack == MAP_FAILED) return 132;
            uintptr_t top = reinterpret_cast<uintptr_t>(stack) + length;
            auto put = [&](const std::string& value) { top -= value.size()+1; memcpy(reinterpret_cast<void*>(top), value.c_str(), value.size()+1); return top; };
            std::vector<uintptr_t> argv {put(busy?"busybox":file)};
            if (busy) { argv.push_back(put("echo")); argv.push_back(put("ALPINE_BUSYBOX_NATIVE_OK")); }
            std::vector<std::string> environment {"HOME="+root+"/home", "LD_LIBRARY_PATH="+libs,
                "LD_PRELOAD="+libs+(kind=="glibc-compatible"?"/libhish_linux_glibc_compat.so":kind=="glibc-separate"?"/libhish_linux_glibc_separate.so":kind=="glibc-padded"?"/libhish_linux_glibc_padded.so":glibc?"/libhish_linux_glibc_libc.so":"/libhish_linux_musl_loader.so")+(kind=="alpine-busybox-compatible"?":"+libs+"/libhish_linux_identity_padded.so":"")};
            if(kind=="glibc-static-compatible"||kind=="glibc-static-fallback"||kind=="glibc-compatible"||kind=="glibc-padded"||kind=="glibc-separate")environment.push_back("GLIBC_TUNABLES=glibc.pthread.rseq=0");
            std::vector<uintptr_t> env; for (const auto& value : environment) env.push_back(put(value));
            top -= 16; if (getentropy(reinterpret_cast<void*>(top),16)!=0) { Error(log,"Linux AT_RANDOM",errno); return 133; } const auto random=top;
            std::vector<uintptr_t> words {argv.size()}; words.insert(words.end(),argv.begin(),argv.end()); words.push_back(0);
            words.insert(words.end(),env.begin(),env.end()); words.push_back(0);
            const uintptr_t aux[] = {AT_PAGESZ,static_cast<uintptr_t>(sysconf(_SC_PAGESIZE)),AT_PHDR,main.phdr,AT_PHENT,sizeof(Elf64_Phdr),AT_PHNUM,image.header.e_phnum,
                AT_ENTRY,main.entry,AT_BASE,loader.bias,AT_UID,getuid(),AT_EUID,geteuid(),AT_GID,getgid(),AT_EGID,getegid(),AT_SECURE,0,
                AT_EXECFN,argv[0],AT_RANDOM,random,AT_HWCAP,kind=="glibc-no-hwcap"?0:getauxval(AT_HWCAP),AT_HWCAP2,kind=="glibc-no-hwcap"?0:getauxval(AT_HWCAP2),AT_SYSINFO_EHDR,getauxval(AT_SYSINFO_EHDR),AT_CLKTCK,getauxval(AT_CLKTCK),AT_NULL,0};
            words.insert(words.end(),std::begin(aux),std::end(aux));top=(top-words.size()*sizeof(uintptr_t))&~uintptr_t(15);memcpy(reinterpret_cast<void*>(top),words.data(),words.size()*sizeof(uintptr_t));
            struct sigaction fault{};fault.sa_sigaction=LinuxFault;fault.sa_flags=SA_SIGINFO;sigemptyset(&fault.sa_mask);sigaction(SIGSYS,&fault,nullptr);sigaction(SIGSEGV,&fault,nullptr);
            log("ENTER unmodified Linux libc ABI kind="+kind);
#if defined(__aarch64__)
            Enter(reinterpret_cast<void*>(dynamic?loader.entry:main.entry),reinterpret_cast<void*>(top));
#else
            return 134;
#endif
        });
    }
}
void GlibcMaps(const Log& log, int reportFd, const std::string& libs)
{
    const std::string file=libs+"/libhish_linux_glibc_libc.so";const Image image(file);const size_t page=sysconf(_SC_PAGESIZE);
    size_t text=0,whole=0;for(const auto& ph:image.ph)if(ph.p_type==PT_LOAD){whole=std::max(whole,size_t(ph.p_vaddr+ph.p_memsz+page-1)&~(page-1));if(ph.p_flags&PF_X)text=(ph.p_filesz+page-1)&~(page-1);}
    for(const std::string mode:{"text-RX","whole-RX","whole-RX-denywrite","whole-RX-fixed","whole-RX-BTI"})Child(log,reportFd,"glibc-map/"+mode,[&](){
        const int fd=open(file.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0){Error(log,"glibc file open",errno);return 135;}
        size_t length=mode=="text-RX"?text:whole;void* wanted=nullptr;int flags=MAP_PRIVATE;
        if(mode=="whole-RX-denywrite")flags|=0x800;
        if(mode=="whole-RX-fixed"){void* reserve=mmap(nullptr,length+65536,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(reserve==MAP_FAILED)return 135;wanted=reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(reserve)+65535)&~uintptr_t(65535));flags|=MAP_FIXED;}
        log("glibc reproduction mmap length="+std::to_string(length)+" fileSize="+std::to_string(image.bytes.size())+" flags="+std::to_string(flags));
        void* memory=mmap(wanted,length,PROT_READ|PROT_EXEC|(mode=="whole-RX-BTI"?0x10:0),flags,fd,0);const int error=errno;close(fd);
        if(memory==MAP_FAILED){Error(log,"glibc reproduction mmap",error);return 136;}log("glibc reproduction map PASS");munmap(memory,length);return 0;
    });
}
void Manual(const Log& log, int reportFd, const std::string& root, const std::string& libs, const std::string& where)
{
    const bool runtime = where == "static-bundle" || where == "static-pie";
    const std::string name = where == "static-pie" ? "libhish_probe_static_pie.so" : runtime ? "libhish_probe_static_runtime.so" : "libhish_probe_bare.so";
    const std::string path = runtime || where == "memfd" ? libs + "/" + name : Path(log, root, libs, where, name);
    const Image image(path);
    Child(log, reportFd, "manual-ELF/" + where, [&]() {
        if (!image.valid || (image.header.e_type != ET_DYN && !(runtime && image.header.e_type == ET_EXEC))) { log("manual invalid owned ELF"); return 78; }
        const size_t page = sysconf(_SC_PAGESIZE);
        uint64_t low = UINT64_MAX, high = 0;
        for (const auto& ph : image.ph) {
            if (ph.p_type == PT_INTERP || (ph.p_type == PT_TLS && !runtime)) { log("manual interpreter/TLS unsupported"); return 78; }
            if (ph.p_type == PT_DYNAMIC && Range(ph.p_offset, ph.p_filesz, image.bytes.size())) {
                for (size_t i = 0; i < ph.p_filesz / sizeof(Elf64_Dyn); ++i) {
                    Elf64_Dyn entry {}; memcpy(&entry, image.bytes.data() + ph.p_offset + i * sizeof(entry), sizeof(entry));
                    if (entry.d_tag == DT_NEEDED || (!runtime && (entry.d_tag == DT_RELASZ || entry.d_tag == DT_RELSZ || entry.d_tag == DT_PLTRELSZ) && entry.d_un.d_val)) {
                        log("manual dependencies/relocations unsupported"); return 78;
                    }
                }
            }
            if (ph.p_type == PT_LOAD) {
                if (ph.p_filesz > ph.p_memsz || !Range(ph.p_offset, ph.p_filesz, image.bytes.size()) || ph.p_vaddr > (runtime ? (1ULL << 40) : (1ULL << 24)) || ph.p_memsz > (1ULL << 24)) return 78;
                low = std::min(low, ph.p_vaddr & ~(page - 1));
                high = std::max(high, (ph.p_vaddr + ph.p_memsz + page - 1) & ~(page - 1));
            }
        }
        if (high <= low || high - low > (1ULL << 24)) return 78;
        void* wanted = image.header.e_type == ET_EXEC ? reinterpret_cast<void*>(low) : nullptr;
        if (wanted) {
            std::ifstream maps("/proc/self/maps"); std::string line; int overlaps = 0;
            while (std::getline(maps, line)) {
                unsigned long long begin = 0, end = 0;
                if (sscanf(line.c_str(), "%llx-%llx", &begin, &end) == 2 && begin < high && end > low) { log("manual occupied range " + line); ++overlaps; }
            }
            log("manual listed map overlaps=" + std::to_string(overlaps));
        }
        const int reserveFlags = MAP_PRIVATE | MAP_ANONYMOUS | (wanted ? MAP_FIXED_NOREPLACE : 0);
        log("manual reserve low=" + std::to_string(low) + " length=" + std::to_string(high - low) + " fixed-no-replace=" + std::to_string(wanted != nullptr));
        void* area = mmap(wanted, high - low, PROT_NONE, reserveFlags, -1, 0);
        if (area == MAP_FAILED) { Error(log, "manual reserve", errno); return 79; }
        if (wanted && area != wanted) { log("manual exact-address request not honored; no overwrite attempted"); munmap(area, high - low); return 79; }
        const uintptr_t bias = reinterpret_cast<uintptr_t>(area) - low;
        const int fd = where == "memfd" ? MemoryFile(log, path) : open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { Error(log, "manual open", errno); return 79; }
        for (const auto& ph : image.ph) {
            if (ph.p_type != PT_LOAD || !ph.p_memsz) continue;
            const uint64_t start = ph.p_vaddr & ~(page - 1);
            const size_t length = ((ph.p_vaddr + ph.p_memsz + page - 1) & ~(page - 1)) - start;
            int prot = (ph.p_flags & PF_R ? PROT_READ : 0) | (ph.p_flags & PF_W ? PROT_WRITE : 0) | (ph.p_flags & PF_X ? PROT_EXEC : 0);
            // MAP_FIXED is confined to this child's newly reserved, bounded area.
            if (start < low || start + length > high) return 78;
            void* memory;
            if (ph.p_flags & PF_X) {
                if (ph.p_flags & PF_W || ph.p_memsz != ph.p_filesz || (ph.p_offset % page) != (ph.p_vaddr % page)) return 78;
                memory = mmap(reinterpret_cast<void*>(bias + start), length, prot, MAP_PRIVATE | MAP_FIXED, fd, ph.p_offset & ~(page - 1));
            } else {
                memory = mmap(reinterpret_cast<void*>(bias + start), length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (memory != MAP_FAILED) {
                    memcpy(reinterpret_cast<void*>(bias + ph.p_vaddr), image.bytes.data() + ph.p_offset, ph.p_filesz);
                    if (mprotect(memory, length, prot) != 0) { Error(log, "manual data protection", errno); return 79; }
                }
            }
            if (memory == MAP_FAILED) { Error(log, "manual PT_LOAD flags=" + std::to_string(ph.p_flags), errno); return 79; }
            log("manual PT_LOAD PASS flags=" + std::to_string(ph.p_flags));
        }
        close(fd);
        // Only owned fixtures: no dynamic linker or general relocations. For
        // the static-C fixture its own CRT must initialize libc, TLS and ctors.
        void* stack = mmap(nullptr, page * 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) return 79;
        uintptr_t top = reinterpret_cast<uintptr_t>(stack) + page * 16;
        const char name[] = "hish-owned-bare";
        top -= sizeof(name); memcpy(reinterpret_cast<void*>(top), name, sizeof(name)); const auto arg = top;
        const std::string home = "HOME=" + root + "/home";
        top -= home.size() + 1; memcpy(reinterpret_cast<void*>(top), home.c_str(), home.size() + 1); const auto env = top;
        top -= 16; memset(reinterpret_cast<void*>(top), 0x42, 16); const auto random = top;
        uintptr_t phdr = 0;
        for (const auto& ph : image.ph) {
            if (ph.p_type == PT_PHDR) phdr = bias + ph.p_vaddr;
            if (!phdr && ph.p_type == PT_LOAD && image.header.e_phoff >= ph.p_offset && image.header.e_phoff - ph.p_offset < ph.p_filesz) phdr = bias + ph.p_vaddr + image.header.e_phoff - ph.p_offset;
        }
        const uintptr_t words[] = {1, arg, 0, env, 0,
            AT_PAGESZ, page, AT_PHDR, phdr, AT_PHENT, sizeof(Elf64_Phdr),
            AT_PHNUM, image.header.e_phnum, AT_ENTRY, bias + image.header.e_entry, AT_BASE, 0,
            AT_UID, getuid(), AT_EUID, geteuid(), AT_GID, getgid(), AT_EGID, getegid(), AT_SECURE, 0,
            AT_EXECFN, arg, AT_RANDOM, random, AT_NULL, 0};
        top = (top - sizeof(words)) & ~static_cast<uintptr_t>(15);
        memcpy(reinterpret_cast<void*>(top), words, sizeof(words));
        if (image.header.e_entry < low || image.header.e_entry >= high) return 78;
        log(runtime ? "manual ENTER owned static-C CRT" : "manual ENTER Linux ABI stack (no libc/TLS/relocations claim)");
#if defined(__aarch64__)
        Enter(reinterpret_cast<void*>(bias + image.header.e_entry), reinterpret_cast<void*>(top));
#else
        return 78;
#endif
    });
}
}
bool IsRouteAudit(const std::string& kind)
{
    for (const char* name : {"inventory", "pty", "cleanup", "lifecycle", "glibc-maps", "linux-programs", "self-code", "loader-api", "mapping-variants", "storage-origins", "system-loader", "devices", "interpreter", "inline-syscalls", "other-memory", "archive", "map-bundle", "map-home", "map-cache", "map-memfd", "map-anon",
        "exec-bundle", "exec-home", "exec-memfd", "load-bundle", "load-home", "load-cache", "load-memfd",
        "manual-bundle", "manual-home", "manual-memfd", "manual-static-bundle", "manual-static-pie"}) if (kind == std::string("audit-") + name) return true;
    return false;
}
void RouteAudit(const std::string& root, const std::string& libs, const std::string& kind, int reportFd, const Log& log)
{
    if (mkdir(Cache(root).c_str(), 0700) != 0 && errno != EEXIST) Error(log, "audit cache directory", errno);
    log("audit scope: current app UID, no added permission, no system mutation");
    if (kind == "audit-loader-api") LoaderApis(log, reportFd, root, libs);
    else if(kind=="audit-lifecycle") Child(log,reportFd,"owned-subreaper",[&](){
        if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)){Error(log,"subreaper set",errno);return 137;}
        int enabled=0;if(prctl(PR_GET_CHILD_SUBREAPER,&enabled,0,0,0)){Error(log,"subreaper get",errno);return 137;}
        log("owned subreaper enabled="+std::to_string(enabled));return enabled==1?0:138;
    });
    else if (kind == "audit-linux-programs") LinuxPrograms(log, reportFd, root, libs);
    else if (kind == "audit-glibc-maps") GlibcMaps(log, reportFd, libs);
    else if (kind == "audit-self-code") SelfCode(log, reportFd, libs);
    else if (kind == "audit-mapping-variants") MappingVariants(log, reportFd, libs);
    else if (kind == "audit-storage-origins") StorageOrigins(log, reportFd, root, libs);
    else if (kind == "audit-system-loader") SystemLoader(log, reportFd, root, libs);
    else if (kind == "audit-devices") {
        for (const char* device : {"/dev/kvm", "/dev/vmm", "/dev/hypervisor", "/dev/dri/renderD128", "/dev/mali0", "/dev/ptmx", "/dev/pts/0", "/dev/tty"}) {
            struct stat st {}; errno = 0; const int result = stat(device, &st); const int error = result == 0 ? 0 : errno;
            log(std::string("DEVICE ") + device + " stat=" + std::to_string(result) + " errno=" + std::to_string(error));
            // Read-only availability check, no driver configuration or device requests.
            const int fd = open(device, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            log(std::string("DEVICE ") + device + " open=" + std::to_string(fd) + " errno=" + std::to_string(fd < 0 ? errno : 0));
            if (fd >= 0) close(fd);
        }
    }
    else if (kind == "audit-interpreter") Interpreter(log, reportFd, root, libs);
    else if (kind == "audit-inline-syscalls") InlineCalls(log, reportFd);
    else if (kind == "audit-other-memory") OtherMemory(log, reportFd);
    else if (kind == "audit-archive") Archive(log, reportFd, libs);
    else if (kind == "audit-inventory") {
        for (const char* name : {"libhish_probe_exec.so", "libhish_probe_payload.so", "libhish_probe_bare.so", "libhish_probe_static.so", "libhish_probe_static_runtime.so", "libhish_probe_static_pie.so", "libhish_probe_dynamic_runtime.so"}) Describe(log, libs + "/" + name);
        for (const char* name : {"/lib/ld-musl-aarch64.so.1", "/system/bin/ld-musl-aarch64.so.1", "/system/bin/toybox", "/dev/ptmx", "/dev/pts/ptmx"}) Describe(log, name);
    } else if (kind == "audit-cleanup") {
        for (const auto& directory : {root + "/home", Cache(root)}) {
            for (const char* name : {"libhish_probe_exec.so", "libhish_probe_payload.so", "libhish_probe_bare.so", "libhish_probe_static.so", "libhish_probe_dynamic_runtime.so"}) {
                const std::string file = directory + "/audit-" + name;
                if (unlink(file.c_str()) == 0) log("removed owned fixture " + file);
                else if (errno != ENOENT) Error(log, "remove owned fixture", errno);
            }
        }
        if (rmdir(Cache(root).c_str()) != 0 && errno != ENOENT) Error(log, "remove owned cache directory", errno);
        log("audit cleanup END; reports retained");
    } else if (kind == "audit-pty") {
        for (const char* path : {"/dev/ptmx", "/dev/pts/ptmx", "/dev/tty"}) {
            for (const int flags : {O_RDWR | O_NOCTTY | O_CLOEXEC, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC}) {
                errno = 0; const int fd = open(path, flags); const int error = errno;
                log(std::string("PTY open ") + path + " flags=" + std::to_string(flags) + " result=" + std::to_string(fd));
                if (fd < 0) Error(log, "PTY open", error);
                else { int locked = -1; const int result = ioctl(fd, TIOCGPTLCK, &locked); log("TIOCGPTLCK=" + std::to_string(result) + " locked=" + std::to_string(locked)); close(fd); }
            }
            const int fd = syscall(SYS_openat, AT_FDCWD, path, O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
            if (fd < 0) Error(log, std::string("PTY syscall openat ") + path, errno); else { log("PTY syscall openat PASS"); close(fd); }
        }
    } else if (kind.rfind("audit-exec-", 0) == 0) Execs(log, reportFd, root, libs, kind.substr(11));
    else if (kind.rfind("audit-load-", 0) == 0) Loads(log, reportFd, root, libs, kind.substr(11));
    else if (kind.rfind("audit-map-", 0) == 0) Maps(log, reportFd, root, libs, kind.substr(10));
    else if (kind.rfind("audit-manual-", 0) == 0) Manual(log, reportFd, root, libs, kind.substr(13));
}
}
