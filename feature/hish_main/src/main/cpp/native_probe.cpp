#include "native_probe.h"
#include "native_terminal.h"
#include "native_route_audit.h"
#include "native_ptrace_audit.h"
#include "native_seccomp_audit.h"
#include "native_exec_probe.h"
#include "linux_library_path.h"
#include <AbilityKit/native_child_process.h>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <hilog/log.h>
#include <poll.h>
#include <pty.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>
#ifdef HISH_DOWNLOAD_ABI_TEST
#include "native_download_probe.h"
#include "native_library_trace.h"
#endif
extern char** environ;

namespace {
void AuditProcessStarted(int error, OHIPCRemoteProxy* remote)
{
    OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HiSHProbe", "framework callback code=%{public}d remote=%{public}d", error, remote != nullptr);
    if (remote) {
        auto destroy = reinterpret_cast<void (*)(OHIPCRemoteProxy*)>(dlsym(RTLD_DEFAULT, "OH_IPCRemoteProxy_Destroy"));
        if (destroy) destroy(remote);
    }
}
class Report {
public:
    int fd;
    std::string text;
    explicit Report(const std::string& file) : fd(open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600))
    {
        if (fd < 0) { throw std::runtime_error("probe report open: " + std::to_string(errno)); }
    }
    ~Report() { close(fd); }
    void Add(const std::string& line)
    {
        text += line + "\n";
        const std::string record = line + "\n";
        size_t offset = 0;
        while (offset < record.size()) {
            const ssize_t n = write(fd, record.data() + offset, record.size() - offset);
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0) { throw std::runtime_error("probe report write failed"); }
            offset += n;
        }
        fsync(fd);
        std::istringstream lines(line); std::string item;
        while (std::getline(lines, item)) { OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HiSHProbe", "%{public}s", item.c_str()); }
    }
    void Error(const std::string& stage, int code) { Add(stage + " errno=" + std::to_string(code) + " (" + strerror(code) + ")"); }
};
std::string ReadFile(const std::string& path)
{
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
void MakeDir(const std::string& path)
{
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) { throw std::runtime_error("probe mkdir: " + std::to_string(errno)); }
}
std::string LibraryDir()
{
    Dl_info info {};
    if (!dladdr(reinterpret_cast<void*>(&hish::Probe), &info) || !info.dli_fname) { throw std::runtime_error("dladdr failed"); }
    const std::string file(info.dli_fname);
    return file.substr(0, file.find_last_of('/'));
}
void Copy(Report& out, const std::string& source, const std::string& target)
{
    const int in = open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) { out.Error("copy source " + source, errno); return; }
    const int dest = open(target.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700);
    if (dest < 0) { const int error = errno; close(in); out.Error("copy target", error); return; }
    char bytes[8192]; ssize_t count; bool good = true;
    while ((count = read(in, bytes, sizeof(bytes))) > 0) {
        ssize_t offset = 0;
        while (offset < count) {
            const ssize_t n = write(dest, bytes + offset, count - offset);
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0) { good = false; break; }
            offset += n;
        }
        if (!good) { break; }
    }
    if (count < 0) { good = false; }
    fsync(dest); close(in); close(dest);
    out.Add(std::string("copy ") + (good ? "PASS " : "FAIL ") + target);
    if (chmod(target.c_str(), 0700) != 0) { out.Error("chmod", errno); }
}
void Wait(Report& out, pid_t pid)
{
    int status = 0;
    for (int n = 0; n < 200; ++n) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) { out.Add("child exit=" + std::to_string(WEXITSTATUS(status))); }
            else if (WIFSIGNALED(status)) { out.Add("child signal=" + std::to_string(WTERMSIG(status))); }
            return;
        }
        if (result < 0 && errno != EINTR) { out.Error("waitpid", errno); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    out.Add("child TIMEOUT; killing owned test child");
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}
void Baseline(Report& out, const std::string& root)
{
    utsname name {}; uname(&name);
    out.Add("arch=" + std::string(name.machine) + " kernel=" + name.release);
    const int seccomp = prctl(PR_GET_SECCOMP); const int seccompError = errno;
    out.Add("prctl seccomp=" + std::to_string(seccomp) + " errno=" + std::to_string(seccomp < 0 ? seccompError : 0) + " no_new_privs=" + std::to_string(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0)));
    out.Add("selinux=" + ReadFile("/proc/self/attr/current"));
    std::istringstream status(ReadFile("/proc/self/status")); std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("Cap", 0) == 0 || line.rfind("Seccomp", 0) == 0 || line.rfind("NoNewPrivs", 0) == 0) { out.Add(line); }
    }
    const std::string file = root + "/home/.probe-write";
    const int fd = open(file.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) { out.Error("home open", errno); }
    else {
        const char value = 'x'; char back = 0;
        const bool ok = write(fd, &value, 1) == 1 && lseek(fd, 0, SEEK_SET) == 0 && read(fd, &back, 1) == 1 && back == value;
        close(fd); unlink(file.c_str()); out.Add(std::string("home read/write ") + (ok ? "PASS" : "FAIL"));
    }
    for (const char* path : {"/system/bin/sh", "/bin/sh", "/system/bin/toybox", "/lib/ld-musl-aarch64.so.1", "/system/bin/ld-musl-aarch64.so.1", "/dev/ptmx"}) {
        struct stat info {}; const int result = stat(path, &info); const int error = errno;
        out.Add(std::string("path=") + path + " stat=" + std::to_string(result) + " errno=" + std::to_string(result == 0 ? 0 : error) +
            " mode=" + std::to_string(result == 0 ? info.st_mode & 07777 : 0));
    }
    int pair[2]; const int result = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
    if (result == 0) { close(pair[0]); close(pair[1]); out.Add("socketpair PASS"); }
    else { out.Error("socketpair", errno); }
}
void Pty(Report& out)
{
    out.Add("openpty BEGIN");
    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) == 0) {
        close(master); close(slave); out.Add("openpty PASS");
    } else { out.Error("openpty", errno); }
    out.Add("posix_openpt BEGIN");
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) { out.Error("posix_openpt", errno); }
    else {
        out.Add("posix_openpt PASS");
        if (grantpt(master) != 0) { out.Error("grantpt", errno); }
        if (unlockpt(master) != 0) { out.Error("unlockpt", errno); }
        char* path = ptsname(master);
        if (path) { out.Add(std::string("ptsname=") + path); }
        else { out.Error("ptsname", errno); }
        close(master);
    }
}
void Module(Report& out, const std::string& path)
{
    out.Add("dlopen BEGIN " + path);
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) { const char* error = dlerror(); out.Add(std::string("dlopen FAIL ") + (error ? error : "unknown")); return; }
    out.Add("dlopen PASS");
    if (path.find("probe_exec") != std::string::npos) {
        auto main = reinterpret_cast<int (*)(int, char**)>(dlsym(module, "main"));
        if (!main) { out.Add("ELF main symbol unavailable"); }
        else {
            const pid_t pid = fork();
            if (pid == 0) {
                dup2(out.fd, 1); dup2(out.fd, 2);
                char name[] = "native-probe"; char* args[] = {name, nullptr};
                const int code = main(1, args); fflush(nullptr); _exit(code);
            }
            if (pid < 0) out.Error("fork ELF main", errno); else Wait(out, pid);
        }
        dlclose(module); return;
    }
    auto entry = reinterpret_cast<int (*)(int)>(dlsym(module, "HiSHProbePayload"));
    if (entry) { out.Add("module result=" + std::to_string(entry(out.fd))); }
    else { out.Add("dlsym FAIL"); }
    dlclose(module);
}
}

namespace hish {
std::string Probe(const std::string& root, const std::string& kind)
{
    const std::vector<std::string> allowed {"combined-notify-check", "combined-check", "seccomp-check", "ptrace-prepare", "proot-check", "ptrace-compiled", "ptrace-advanced", "ptrace-check", "input-check", "process-check", "job-check", "bridge-check", "exec-preload", "compiler-check", "jit-check", "workflow-check", "network-check", "session-check", "tty-check", "hybrid-check", "framework-api", "baseline", "pty", "fork", "vfork-exec", "fork-exec", "spawn-system", "spawn-bin", "exec-system",
        "spawn-bundle", "spawn-home", "exec-bundle", "exec-home", "module-bundle", "module-home", "fork-module", "prepare", "reports", "spawn-loader", "exec-loader", "shell-check", "spawn-toybox", "fexec-bundle", "fexec-home", "execveat-home", "module-elf", "pipe-fork", "pipe-plain", "file-modes", "toybox-list", "code-pages", "memfd-exec", "memfd-module", "framework"};
    const bool saved = kind.rfind("saved-", 0) == 0;
    const std::string selected = saved ? kind.substr(6) : kind;
    bool valid = selected == "exec-check" || IsRouteAudit(selected); for (const auto& item : allowed) { valid = valid || item == selected; }
#ifdef HISH_DOWNLOAD_ABI_TEST
    valid = valid || selected == "download-check" || selected == "library-check" || selected == "library-trace" || selected == "vim-trace";
#endif
    if (!valid || root.empty() || root.front() != '/' || root.find('\n') != std::string::npos) { throw std::runtime_error("Invalid probe"); }
    MakeDir(root); MakeDir(root + "/home"); MakeDir(root + "/reports");
    Report out(root + "/reports/" + kind + ".log");
    out.Add("PROBE " + kind + " BEGIN pid=" + std::to_string(getpid()) + " uid=" + std::to_string(getuid()));
    out.Add("root=" + root + " libs=" + LibraryDir());
    if (saved) { out.Add(ReadFile(root + "/reports/" + selected + ".log")); }
#ifdef HISH_DOWNLOAD_ABI_TEST
    else if (kind == "library-trace") { HiSHLibraryTrace(root,ReadFile(root+"/reports/library-check.log"),[&](const std::string& line){out.Add(line);}); }
    else if (kind == "vim-trace") { HiSHLibraryTrace(root,ReadFile(root+"/reports/library-check.log"),[&](const std::string& line){out.Add(line);},true); }
    else if (kind == "download-check" || kind == "library-check") {
        HiSHDownloadProbe(root, [&](const std::string& line) { out.Add(line); });
        if(kind=="library-check"){
            const auto installed=LibraryDir();char search[HISH_LIBRARY_PATH_LIMIT],canonical[PATH_MAX];
            auto entry="LD_LIBRARY_PATH="+root+"/home";char* privateEnvironment[]={entry.data(),nullptr};
            if(HishMuslLibraryPath(search,sizeof(search),installed.c_str(),privateEnvironment)||!realpath((root+"/home").c_str(),canonical)||std::string(search).find(canonical)==std::string::npos)throw std::runtime_error("private library search policy failed");
            out.Add("LIBRARY_POLICY_PRIVATE_OK");
            char* noEnvironment[]={nullptr};
            if(HishMuslLibraryPath(search,sizeof(search),installed.c_str(),noEnvironment)||!realpath((root+"/usr/lib/musl").c_str(),canonical)||std::string(search).find(canonical)==std::string::npos)throw std::runtime_error("default musl library search failed");
            out.Add("LIBRARY_POLICY_DEFAULT_OK");
            for(const char* bad:{".","/system",":/system","/system:"}){
                entry=std::string("LD_LIBRARY_PATH=")+bad;char* environment[]={entry.data(),nullptr};
                if(!HishMuslLibraryPath(search,sizeof(search),installed.c_str(),environment))throw std::runtime_error("unsafe library search path accepted");
            }
            out.Add("LIBRARY_POLICY_RELATIVE_EMPTY_SYSTEM_REJECTED_OK");
            char small[8];
            if(!HishMuslLibraryPath(small,sizeof(small),installed.c_str(),noEnvironment)||errno!=E2BIG)throw std::runtime_error("library search buffer limit failed");
            out.Add("LIBRARY_POLICY_BOUNDS_OK");
        }
        if(kind=="library-check")for(const char* check:{"baseline","code-pages","exec-check","compiler-check","workflow-check","combined-check","hybrid-check","shell-check","process-check","bridge-check","session-check","jit-check","tty-check","input-check","job-check"}){
            out.Add(std::string("LIBRARY_REGRESSION_BEGIN ")+check);
            // Same probe bodies, without force-stopping/restarting the app
            // between cases. Each case retains its own report and real output.
            Probe(root,check);
            out.Add(std::string("LIBRARY_REGRESSION_END ")+check);
        }
    }
#endif
    else if (kind == "seccomp-check") { SeccompAudit(out.fd, [&](const std::string& line) { out.Add(line); }); }
    else if (kind == "ptrace-check") { PtraceAudit(out.fd, [&](const std::string& line) { out.Add(line); }); }
    else if (kind == "ptrace-advanced") { PtraceAdvancedAudit(root, out.fd, [&](const std::string& line) { out.Add(line); }); }
    else if (kind == "ptrace-compiled") { PtraceCompiledAudit(root, out.fd, [&](const std::string& line) { out.Add(line); }); }
    else if (IsRouteAudit(kind)) {
        RouteAudit(root, LibraryDir(), kind, out.fd, [&](const std::string& line) { out.Add(line); });
    }
    else if(kind=="exec-preload") {
        for(const bool absolute:{true,false}){
            out.Add(std::string("PRELOAD_CASE absolute=")+(absolute?"true":"false"));
            const std::string library=LibraryDir()+"/libhish_probe_payload.so",search=LibraryDir();
            const pid_t child=fork();
            if(!child){
                dup2(out.fd,1);dup2(out.fd,2);
                setenv("HISH_PRELOAD_AUDIT","1",1);setenv("LD_PRELOAD",absolute?library.c_str():"libhish_probe_payload.so",1);
                setenv("LD_LIBRARY_PATH",search.c_str(),1);
                char* args[]={const_cast<char*>("toybox"),const_cast<char*>("true"),nullptr};
                execve("/system/bin/toybox",args,environ);dprintf(1,"PRELOAD_EXEC_ERRNO=%d\n",errno);_exit(127);
            }
            if(child>0)Wait(out,child);else out.Error("preload fork",errno);
        }
        const std::string report=ReadFile(root+"/reports/exec-preload.log");
        out.Add(std::string("PRELOAD_CONSTRUCTOR_OBSERVED=")+(report.find("HISH_OHOS_PRELOAD_CONSTRUCTOR_OK")!=std::string::npos?"true":"false"));
        out.Add(report);
    }
    else if (kind == "framework-api") {
        auto supported = reinterpret_cast<bool (*)()>(dlsym(RTLD_DEFAULT, "OH_Ability_IsNativeChildProcessSupported"));
        out.Add(std::string("native supported API26=") + (supported ? (supported() ? "true" : "false") : "symbol absent"));
        out.Add("CreateNativeChildProcess BEGIN");
        out.Add("CreateNativeChildProcess code=" + std::to_string(OH_Ability_CreateNativeChildProcess("libhish_probe_payload.so", AuditProcessStarted)));
        for (const auto mode : {NCP_ISOLATION_MODE_NORMAL, NCP_ISOLATION_MODE_ISOLATED}) {
            auto* configs = OH_Ability_CreateChildProcessConfigs();
            if (!configs) { out.Add("child configs null"); continue; }
            out.Add("config isolation=" + std::to_string(mode) + " set=" + std::to_string(OH_Ability_ChildProcessConfigs_SetIsolationMode(configs, mode)));
            out.Add("CreateWithConfigs code=" + std::to_string(OH_Ability_CreateNativeChildProcessWithConfigs("libhish_probe_payload.so", configs, AuditProcessStarted)));
            char params[] = "owned-audit"; NativeChildProcess_Args args {params, {nullptr}}; int32_t pid = -1;
            const auto code = OH_Ability_StartNativeChildProcessWithConfigs("libhish_probe_payload.so:HiSHRouteChild", args, configs, &pid);
            out.Add("StartWithConfigs code=" + std::to_string(code) + " pid=" + std::to_string(pid));
            if (code == NCP_NO_ERROR) OH_Ability_KillChildProcess(pid);
            OH_Ability_DestroyChildProcessConfigs(configs);
        }
    }
    else if (kind == "framework") {
        const int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (fd < 0) out.Error("framework fd", errno);
        else {
            std::string params = "probe\n" + root + "\n" + std::to_string(getpid());
            NativeChildProcess_Fd terminal {const_cast<char*>("terminal"), fd, nullptr};
            NativeChildProcess_Args args {params.data(), {&terminal}};
            NativeChildProcess_Options options {NCP_ISOLATION_MODE_NORMAL, 0};
            int32_t pid = -1;
            out.Add("OH_Ability_StartNativeChildProcess BEGIN");
            const auto code = OH_Ability_StartNativeChildProcess("libhish_main.so:HiSHNativeMain", args, options, &pid);
            out.Add("framework code=" + std::to_string(code));
            close(fd);
            if (code == NCP_NO_ERROR) { OH_Ability_KillChildProcess(pid); }
        }
    }
    else if (kind == "code-pages") {
        const size_t size = sysconf(_SC_PAGESIZE);
        out.Add("anonymous RW mmap BEGIN");
        void* code = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED) { out.Error("mmap", errno); }
        else {
#if defined(__aarch64__)
            const uint32_t instructions[] = {0x52800540, 0xd65f03c0}; // mov w0,#42; ret
#else
            const uint8_t instructions[] = {0xb8,42,0,0,0,0xc3}; // mov eax,42; ret
#endif
            memcpy(code, instructions, sizeof(instructions));
            __builtin___clear_cache(static_cast<char*>(code), static_cast<char*>(code) + sizeof(instructions));
            out.Add("mprotect RW->RX BEGIN");
            if (mprotect(code, size, PROT_READ | PROT_EXEC) != 0) { out.Error("mprotect RX", errno); }
            else {
                out.Add("anonymous native call BEGIN");
                const int value = reinterpret_cast<int (*)()>(code)();
                out.Add("anonymous native result=" + std::to_string(value));
            }
            munmap(code, size);
        }
    }
    else if (kind == "memfd-exec" || kind == "memfd-module") {
        out.Add("memfd_create BEGIN");
        const int fd = memfd_create("hish-native-probe", MFD_CLOEXEC);
        if (fd < 0) { out.Error("memfd_create", errno); }
        else {
            const std::string image = ReadFile(LibraryDir() + (kind == "memfd-module" ? "/libhish_probe_payload.so" : "/libhish_probe_exec.so"));
            if (write(fd, image.data(), image.size()) != static_cast<ssize_t>(image.size())) { out.Error("memfd copy", errno); }
            else if (kind == "memfd-module") { Module(out, "/proc/self/fd/" + std::to_string(fd)); }
            else {
                out.Add("memfd fexecve BEGIN in child");
                const pid_t pid = fork();
                if (pid == 0) {
                    dup2(out.fd, 1); dup2(out.fd, 2);
                    char name[] = "native-probe"; char* args[] = {name, nullptr};
                    fexecve(fd, args, environ);
                    dprintf(2, "memfd fexecve errno=%d\n", errno); _exit(127);
                }
                if (pid < 0) out.Error("fork", errno); else Wait(out, pid);
            }
            close(fd);
        }
    }
    else if (kind == "pipe-fork" || kind == "pipe-plain") {
        int fds[2];
        if ((kind == "pipe-plain" ? pipe(fds) : pipe2(fds, O_CLOEXEC | O_NONBLOCK)) != 0) { out.Error("pipe", errno); }
        else {
            out.Add("pipe flags read=" + std::to_string(fcntl(fds[0], F_GETFL)) + " write=" + std::to_string(fcntl(fds[1], F_GETFL)));
            const pid_t pid = fork();
            if (pid == 0) {
                close(fds[0]); dup2(fds[1], 1); close(fds[1]);
                const char text[] = "PIPE_CHILD_OK";
                const int flags = fcntl(1, F_GETFL);
                const auto written = write(1, text, sizeof(text) - 1);
                dprintf(out.fd, "child flags=%d write=%ld errno=%d\n", flags, static_cast<long>(written), errno);
                _exit(written == sizeof(text) - 1 ? 42 : 43);
            }
            close(fds[1]);
            if (pid < 0) out.Error("fork", errno); else Wait(out, pid);
            char text[128]; const ssize_t count = read(fds[0], text, sizeof(text));
            if (count > 0) out.Add(std::string(text, count)); else out.Error("pipe read", errno);
            close(fds[0]);
        }
    }
    else if (kind == "file-modes") {
        for (const auto& file : {LibraryDir() + "/libhish_probe_exec.so", root + "/home/native-probe"}) {
            struct stat info {}; const int result = stat(file.c_str(), &info);
            out.Add(file + " stat=" + std::to_string(result) + " mode=" + std::to_string(info.st_mode & 07777) + " uid=" + std::to_string(info.st_uid) + " X_OK=" + std::to_string(access(file.c_str(), X_OK)));
        }
    }
    else if (kind == "reports") {
        for (const auto& item : allowed) {
            if (item != "reports") { out.Add("SAVED " + item + "\n" + ReadFile(root + "/reports/" + item + ".log")); }
        }
    }
    else if(kind=="session-check") {
        for(const std::string command:{"linux-musl-check","linux-glibc-check"}) for(const bool shellExit:{false,true}) {
            const auto session=Start(root,80,24,"shell");std::string transcript;pid_t detached=-1;
            const std::string script=command+" --orphan\n";
            Write(session.id,reinterpret_cast<const uint8_t*>(script.data()),script.size());
            for(int attempt=0;attempt<200;attempt++) {
                auto data=Read(session.id);transcript.append(data.bytes.begin(),data.bytes.end());
                const auto at=transcript.find("LINUX_DETACHED_PID=");
                if(at!=std::string::npos&&transcript.find('\n',at)!=std::string::npos){detached=atoi(transcript.c_str()+at+19);break;}
                if(!data.running)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const bool created=detached>1&&kill(detached,0)==0;
            out.Add("SESSION command="+command+" detached="+std::to_string(detached)+" shellExit="+std::to_string(shellExit)+" created="+std::to_string(created));
            if(shellExit) {
                const std::string exit="exit\n";Write(session.id,reinterpret_cast<const uint8_t*>(exit.data()),exit.size());
            } else { Close(session.id); }
            bool reaped=false;
            for(int attempt=0;attempt<200&&created;attempt++) {
                if(kill(detached,0)<0&&errno==ESRCH){reaped=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Close(session.id);
            out.Add(std::string("SESSION ")+(created&&reaped?"PASS":"ASSERT_CLEANUP_FAIL"));
        }
    }
    else if(kind=="input-check") {
        for(const std::string command:{"linux-musl-check","linux-glibc-check"}){
            const auto session=Start(root,80,24,"shell");std::string transcript;bool sent=false,flushed=false,fed=false,timed=false,background=false,finished=false;
            const auto sendText=[&](const std::string& text){size_t at=0;while(at<text.size()){size_t n=Write(session.id,reinterpret_cast<const uint8_t*>(text.data()+at),text.size()-at);at+=n;if(!n)std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
            sendText(command+" --tty-input "+(command=="linux-musl-check"?"linux-glibc-check":"linux-musl-check")+"\n");
            try{
                for(int tick=0;tick<2000;tick++){
                    auto data=Read(session.id);if(!data.bytes.empty()){std::string text(data.bytes.begin(),data.bytes.end());transcript+=text;out.Add(text);}
                    if(!sent&&transcript.find("INPUT_EOF_READY")!=std::string::npos){sendText(std::string("ab\4cd\4e\n",8));sent=true;}
                    if(!flushed&&transcript.find("INPUT_FLUSH_READY")!=std::string::npos){sendText("discard\4");flushed=true;}
                    if(!fed&&transcript.find("INPUT_AFTER_FLUSH_READY")!=std::string::npos){sendText("f\n");fed=true;}
                    if(!timed&&transcript.find("INPUT_INTERBYTE_READY")!=std::string::npos){sendText("x");timed=true;}
                    if(!background&&transcript.find("INPUT_BACKGROUND_FEED_READY")!=std::string::npos){sendText("q\n");background=true;}
                    if(!finished&&transcript.find("INPUT_COMPLETE")!=std::string::npos){sendText("printf 'INPUT_SHELL_ALIVE\\n'\nexit\n");finished=true;}
                    if(!data.running)break;
                    if(tick==1999)out.Add("INPUT TIMEOUT");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }catch(...){Close(session.id);throw;}
            Close(session.id);out.Add("INPUT command="+command+" result="+(sent&&flushed&&fed&&timed&&background&&finished&&transcript.find("INPUT_SHELL_ALIVE")!=std::string::npos?"PASS":"FAIL"));
        }
    }
    else if(kind=="job-check") {
        const auto session=Start(root,80,24,"shell");std::string transcript;int phase=0,phaseTick=0;size_t stopOutput=0;
        const auto sendText=[&](const std::string& text){Write(session.id,reinterpret_cast<const uint8_t*>(text.data()),text.size());};
        sendText("printf 'JOB_LAUNCH\\n'; sleep 30\n");
        try{
            for(int tick=0;tick<2000;tick++){
                auto data=Read(session.id);if(!data.bytes.empty()){std::string text(data.bytes.begin(),data.bytes.end());transcript+=text;out.Add(text);}
                if(phase==0&&transcript.find("JOB_LAUNCH")!=std::string::npos){phase=1;phaseTick=tick;}
                if(phase==1&&tick-phaseTick>=20){const uint8_t key=26;Write(session.id,&key,1);phase=2;stopOutput=transcript.size();}
                if(phase==2&&transcript.find("$ ",stopOutput)!=std::string::npos){
                    sendText("jobs\nbg\nprintf 'JOB_BG_DONE\\n'\n");phase=3;
                }
                if(phase==3&&transcript.find("JOB_BG_DONE")!=std::string::npos){sendText("fg\n");phase=4;phaseTick=tick;}
                if(phase==4&&tick-phaseTick>=20){const uint8_t key=3;Write(session.id,&key,1);phase=5;phaseTick=tick;}
                if(phase==5&&tick-phaseTick>=10){sendText("status=$?\nprintf 'JOB_FG_STATUS=%s\\n' \"$status\"\nprintf 'JOB_CONTROL_END\\n'\nexit\n");phase=6;}
                if(!data.running)break;
                if(tick==1999)out.Add("JOB TIMEOUT");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }catch(...){Close(session.id);throw;}
        Close(session.id);
        out.Add(std::string("JOB result=")+(phase==6&&transcript.find("JOB_FG_STATUS=130")!=std::string::npos&&transcript.find("JOB_CONTROL_END")!=std::string::npos?"PASS":"FAIL"));
    }
    else if(kind=="tty-check") {
        for(const std::string command:{"linux-tty-check","glibc-tty-check"}){
            const auto session=Start(root,80,24,"shell");bool sent=false,signals=false,stopped=false,interrupted=false,finished=false,ready=false;std::string transcript;
            const std::string begin=command+"\n";Write(session.id,reinterpret_cast<const uint8_t*>(begin.data()),begin.size());
            try {
                for(int attempt=0;attempt<1000;attempt++){
                    auto data=Read(session.id);if(!data.bytes.empty()){const std::string text(data.bytes.begin(),data.bytes.end());transcript+=text;out.Add(text);}
                    if(!sent&&transcript.find("TTY_RAW_READY")!=std::string::npos){
                        out.Add(std::string("TTY native metadata raw=")+(data.rawInput?"true":"false")+" echo="+(data.echoInput?"true":"false"));
                        if(!data.rawInput||data.echoInput||data.convertEol)out.Add("TTY_ASSERT_METADATA_FAIL");
                        Resize(session.id,132,43);const uint8_t raw[]={'a',27,'[','A',3};Write(session.id,raw,sizeof(raw));sent=true;
                    }
                    if(!signals&&transcript.find("TTY_SIGNAL_READY")!=std::string::npos){const uint8_t keys[]={2,28};Write(session.id,keys,sizeof(keys));signals=true;}
                    if(!stopped&&transcript.find("TTY_JOB_READY")!=std::string::npos){const uint8_t key=26;Write(session.id,&key,1);stopped=true;}
                    if(!interrupted&&transcript.find("TTY_JOB_CONTINUED")!=std::string::npos){const uint8_t key=2;Write(session.id,&key,1);interrupted=true;}
                    if(!finished&&transcript.find("TTY_RESTORED")!=std::string::npos&&!data.rawInput){
                        const std::string end="printf 'TTY_SHELL_READY\\n'\nexit\n";Write(session.id,reinterpret_cast<const uint8_t*>(end.data()),end.size());finished=true;
                    }
                    if(transcript.find("TTY_SHELL_READY")!=std::string::npos)ready=true;
                    if(!data.running||transcript.find("TTY_ASSERT")!=std::string::npos)break;
                    if(attempt==999)out.Add("TTY TIMEOUT");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }catch(...){Close(session.id);throw;}
            Close(session.id);
            out.Add("TTY command="+command+" result="+(sent&&signals&&stopped&&interrupted&&finished&&ready&&transcript.find("TTY_FOREGROUND_STOP_CONTINUE_INTERRUPT_OK")!=std::string::npos&&transcript.find("TTY_ASSERT")==std::string::npos?"PASS":"FAIL"));
        }
    }
    else if (kind == "exec-check" || kind == "combined-notify-check" || kind == "combined-check" || kind == "ptrace-prepare" || kind == "proot-check" || kind == "shell-check" || kind == "hybrid-check" || kind == "workflow-check" || kind == "network-check" || kind == "compiler-check" || kind == "jit-check" || kind == "bridge-check" || kind == "process-check") {
        const auto session = Start(root, 80, 24, "shell");
        out.Add("shell pid=" + std::to_string(session.pid) + " pty=" + std::to_string(session.pty) + " ptyError=" + std::to_string(session.ptyError));
        const std::string script = kind == "exec-check" ? HiSHExecProbeScript : kind == "combined-notify-check" ? R"SH(printf 'COMBINED_NOTIFY_BEGIN\n'
libs=$(hish-tools --path tcc)
libs="${libs%/*}"
d="$HOME/.hish-combined-notify"
mkdir -p "$d" || exit 211
cp "$libs/libhish_probe_bare.so" "$d/bare" && chmod 700 "$d/bare" || exit 212
hish-run --diagnostic --notify "$d/bare"
status=$?
printf 'COMBINED_NOTIFY_STATUS=%s\n' "$status"
if [ "$status" != 42 ]; then printf 'COMBINED_NOTIFY_BLOCKED_KEEP_INPUT\n'; exit 213; fi
rm -f "$d/bare" && rmdir "$d" || exit 214
printf 'COMBINED_NOTIFY_OK\nCOMBINED_NOTIFY_END\n'
exit 0
)SH" : kind == "combined-check" ? R"SH(printf 'COMBINED_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
d="$HOME/.hish-combined-evaluation"
mkdir -p "$d" || exit 201
cat > "$d/program.c" <<'SOURCE'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int initialized=17, bss[64], constructed;
static int *pointer=&initialized;
__attribute__((constructor)) static void initialize(void) { constructed=31; }
int main(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],"argument-37") || *pointer!=17 || bss[63] || constructed!=31) return 81;
    if(!getenv("PREFIX") || !getenv("HOME") || !getenv("HISH_COMBINED_TEST") || strcmp(getenv("HISH_COMBINED_TEST"),"preserved")) return 82;
    FILE *status=fopen("/proc/self/status","r"); if(!status) return 83;
    char line[256]; int detached=0;
    while(fgets(line,sizeof(line),status)) if(!strncmp(line,"TracerPid:",10)) detached=atoi(line+10)==0;
    fclose(status); if(!detached) return 84;
    FILE *data=fopen("native-output.txt","w"); if(!data) return 85;
    fputs("PREFIX_LIBC_PATH_DATA_BSS_CTOR_OK\n",data); if(fclose(data)) return 86;
    puts("COMBINED_NEW_ELF_NATIVE_OK"); return 0;
}
SOURCE
cd "$d" || exit 204
tcc "$d/program.c" -o "$d/default-address" || exit 202
chmod 700 "$d/default-address" || exit 203
export HISH_COMBINED_TEST=preserved
hish-run --diagnostic "$d/default-address" argument-37
printf 'COMBINED_DEFAULT_MRSH_STATUS=%s\n' "$?"
sh -c 'hish-run --diagnostic "$HOME/.hish-combined-evaluation/default-address" argument-37'
printf 'COMBINED_DEFAULT_BRIDGE_STATUS=%s\n' "$?"
tcc -Wl,-Ttext=60000000 "$d/program.c" -o "$d/program" || exit 202
chmod 700 "$d/program" || exit 203
printf 'COMBINED_PHONE_ELF_READY\n'
cd "$d" || exit 204
export HISH_COMBINED_TEST=preserved
hish-run --diagnostic ./program argument-37
status=$?
printf 'COMBINED_MRSH_STATUS=%s\n' "$status"
if [ "$status" != 0 ]; then printf 'COMBINED_BLOCKED_KEEP_INPUTS\n'; exit 205; fi
grep -q PREFIX_LIBC_PATH_DATA_BSS_CTOR_OK native-output.txt || exit 206
printf 'COMBINED_PREFIX_LIBC_OK\n'
sh -c 'hish-run --diagnostic ./program argument-37' || exit 207
printf 'COMBINED_LINUX_EXEC_BRIDGE_OK\n'
libs=$(hish-tools --path tcc)
libs="${libs%/*}"
cp "$libs/libhish_probe_bare.so" bare && chmod 700 bare || exit 210
hish-run --diagnostic ./bare
if [ "$?" != 42 ]; then printf 'COMBINED_ASSERT_STATIC_FAIL\n'; exit 211; fi
printf 'COMBINED_STATIC_PIE_OK\n'
cp "$(hish-tools --path linux-musl-check)" musl && chmod 700 musl || exit 212
hish-run --diagnostic ./musl
if [ "$?" != 42 ]; then printf 'COMBINED_ASSERT_MUSL_FAIL\n'; exit 213; fi
printf 'COMBINED_MUSL_TLS_THREADS_OK\n'
cp "$(hish-tools --path linux-glibc-check)" glibc && chmod 700 glibc || exit 214
hish-run --diagnostic ./glibc
if [ "$?" != 42 ]; then printf 'COMBINED_ASSERT_GLIBC_FAIL\n'; exit 215; fi
printf 'COMBINED_GLIBC_TLS_THREADS_OK\n'
printf 'not an ELF\n' > malformed
chmod 700 malformed
hish-run ./malformed
if [ "$?" = 0 ]; then printf 'COMBINED_ASSERT_MALFORMED_FAIL\n'; exit 208; fi
printf 'COMBINED_MALFORMED_REJECTED_OK\n'
rm -f program.c program native-output.txt malformed default-address bare musl glibc
cd "$HOME" && rmdir "$d" || exit 209
printf 'COMBINED_CLEANUP_OK\nCOMBINED_END\n'
exit 0
)SH" : kind == "ptrace-prepare" ? R"SH(d="$HOME/.hish-proot-evaluation"
mkdir -p "$d" || exit 201
cat > "$d/function.c" <<'SOURCE'
int hish_eval(int n) { int value=17; for(int i=1;i<=n;i++) value+=i; return value; }
SOURCE
tcc -c "$d/function.c" -o "$d/function.o" || exit 202
printf 'PTRACE_PHONE_OBJECT_READY\nPTRACE_PREPARE_END\n'
exit 0
)SH" : kind == "proot-check" ? R"SH(printf 'PROOT_EVAL_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
d="$HOME/.hish-proot-evaluation"
mkdir -p "$d" || exit 201
cat > "$d/function.c" <<'SOURCE'
int hish_eval(int n) { int value=17; for(int i=1;i<=n;i++) value+=i; return value; }
SOURCE
tcc -c "$d/function.c" -o "$d/function.o" || exit 202
printf 'PROOT_PHONE_OBJECT_READY\n'
cat > "$d/program.c" <<'SOURCE'
#include <stdio.h>
int main(void) { puts("PROOT_NEW_ELF_RAN"); return 0; }
SOURCE
tcc "$d/program.c" -o "$d/program" || exit 203
printf 'PROOT_PHONE_ELF_READY\n'
proot=$(hish-tools --path tcc)
proot="${proot%/*}/libhish_eval_proot.so"
libs="${proot%/*}"
"$proot" --version
printf 'PROOT_VERSION_STATUS=%s\n' "$?"
export PROOT_NO_SECCOMP=1
export PROOT_TMP_DIR="$d"
"$proot" -v 1 -r / -w "$HOME" /system/bin/toybox echo PROOT_SYSTEM_EXEC_OK
printf 'PROOT_SYSTEM_STATUS=%s\n' "$?"
cp "$libs/libhish_linux_musl_static.so" "$d/static-control" || exit 204
chmod 700 "$d/static-control"
"$proot" -v 1 -r / -w "$HOME" "$d/static-control" --echo PROOT_STATIC_EXEC_OK
printf 'PROOT_STATIC_STATUS=%s\n' "$?"
"$proot" -v 1 -r / -w "$HOME" -b "$libs/libhish_linux_musl_loader.so:/lib/ld-musl-aarch64.so.1" "$d/program"
printf 'PROOT_NEW_ELF_STATUS=%s\n' "$?"
printf 'PROOT_EVAL_END\n'
exit 0
)SH" : kind == "process-check" ? R"SH(printf 'PROCESS_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
linux-musl-check --process-extensions linux-glibc-check
status=$?
printf 'PROCESS_MUSL_STATUS=%s\n' "$status"
test "$status" = 0 || exit 180
linux-glibc-check --process-extensions linux-musl-check
status=$?
printf 'PROCESS_GLIBC_STATUS=%s\n' "$status"
test "$status" = 0 || exit 181
linux-musl-check --exec-search linux-glibc-check --self-exec || exit 191
linux-glibc-check --exec-search linux-musl-check --self-exec || exit 192
printf 'PROCESS_LOGICAL_SELF_EXEC_OK\n'
d=$(busybox mktemp -d "$HOME/.hish-script.XXXXXX") || exit 193
trap 'rm -rf "$d"' EXIT
printf '#!/bin/sh -e\nfalse\nprintf "UNEXPECTED_SCRIPT_FLAG"\n' > "$d/strict"
printf '#!/usr/bin/env dash\nprintf "SCRIPT_ENV_%%s" "$1"\n' > "$d/env"
chmod 700 "$d/strict" "$d/env"
"$d/strict"
test "$?" = 1 || exit 194
linux-musl-check --exec "$d/strict"
test "$?" = 1 || exit 195
linux-glibc-check --exec "$d/strict"
test "$?" = 1 || exit 196
test "$("$d/env" NATIVE)" = SCRIPT_ENV_NATIVE || exit 197
test "$(linux-musl-check --exec "$d/env" MUSL)" = SCRIPT_ENV_MUSL || exit 198
test "$(linux-glibc-check --exec "$d/env" GLIBC)" = SCRIPT_ENV_GLIBC || exit 199
printf 'PROCESS_SHEBANG_FLAGS_ENV_OK\n'
printf 'PROCESS_END\n'
exit
)SH" : kind == "bridge-check" ? R"SH(printf 'BRIDGE_BEGIN\n'
export HISH_KERNEL_EXEC_BRIDGE=1 HISH_ELF_DIAGNOSTICS=1
linux-musl-check --exec-search linux-glibc-check --echo BRIDGE_BASIC_OK
printf 'BRIDGE_BASIC_STATUS=%s\n' "$?"
linux-musl-check --bridge-contract linux-glibc-check
status=$?
printf 'BRIDGE_MUSL_STATUS=%s\n' "$status"
test "$status" = 0 || exit 161
linux-glibc-check --bridge-contract linux-musl-check
status=$?
printf 'BRIDGE_GLIBC_STATUS=%s\n' "$status"
test "$status" = 0 || exit 162
printf 'BRIDGE_THREADS_MAPS_OK\n'
p=$(hish-tools --path linux-glibc-check)
linux-musl-check --exec-contract "$p" || exit 163
p=$(hish-tools --path linux-musl-check)
linux-glibc-check --exec-contract "$p" || exit 164
printf 'BRIDGE_EXEC_CONTRACT_OK\n'
p=$(hish-tools --path linux-glibc-check)
test "$(linux-musl-check --exec-stdio "$p")" = LINUX_EXEC_STDIO_CLOEXEC_OK || exit 170
p=$(hish-tools --path linux-musl-check)
test "$(linux-glibc-check --exec-stdio "$p")" = LINUX_EXEC_STDIO_CLOEXEC_OK || exit 171
printf 'BRIDGE_STDIO_CLOEXEC_OK\n'
linux-musl-check --spawn-files linux-glibc-check || exit 165
linux-glibc-check --spawn-files linux-musl-check || exit 166
printf 'BRIDGE_SPAWN_FILES_OK\n'
for method in fexecve empty relative; do
  p=$(hish-tools --path linux-glibc-check)
  test "$(linux-musl-check --fd-exec "$method" "$p")" = LINUX_FD_EXEC_OK || exit 167
  p=$(hish-tools --path linux-musl-check)
  test "$(linux-glibc-check --fd-exec "$method" "$p")" = LINUX_FD_EXEC_OK || exit 168
done
printf 'BRIDGE_FD_EXEC_OK\nBRIDGE_END\n'
exit 0
)SH" : kind == "compiler-check" ? R"SH(printf 'COMPILER_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
tcc -v || exit 150
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -print-search-dirs
d=$(busybox mktemp -d "$HOME/.hish-compile.XXXXXX") || exit 151
trap 'rm -rf "$d"' EXIT
cat > "$d/main.c" <<'SOURCE'
#include <stdio.h>
int main(void) { puts("COMPILED_ON_PHONE_OK"); return 23; }
SOURCE
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -c "$d/main.c" -o "$d/main.o" || exit 152
test -s "$d/main.o" || exit 153
printf 'COMPILER_OBJECT_OK\n'
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" "$d/main.c" -o "$d/a.out" || exit 154
test -s "$d/a.out" || exit 155
printf 'COMPILER_LINKED_ELF_OK\n'
busybox od -An -tx1 -N64 "$d/a.out"
busybox sha256sum "$d/a.out"
linux-musl-check --execution-policy "$d/a.out" || exit 156
linux-musl-check --exec-search linux-musl-check --execution-policy "$d/a.out" || exit 169
"$d/a.out"
status=$?
test "$status" = 23 || exit 157
printf 'COMPILER_EXECUTED_OK\n'
printf 'int _start(void) { return 23; }\n' > "$d/memory.c"
tcc -nostdlib -run "$d/memory.c"
status=$?
printf 'COMPILER_MEMORY_RUN_STATUS=%s\n' "$status"
if test "$status" = 23; then printf 'COMPILER_MEMORY_EXECUTED_OK\n'; else printf 'COMPILER_MEMORY_NOT_EXECUTED\n'; fi
printf 'COMPILER_END\n'
exit 0
)SH" : kind == "jit-check" ? R"SH(printf 'JIT_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
d=$(busybox mktemp -d "$HOME/.hish-jit.XXXXXX") || exit 180
trap 'rm -rf "$d"' EXIT
printf 'int _start(void) { return 23; }\n' > "$d/bare.c"
tcc -nostdlib -run "$d/bare.c"
status=$?
printf 'JIT_BARE_RUN_STATUS=%s\n' "$status"
test "$status" = 23 || exit 180
cat > "$d/hello.c" <<'SOURCE'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int counter = 5;
int main(int argc, char **argv) {
  char *p = malloc(32);
  if (!p) return 80;
  strcpy(p, "heap");
  printf("JIT_TCC_RUN_OK argc=%d arg1=%s %s counter=%d\n", argc, argc > 1 ? argv[1] : "-", p, counter + 1);
  free(p);
  return 23;
}
SOURCE
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -run "$d/hello.c" phone-arg
printf 'JIT_TCC_RUN_STATUS=%s\n' "$?"
libs=$(hish-tools --path tcc)
libs="${libs%/*}"
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -nostdlib "$libs/libhish_linux_musl_loader.so" "$HISH_TOOLCHAIN_ROOT/lib/libtcc1.a" -run "$d/hello.c" phone-arg
printf 'JIT_TCC_BUNDLE_LIBC_RUN_STATUS=%s\n' "$?"
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -nostdlib "$HISH_TOOLCHAIN_ROOT/lib/libtcc1.a" -run "$d/hello.c" phone-arg
printf 'JIT_TCC_PROCESS_LIBC_RUN_STATUS=%s\n' "$?"
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -static -run "$d/hello.c" phone-arg
printf 'JIT_TCC_STATIC_LIBC_RUN_STATUS=%s\n' "$?"
mkdir "$d/host-libc" || exit 181
printf 'INPUT ( )\n' > "$d/host-libc/libc.so"
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -L"$d/host-libc" -run "$d/hello.c" phone-arg
status=$?
printf 'JIT_TCC_HOST_LIBC_RUN_STATUS=%s\n' "$status"
test "$status" = 23 || exit 182
cat > "$d/helper.c" <<'SOURCE'
int helper(int value) { return value + 2; }
SOURCE
cat > "$d/multi.c" <<'SOURCE'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
extern int helper(int);
static void *worker(void *arg) {
  int *value = arg;
  *value = helper(40);
  errno = 7;
  return errno == 7 ? arg : NULL;
}
int main(int argc, char **argv) {
  if (argc != 3 || !getenv("PREFIX")) return 81;
  char *buffer = calloc(1, 16);
  if (!buffer) return 82;
  buffer = realloc(buffer, 128);
  if (!buffer) return 83;
  int value = 0;
  void *result = NULL;
  pthread_t thread;
  errno = 9;
  if (pthread_create(&thread, NULL, worker, &value)) return 84;
  if (pthread_join(thread, &result) || result != &value || value != 42 || errno != 9) return 85;
  snprintf(buffer, 128, "multi-%s-%d", argv[1], value);
  FILE *file = fopen(argv[2], "w");
  if (!file || fputs(buffer, file) < 0 || fclose(file)) return 86;
  char readback[128] = {0};
  file = fopen(argv[2], "r");
  if (!file || !fgets(readback, sizeof readback, file) || fclose(file)) return 87;
  if (strcmp(buffer, readback) || remove(argv[2])) return 88;
  printf("JIT_MULTI_OK arg=%s value=%d heap=OK file=OK thread=OK errno=OK\n", argv[1], value);
  free(buffer);
  return 23;
}
SOURCE
tcc -c "$d/helper.c" -o "$d/helper.o" || exit 183
for arg in first second third; do
  tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -L"$d/host-libc" "$d/helper.o" -run "$d/multi.c" "$arg" "$d/result.txt"
  status=$?
  printf 'JIT_MULTI_STATUS=%s\n' "$status"
  test "$status" = 23 || exit 184
done
printf '#include <stdlib.h>\nint main(void) { exit(23); }\n' > "$d/exit.c"
tcc -B"$HISH_TOOLCHAIN_ROOT/lib" -L"$d/host-libc" -run "$d/exit.c"
status=$?
printf 'JIT_EXIT_STATUS=%s\n' "$status"
test "$status" = 23 || exit 185
printf 'JIT_HOST_LIBC_VERIFIED_OK\n'
printf 'JIT_END\n'
exit 0
)SH" : kind == "network-check" ? R"SH(printf 'NETWORK_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
curl --version || exit 130
d=$(busybox mktemp -d "$HOME/.hish-network.XXXXXX") || exit 131
trap 'rm -rf "$d"' EXIT
curl --fail --silent --show-error --connect-timeout 10 --max-time 25 https://example.com/ -o "$d/page"
result=$?
printf 'NETWORK_CURL_STATUS=%s\n' "$result"
if test "$result" = 0; then
  grep 'Example Domain' "$d/page" >/dev/null || exit 132
  printf 'NETWORK_HTTPS_VERIFIED_OK\n'
fi
printf '' > "$d/invalid-ca"
curl --cacert "$d/invalid-ca" --silent --show-error --connect-timeout 10 --max-time 20 https://example.com/ -o /dev/null
test "$?" = 77 || exit 137
printf 'NETWORK_INVALID_CA_REJECTED_OK\n'
git -c http.connectTimeout=10 -c http.lowSpeedLimit=1 -c http.lowSpeedTime=20 ls-remote https://gitee.com/openharmony/third_party_musl.git HEAD > "$d/refs"
status=$?
printf 'NETWORK_GIT_STATUS=%s\n' "$status"
if test "$status" = 0; then
  grep 'HEAD' "$d/refs" || exit 133
  printf 'NETWORK_GIT_REMOTE_OK\n'
  git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=20 clone --depth=1 --single-branch --no-tags --quiet https://gitee.com/openharmony/third_party_musl.git "$d/clone" || exit 141
  test "$(git -C "$d/clone" rev-parse HEAD)" = "$(cut -f1 "$d/refs")" || exit 142
  test -z "$(git -C "$d/clone" status --porcelain)" || exit 143
  printf 'NETWORK_GIT_CLONE_OK\n'
fi
rm -r "$d"
printf 'NETWORK_END\n'
exit 0
)SH" : kind == "workflow-check" ? R"SH(printf 'WORKFLOW_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
curl --version || exit 121
git --version || exit 122
test "$(dash -c 'curl --version >/dev/null && git --version')" = 'git version 2.52.0' || exit 144
printf 'WORKFLOW_LINUX_TOOL_EXEC_OK\n'
d=$(busybox mktemp -d "$HOME/.hish-workflow.XXXXXX") || exit 123
trap 'rm -rf "$d"' EXIT
git -C "$d" init -q || exit 124
printf '#!/bin/sh\nprintf "NATIVE_PROJECT_OK\\n"\n' > "$d/main.sh"
chmod 700 "$d/main.sh"
git -C "$d" add main.sh || exit 125
git -C "$d" -c user.name=HiSH-Test -c user.email=test@localhost commit -qm 'native initial' || exit 126
test "$(git -C "$d" log -1 --format=%s)" = 'native initial' || exit 127
printf 'WORKFLOW_GIT_COMMIT_OK\n'
git clone --no-local --quiet "$d" "$d/clone" || exit 138
test "$(git -C "$d/clone" log -1 --format=%s)" = 'native initial' || exit 139
printf 'WORKFLOW_GIT_CLONE_OK\n'
test "$(dash "$d/main.sh")" = NATIVE_PROJECT_OK || exit 128
printf 'WORKFLOW_SCRIPT_OK\n'
printf '# edited in private HOME\n' >> "$d/main.sh"
git -C "$d" diff --no-ext-diff --exit-code >/dev/null
test "$?" = 1 || exit 129
git -C "$d" diff --no-ext-diff | grep 'edited in private HOME' || exit 134
printf 'WORKFLOW_GIT_DIFF_OK\n'
sh -c 'kill -TERM $$'
test "$?" = 143 || exit 140
printf 'WORKFLOW_SIGNAL_STATUS_OK\n'
git -C "$d" fsck --no-dangling || exit 135
cp "$(hish-tools --path git)" "$d/home-git" && chmod 700 "$d/home-git" || exit 145
test "$("$d/home-git" --version)" = 'git version 2.52.0' || exit 146
printf 'WORKFLOW_HOME_ELF_EXECUTED_OK\n'
chmod 600 "$d/home-git" || exit 145
"$d/home-git" --version
test "$?" = 126 || exit 146
printf 'WORKFLOW_NONEXEC_REJECTED_OK\n'
rm -r "$d"
test ! -e "$d" || exit 136
printf 'WORKFLOW_CLEANUP_OK\nWORKFLOW_END\n'
exit 0
)SH" : kind == "hybrid-check" ? R"SH(printf 'HYBRID_BEGIN\n'
hish-tools
catalog=$(hish-tools)
for required in busybox dash gnu-uname linux-tty-check; do
  case "$catalog" in *"$required"*) ;; *) printf 'ASSERT_TOOL_CATALOG_FAIL\n'; exit 120 ;; esac
done
printf 'HYBRID_TOOL_CATALOG_OK\n'
linux-musl-check
test "$?" = 42 || { printf 'ASSERT_LINUX_MUSL_FAIL\n'; exit 71; }
linux-musl-static-check
test "$?" = 42 || { printf 'ASSERT_LINUX_STATIC_FAIL\n'; exit 72; }
linux-glibc-check
test "$?" = 42 || { printf 'ASSERT_GLIBC_DYNAMIC_FAIL\n'; exit 81; }
linux-glibc-static-check
test "$?" = 42 || { printf 'ASSERT_GLIBC_STATIC_FAIL\n'; exit 82; }
printf 'HYBRID_GLIBC_OK\n'
v=$(busybox echo ALPINE_NATIVE_OK)
test "$v" = ALPINE_NATIVE_OK || { printf 'ASSERT_BUSYBOX_FAIL\n'; exit 83; }
v=$(dash -c 'n=0; for x in a b c; do n=$((n + 1)); done; printf "DASH_%s" "$n"')
test "$v" = DASH_3 || { printf 'ASSERT_DASH_FAIL\n'; exit 84; }
v=$(gnu-printf '%s_%d' GNU 42)
test "$v" = GNU_42 || { printf 'ASSERT_GNU_PRINTF_FAIL\n'; exit 85; }
test "$(gnu-uname -m)" = aarch64 || { printf 'ASSERT_GNU_UNAME_FAIL\n'; exit 86; }
printf 'HYBRID_DISTRO_TOOLS_OK\n'
v=$(dash -c 'linux-musl-check --echo DASH_TO_MUSL')
test "$v" = DASH_TO_MUSL || { printf 'ASSERT_DASH_EXTERNAL_FAIL\n'; exit 89; }
v=$(busybox sh -c 'linux-glibc-check --echo MUSL_TO_GLIBC')
test "$v" = MUSL_TO_GLIBC || { printf 'ASSERT_ASH_EXTERNAL_FAIL\n'; exit 90; }
v=$(linux-musl-check --exec-search linux-glibc-check --exec-search linux-musl-check --echo CROSS_LIBC_EXEC)
test "$v" = CROSS_LIBC_EXEC || { printf 'ASSERT_CROSS_LIBC_EXEC_FAIL\n'; exit 91; }
v=$(linux-glibc-check --exec-search gnu-printf '%s_%d' EXEC 42)
test "$v" = EXEC_42 || { printf 'ASSERT_GLIBC_EXEC_FAIL\n'; exit 92; }
dash -c 'linux-musl-check --exit 27'
test "$?" = 27 || { printf 'ASSERT_LINUX_CHILD_STATUS_FAIL\n'; exit 93; }
printf 'HYBRID_LINUX_CHILD_EXEC_OK\n'
linux-musl-check --exec-search linux-glibc-check
test "$?" = 42 || { printf 'ASSERT_GLIBC_REEXEC_TLS_FAIL\n'; exit 95; }
linux-glibc-check --exec-search linux-musl-check
test "$?" = 42 || { printf 'ASSERT_MUSL_REEXEC_TLS_FAIL\n'; exit 96; }
p=$(hish-tools --path linux-glibc-check)
linux-musl-check --exec-contract "$p"
test "$?" = 0 || { printf 'ASSERT_GLIBC_EXEC_CONTRACT_FAIL\n'; exit 97; }
linux-glibc-check --threaded-exec "$p"
test "$?" = 0 || { printf 'ASSERT_GLIBC_THREADED_EXEC_FAIL\n'; exit 98; }
p=$(hish-tools --path linux-musl-check)
linux-glibc-check --exec-contract "$p"
test "$?" = 0 || { printf 'ASSERT_MUSL_EXEC_CONTRACT_FAIL\n'; exit 99; }
linux-musl-check --threaded-exec "$p"
test "$?" = 0 || { printf 'ASSERT_MUSL_THREADED_EXEC_FAIL\n'; exit 100; }
printf 'HYBRID_EXEC_CONTRACT_OK\n'
v=$(dash -c '/system/bin/toybox uname -m')
test "$v" = aarch64 || { printf 'ASSERT_GLIBC_SYSTEM_EXEC_FAIL\n'; exit 101; }
v=$(busybox sh -c '/system/bin/toybox uname -m')
test "$v" = aarch64 || { printf 'ASSERT_MUSL_SYSTEM_EXEC_FAIL\n'; exit 102; }
busybox sh -c 'sleep 0.01'
test "$?" = 0 || { printf 'ASSERT_ASH_SLEEP_FAIL\n'; exit 103; }
dash -c 'sleep 0.01'
test "$?" = 0 || { printf 'ASSERT_DASH_SLEEP_FAIL\n'; exit 104; }
linux-musl-check --exec-search ohos-check
test "$?" = 42 || { printf 'ASSERT_LINUX_OHOS_EXEC_FAIL\n'; exit 105; }
printf 'HYBRID_LINUX_SYSTEM_EXEC_OK\n'
test "$(linux-musl-check --spawn linux-glibc-check --echo SPAWN_GLIBC)" = SPAWN_GLIBC || { printf 'ASSERT_SPAWN_GLIBC_FAIL\n'; exit 106; }
test "$(linux-glibc-check --spawn linux-musl-check --echo SPAWN_MUSL)" = SPAWN_MUSL || { printf 'ASSERT_SPAWN_MUSL_FAIL\n'; exit 107; }
linux-musl-check --spawn-contract linux-glibc-check
test "$?" = 0 || { printf 'ASSERT_SPAWN_GLIBC_ATTR_FAIL\n'; exit 108; }
linux-glibc-check --spawn-contract linux-musl-check
test "$?" = 0 || { printf 'ASSERT_SPAWN_MUSL_ATTR_FAIL\n'; exit 109; }
linux-musl-check --spawn-negative
test "$?" = 0 || { printf 'ASSERT_SPAWN_MUSL_ERRORS_FAIL\n'; exit 110; }
linux-glibc-check --spawn-negative
test "$?" = 0 || { printf 'ASSERT_SPAWN_GLIBC_ERRORS_FAIL\n'; exit 111; }
test "$(linux-musl-check --system 'linux-glibc-check --echo SYSTEM_GLIBC')" = SYSTEM_GLIBC || { printf 'ASSERT_SYSTEM_GLIBC_FAIL\n'; exit 112; }
test "$(linux-glibc-check --system 'linux-musl-check --echo SYSTEM_MUSL')" = SYSTEM_MUSL || { printf 'ASSERT_SYSTEM_MUSL_FAIL\n'; exit 113; }
linux-glibc-check --system 'exit 23'
test "$?" = 23 || { printf 'ASSERT_SYSTEM_STATUS_FAIL\n'; exit 114; }
p=$(hish-tools --path linux-glibc-check)
test "$(linux-musl-check --execl "$p")" = LINUX_EXECL_OK || { printf 'ASSERT_EXECL_FAIL\n'; exit 115; }
printf 'HYBRID_SPAWN_SYSTEM_OK\n'
linux-musl-check --spawn-files linux-glibc-check
test "$?" = 0 || { printf 'ASSERT_SPAWN_MUSL_FILES_FAIL\n'; exit 147; }
linux-glibc-check --spawn-files linux-musl-check
test "$?" = 0 || { printf 'ASSERT_SPAWN_GLIBC_FILES_FAIL\n'; exit 148; }
printf 'HYBRID_SPAWN_FILES_OK\n'
for method in fexecve empty relative; do
  p=$(hish-tools --path linux-glibc-check)
  test "$(linux-musl-check --fd-exec "$method" "$p")" = LINUX_FD_EXEC_OK || { printf 'ASSERT_MUSL_FD_EXEC_FAIL\n'; exit 158; }
  p=$(hish-tools --path linux-musl-check)
  test "$(linux-glibc-check --fd-exec "$method" "$p")" = LINUX_FD_EXEC_OK || { printf 'ASSERT_GLIBC_FD_EXEC_FAIL\n'; exit 159; }
done
printf 'HYBRID_FD_EXEC_OK\n'
d=".hish-bad-elf-$$"
mkdir "$d"
for invalid in overflow align phdr rwx; do
  f="$d/$invalid"
  printf 'MALFORMED_PREPARE type=%s path=%s\n' "$invalid" "$f"
  linux-musl-check --make-bad-elf "$invalid" "$f"
  status=$?
  test "$status" = 0 || { printf 'ASSERT_MALFORMED_PREPARE_FAIL status=%s\n' "$status"; exit 116; }
  "$f" 2>/dev/null
  test "$?" = 126 || { printf 'ASSERT_MALFORMED_OHOS_FAIL\n'; exit 117; }
  linux-musl-check --reject-elf "$invalid" "$f"
  test "$?" = 0 || { printf 'ASSERT_MALFORMED_MUSL_FAIL\n'; exit 118; }
  linux-glibc-check --reject-elf "$invalid" "$f"
  test "$?" = 0 || { printf 'ASSERT_MALFORMED_GLIBC_FAIL\n'; exit 119; }
done
rm -r "$d"
printf 'HYBRID_MALFORMED_ELF_OK\n'
p=$(hish-tools --path linux-musl-check)
v=$("$p" --echo absolute-path)
test "$v" = absolute-path || { printf 'ASSERT_ABSOLUTE_ELF_FAIL\n'; exit 87; }
printf '#!/bin/sh\nprintf "SCRIPT_%%s" "$1"\n' > .hish-script-check
chmod 700 .hish-script-check
test "$(./.hish-script-check OK)" = SCRIPT_OK || { printf 'ASSERT_HOME_SCRIPT_FAIL\n'; exit 88; }
test "$(dash -c './.hish-script-check LINUX')" = SCRIPT_LINUX || { printf 'ASSERT_LINUX_HOME_SCRIPT_FAIL\n'; exit 94; }
rm .hish-script-check
printf 'HYBRID_ABSOLUTE_ELF_SCRIPT_OK\n'
v=$(linux-musl-check --echo 'native pipeline' | grep pipeline)
test "$v" = 'native pipeline' || { printf 'ASSERT_HYBRID_PIPE_FAIL\n'; exit 73; }
printf 'HYBRID_PIPE_SUBSTITUTION_OK\n'
linux-musl-check --exit 23
test "$?" = 23 || { printf 'ASSERT_HYBRID_EXIT_FAIL\n'; exit 74; }
printf 'HYBRID_EXIT_STATUS_OK\n'
d=".hish-hybrid-$$"
mkdir "$d"
linux-musl-check --echo 'native redirection' > "$d/output"
test "$(cat "$d/output")" = 'native redirection' || { printf 'ASSERT_HYBRID_REDIR_FAIL\n'; exit 75; }
rm -r "$d"
printf 'HYBRID_REDIRECTION_CLEANUP_OK\n'
linux-musl-check --exit 0 &
wait $!
test "$?" = 0 || { printf 'ASSERT_HYBRID_WAIT_FAIL\n'; exit 76; }
printf 'HYBRID_BACKGROUND_WAIT_OK\n'
v=$(/system/bin/toybox uname -m)
test "$v" = aarch64 || { printf 'ASSERT_HYBRID_SYSTEM_FAIL\n'; exit 77; }
printf 'HYBRID_SYSTEM_EXEC_OK\n'
sh -c 'exec linux-musl-check --exit 19'
test "$?" = 19 || { printf 'ASSERT_HYBRID_EXEC_FAIL\n'; exit 78; }
printf 'HYBRID_EXEC_BUILTIN_OK\nHYBRID_END\n'
exit 0
)SH" : R"SH(printf 'SHELL_NATIVE_BEGIN pid=%s HOME=%s\n' "$$" "$HOME"
pwd
uname -m
case "$HOME" in /data/storage/el2/base/files/native/home) ;; *) exit 31 ;; esac
d=".hish-check-$$"
mkdir "$d"
printf 'alpha\nbeta\n' > "$d/input"
v=$(cat "$d/input" | grep beta)
test "$v" = beta || { printf 'ASSERT_PIPE_FAIL\n'; exit 41; }
printf 'PIPE_AND_SUBSTITUTION_OK\n'
f() { printf 'FUNCTION_%s\n' "$1"; }
f OK
n=0
for word in a b c; do n=$((n + 1)); done
test "$n" = 3 || { printf 'ASSERT_LOOP_FAIL\n'; exit 42; }
printf 'LOOP_AND_ARITHMETIC_OK\n'
sum=0
for item in 1 2 3; do
  sum=$((sum + item))
done
if test "$sum" = 6; then
  printf 'MULTILINE_FOR_IF_OK\n'
else
  printf 'ASSERT_MULTILINE_FOR_FAIL\n'
  exit 44
fi
multi() {
  printf 'MULTILINE_FUNCTION_OK'
}
test "$(multi)" = MULTILINE_FUNCTION_OK || { printf 'ASSERT_MULTILINE_FUNCTION_FAIL\n'; exit 45; }
cat <<'HISH_EOF' > "$d/heredoc"
literal $HOME
HISH_EOF
test "$(cat "$d/heredoc")" = 'literal $HOME' || { printf 'ASSERT_HEREDOC_FAIL\n'; exit 46; }
printf 'SHELL_MULTILINE_OK\n'
(cd "$d"; pwd; printf 'SUBSHELL_OK\n')
sh -c 'printf "NESTED_SHELL_OK\n"'
sleep 0.05 &
wait $!
printf 'BACKGROUND_WAIT_OK\n'
printf '中文字节正常\n' | cat > "$d/utf8"
cat "$d/utf8"
v=$(cat "$d/utf8")
test "$v" = '中文字节正常' || { printf 'ASSERT_UTF8_FAIL\n'; exit 43; }
rm -r "$d"
test ! -e "$d"
printf 'STORAGE_CLEANUP_OK\nSHELL_NATIVE_END\n'
exit 0
)SH";
        size_t offset = 0;
        try {
            const int limit = kind == "network-check" ? 12000 : kind == "exec-check" || kind == "combined-check" || kind == "combined-notify-check" ? 7000 : 2000;
            for (int n = 0; n < limit; ++n) {
                if (offset < script.size()) { offset += Write(session.id, reinterpret_cast<const uint8_t*>(script.data()) + offset, script.size() - offset); }
                auto data = Read(session.id);
                if (!data.bytes.empty()) { out.Add(std::string(data.bytes.begin(), data.bytes.end())); }
                if (!data.running) { out.Add("shell EOF"); break; }
                if (n == limit - 1) { out.Add("shell TIMEOUT"); }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (...) { Close(session.id); throw; }
        Close(session.id);
    }
    else if (kind == "baseline") { Baseline(out, root); }
    else if (kind == "pty") { Pty(out); }
    else if (kind == "prepare") {
        Copy(out, LibraryDir() + "/libhish_probe_exec.so", root + "/home/native-probe");
        Copy(out, LibraryDir() + "/libhish_probe_payload.so", root + "/home/libprobe.so");
    } else if (kind == "fork-module") {
        void* module = dlopen((LibraryDir() + "/libhish_probe_payload.so").c_str(), RTLD_NOW | RTLD_LOCAL);
        auto entry = module ? reinterpret_cast<int (*)(int)>(dlsym(module, "HiSHProbePayload")) : nullptr;
        if (!entry) { out.Add("module load FAIL"); }
        else {
            out.Add("fork native module BEGIN");
            const pid_t pid = fork();
            if (pid == 0) { _exit(entry(out.fd)); }
            if (pid < 0) { out.Error("fork module", errno); }
            else { Wait(out, pid); }
        }
        if (module) { dlclose(module); }
    } else if (kind.rfind("module-", 0) == 0) {
        Module(out, kind == "module-home" ? root + "/home/libprobe.so" : LibraryDir() + (kind == "module-elf" ? "/libhish_probe_exec.so" : "/libhish_probe_payload.so"));
    } else if (kind == "fexec-bundle" || kind == "fexec-home" || kind == "execveat-home") {
        const std::string file = kind == "fexec-bundle" ? LibraryDir() + "/libhish_probe_exec.so" : root + "/home/native-probe";
        const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { out.Error("open ELF fd", errno); }
        else {
            out.Add("fexecve BEGIN in child");
            const bool useExecveat = kind == "execveat-home";
            const pid_t pid = fork();
            if (pid == 0) {
                dup2(out.fd, 1); dup2(out.fd, 2);
                char name[] = "native-probe"; char* args[] = {name, nullptr};
                if (useExecveat) { syscall(SYS_execveat, fd, "", args, environ, AT_EMPTY_PATH); }
                else { fexecve(fd, args, environ); }
                dprintf(2, "fexecve errno=%d\n", errno); _exit(127);
            }
            if (pid < 0) out.Error("fork", errno); else Wait(out, pid);
            close(fd);
        }
    } else {
        const std::string path = (kind == "spawn-toybox" || kind == "toybox-list") ? "/system/bin/toybox" : kind.find("loader") != std::string::npos ? "/lib/ld-musl-aarch64.so.1" : kind.find("bundle") != std::string::npos ? LibraryDir() + "/libhish_probe_exec.so" :
            kind.find("home") != std::string::npos ? root + "/home/native-probe" : kind == "spawn-bin" ? "/bin/sh" : "/system/bin/sh";
        char name[] = "sh", flag[] = "-c", command[] = "printf 'NATIVE_EXEC_OK\\n'; id; uname -m";
        char* argv[] = {name, flag, command, nullptr};
        char toybox[] = "toybox", uname[] = "uname", machine[] = "-m", list[] = "--long";
        char* toyboxArgv[] = {toybox, uname, machine, nullptr};
        char* listArgv[] = {toybox, list, nullptr};
        const std::string bundled = LibraryDir() + "/libhish_probe_exec.so";
        char* loaderArgv[] = {const_cast<char*>(path.c_str()), const_cast<char*>(bundled.c_str()), nullptr};
        char** arguments = kind == "spawn-toybox" ? toyboxArgv : kind == "toybox-list" ? listArgv : kind.find("loader") != std::string::npos ? loaderArgv : argv;
        out.Add("target=" + path);
        // No environment mutation in the application's process.
        if (kind.rfind("spawn-", 0) == 0 || kind == "toybox-list") {
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, out.fd, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, out.fd, STDERR_FILENO);
            pid_t pid = -1; out.Add("posix_spawn BEGIN");
            const int result = posix_spawn(&pid, path.c_str(), &actions, nullptr, arguments, environ);
            posix_spawn_file_actions_destroy(&actions);
            if (result != 0) { out.Error("posix_spawn", result); }
            else { out.Add("spawn pid=" + std::to_string(pid)); Wait(out, pid); }
        } else if (kind.rfind("exec-", 0) == 0) {
            out.Add("execve BEGIN (replaces this expendable application process)");
            const int oldOut = dup(STDOUT_FILENO), oldErr = dup(STDERR_FILENO);
            if (oldOut < 0 || oldErr < 0) {
                if (oldOut >= 0) close(oldOut);
                if (oldErr >= 0) close(oldErr);
                out.Error("save stdio", errno); return out.text;
            }
            fcntl(oldOut, F_SETFD, FD_CLOEXEC); fcntl(oldErr, F_SETFD, FD_CLOEXEC);
            dup2(out.fd, STDOUT_FILENO); dup2(out.fd, STDERR_FILENO);
            execve(path.c_str(), arguments, environ);
            const int error = errno;
            dup2(oldOut, STDOUT_FILENO); dup2(oldErr, STDERR_FILENO);
            close(oldOut); close(oldErr);
            out.Error("execve", error);
        } else {
            const bool useVfork = kind == "vfork-exec";
            const bool onlyFork = kind == "fork";
            int errors[2];
            if (pipe2(errors, O_CLOEXEC | O_NONBLOCK) != 0) { out.Error("error pipe", errno); return out.text; }
            out.Add(useVfork ? "vfork BEGIN" : "fork BEGIN");
            const char* target = path.c_str();
            const pid_t pid = useVfork ? vfork() : fork();
            if (pid == 0) {
                close(errors[0]);
                if (onlyFork) { _exit(42); }
                dup2(out.fd, STDOUT_FILENO); dup2(out.fd, STDERR_FILENO);
                execve(target, arguments, environ);
                const int error = errno;
                write(errors[1], &error, sizeof(error));
                _exit(127);
            }
            close(errors[1]);
            if (pid < 0) { out.Error(useVfork ? "vfork" : "fork", errno); }
            else {
                out.Add("child pid=" + std::to_string(pid)); Wait(out, pid);
                int error = 0;
                if (read(errors[0], &error, sizeof(error)) == sizeof(error)) { out.Error("child execve", error); }
            }
            close(errors[0]);
        }
    }
    out.Add("PROBE " + kind + " END");
    // Include child output written directly to the report, not just parent lines.
    return ReadFile(root + "/reports/" + kind + ".log");
}
}
