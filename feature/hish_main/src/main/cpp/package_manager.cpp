#include "package_cli.h"
#include "native_elf.h"
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#endif

using namespace HishPackage;
namespace {
#if defined(__aarch64__)
#include "package_upstream.h"
extern "C" const unsigned char HiSHPackageData[],HiSHPackageDataEnd[];
const char* ManifestPath="etc/hish/packages.json";
bool BootstrapIntact(const fs::path& dest) {
    if(!fs::exists(dest/"ready"))return false;
    const std::string text=ReadText(dest/ManifestPath);
    return Sha256Hex(text.data(),text.size())==HishUpstream::Manifest_sha256;
}
// The embedded bootstrap is covered by the HAP signature; its extracted copy
// is user-writable, so the manifest is re-checked against the compiled digest
// and every payload is re-checked against the manifest at install time.
fs::path Bootstrap(const fs::path& base) {
    const fs::path dest=base/("bootstrap-" HISH_PACKAGE_BOOTSTRAP_SHA256);
    if(BootstrapIntact(dest))return dest;
    if(fs::exists(dest)){
        for(unsigned n=0;;n++){const fs::path aside=base/(".bootstrap-damaged-"+std::to_string(n));if(!fs::exists(aside)){fs::rename(dest,aside);break;}}
        Say("pkg: the extracted app package data changed; extracting it again");
    }
    const fs::path stage=base/(".bootstrap-"+std::to_string(getpid()));
    std::error_code ec; fs::remove_all(stage,ec); fs::create_directories(stage);
    try {
        auto bytes=Inflate(HiSHPackageData,HiSHPackageDataEnd-HiSHPackageData);Require(bytes.size()>=8&&!memcmp(bytes.data(),"HISHPKG1",8),"bootstrap format");
        size_t at=8;while(at<bytes.size()) {
            Require(bytes.size()-at>=12,"truncated bootstrap");uint32_t n=0,size=0,mode=0;
            memcpy(&n,bytes.data()+at,4);memcpy(&size,bytes.data()+at+4,4);memcpy(&mode,bytes.data()+at+8,4);at+=12;
            Require(n&&n<4096&&n<=bytes.size()-at&&size<=bytes.size()-at-n,"bootstrap bounds");std::string name(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;
            Write(stage,name,bytes.data()+at,size,mode);at+=size;
        }
        fs::create_directories(stage/"lib/apk/db");WriteText(stage/"ready","1\n");
        fs::rename(stage,dest);
    } catch (...) {fs::remove_all(stage,ec);throw;}
    Require(BootstrapIntact(dest),"app package manifest does not match this app build");
    return dest;
}
int Execute(const fs::path& executable,const std::vector<std::string>& arguments,const fs::path& libraries,const fs::path& certificate={}) {
    fflush(nullptr);pid_t pid=fork();Require(pid>=0,"fork package process");
    if(pid==0) {
        setenv("LD_LIBRARY_PATH",libraries.c_str(),1);
        if(!certificate.empty()) {setenv("SSL_CERT_FILE",certificate.c_str(),1);setenv("APK_CONFIG","/dev/null",1);unsetenv("SSL_NO_VERIFY_HOSTNAME");unsetenv("SSL_CERT_DIR");}
        std::vector<char*> argv;std::string command=executable.filename().string();argv.push_back(command.data());for(const auto& arg:arguments)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);
        HiSHExecFile(executable.c_str(),argv.data());dprintf(2,"pkg: execute %s: %s (%s)\n",executable.c_str(),strerror(errno),HiSHExecError());_exit(126);
    }
    int status;pid_t done;do{done=waitpid(pid,&status,0);}while(done<0&&errno==EINTR);Require(done==pid,"wait for package process");return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
std::string NativeDir(){char dir[PATH_MAX];return HiSHNativeLibraryDirectory(dir,sizeof dir)==0?std::string(dir):std::string();}
struct DeviceBackend final : Backend {
    fs::path base,boot;
    DeviceBackend(fs::path b,fs::path boot_):base(std::move(b)),boot(std::move(boot_)){}
    fs::path Ca() const {return boot/"etc/ssl/certs/ca-certificates.crt";}
    int Apk(const std::vector<std::string>& args) {
        std::vector<std::string> command{"--root",boot.string(),"--arch","aarch64","--no-cache","--timeout","30"};command.insert(command.end(),args.begin(),args.end());
        return Execute(boot/"sbin/apk",command,boot/"lib",Ca());
    }
    void FetchAlpine(const std::set<std::string>& names,const fs::path& dir) override {
        std::vector<std::string> args{"fetch","--recursive","--output",dir.string()};args.insert(args.end(),names.begin(),names.end());
        Require(Apk(args)==0,"download/verification failed; installed generation retained");
    }
    void VerifyApk(const fs::path& file) override {Require(Apk({"verify",file.string()})==0,"APK signature rejected: "+file.filename().string());}
    int UpdateIndexes() override {return Apk({"update"});}
    int SearchAlpine(const std::string& term) override {std::vector<std::string> a{"search","--"};if(!term.empty())a.push_back(term);return Apk(a);}
    int PolicyAlpine(const std::vector<std::string>& names) override {std::vector<std::string> a{"policy"};a.insert(a.end(),names.begin(),names.end());return Apk(a);}
    std::string FetchText(const std::string& url) override {
        const fs::path file=base/"state"/(".fetch-"+std::to_string(getpid())); std::error_code ec; fs::remove(file,ec); fs::create_directories(file.parent_path());
        const int rc=Execute(boot/"sbin/fetch-helper",{"--any",url,file.string()},boot/"lib",Ca());
        if(rc){fs::remove(file,ec);throw std::runtime_error("metadata download failed: "+url);}
        std::string text=ReadText(file); fs::remove(file,ec); return text;
    }
    void FetchArtifact(const std::string& url,const fs::path& dest,const std::string& sha512) override {
        Require(Execute(boot/"sbin/fetch-helper",{url,dest.string(),sha512},boot/"lib",Ca())==0,"download or SHA-512 check failed: "+url+"; installed generation retained");
    }
    void CheckArtifact(const fs::path& file,const std::string& sha512) override {
        Require(Execute(boot/"sbin/fetch-helper",{"--hash",file.string(),sha512},boot/"lib")==0,"cached artifact failed its SHA-512 check: "+file.string());
    }
    int RunCheck(const std::string& script,const fs::path& root,const fs::path& temp) override {
        const char* prefix=getenv("PREFIX");Require(prefix&&*prefix=='/',"PREFIX unavailable");
        fflush(nullptr);pid_t pid=fork();Require(pid>=0,"fork check");
        if(!pid){
            setenv("HISH_CHECK_DIR",temp.c_str(),1);setenv("HISH_PACKAGE_ROOT",root.c_str(),1);setenv("HISH_NATIVE_LIBDIR",NativeDir().c_str(),1);
            const char* path=getenv("PATH");const std::string search=(root/"usr/bin").string()+":"+(root/"bin").string()+":"+(path?path:"/system/bin");setenv("PATH",search.c_str(),1);
            if(chdir(temp.c_str()))_exit(125);
            const std::string shell=std::string(prefix)+"/bin/sh";char* argv[]={const_cast<char*>("sh"),const_cast<char*>("-c"),const_cast<char*>(script.c_str()),nullptr};
            HiSHExecFile(shell.c_str(),argv);_exit(126);
        }
        int status;pid_t done;do{done=waitpid(pid,&status,0);}while(done<0&&errno==EINTR);
        return done!=pid?1:WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
    }
    // A generation is referenced while any of our processes carries it in its
    // exec-time environment. Unreadable entries of our UID count as references.
    bool Referenced(const fs::path& generation) override {
        DIR* proc=opendir("/proc"); if(!proc)return true;
        const std::string needle="HISH_PACKAGE_ROOT="+generation.string(); bool found=false; const uid_t self=getuid();
        while(auto* e=readdir(proc)){
            char* end=nullptr;const long pid=strtol(e->d_name,&end,10);if(!end||*end||pid<=0||pid==getpid())continue;
            struct stat st{};const std::string dir=std::string("/proc/")+e->d_name;if(stat(dir.c_str(),&st)||st.st_uid!=self)continue;
            const int fd=open((dir+"/environ").c_str(),O_RDONLY|O_CLOEXEC);if(fd<0){if(errno!=ENOENT&&errno!=ESRCH){found=true;break;}continue;}
            std::string env;char chunk[8192];ssize_t n;while(env.size()<1024*1024&&(n=read(fd,chunk,sizeof chunk))>0)env.append(chunk,n);close(fd);
            for(size_t at=0;at<env.size();){size_t z=env.find('\0',at);if(z==std::string::npos)z=env.size();if(env.compare(at,z-at,needle)==0){found=true;break;}at=z+1;}
            if(found)break;
        }
        closedir(proc);return found;
    }
    static std::string Child(int (*body)()) {
        fflush(nullptr);pid_t pid=fork();if(pid<0)return "not tested (fork errno "+std::to_string(errno)+")";
        if(!pid)_exit(body());
        int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
        if(WIFSIGNALED(status))return "refused (signal "+std::to_string(WTERMSIG(status))+")";
        const int code=WEXITSTATUS(status);
        return code==0?"supported":"refused (errno "+std::to_string(code)+" "+strerror(code)+")";
    }
    std::string Probe(const std::string& name) override {
        if(name=="seccomp-user-notify")return Child([]{
            if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return errno;
            sock_filter filter[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr)),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_getppid,0,1),
                BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
            sock_fprog program{static_cast<unsigned short>(sizeof filter/sizeof filter[0]),filter};
            const long fd=syscall(__NR_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&program);
            return fd>=0?0:errno;
        });
        if(name=="ptrace-syscall"){
            fflush(nullptr);pid_t pid=fork();if(pid<0)return "not tested";
            if(!pid){if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr))_exit(errno);raise(SIGSTOP);getppid();_exit(0);}
            int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
            if(!WIFSTOPPED(status))return WIFEXITED(status)?"refused (PTRACE_TRACEME errno "+std::to_string(WEXITSTATUS(status))+")":"refused";
            std::string result="refused (no syscall stop)";
            if(ptrace(PTRACE_SETOPTIONS,pid,nullptr,reinterpret_cast<void*>(PTRACE_O_TRACESYSGOOD))==0&&ptrace(PTRACE_SYSCALL,pid,nullptr,nullptr)==0){
                while(waitpid(pid,&status,0)<0&&errno==EINTR){}
                if(WIFSTOPPED(status)&&WSTOPSIG(status)==(SIGTRAP|0x80))result="supported (syscall stops observed)";
            } else result="refused (errno "+std::to_string(errno)+")";
            kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}
            return result;
        }
        return "unknown probe";
    }
};
struct Lock {
    int fd;
    explicit Lock(const fs::path& p):fd(open(p.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600)) {if(fd<0)throw std::runtime_error("open package lock");if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);throw std::runtime_error("another package operation is running");}}
    ~Lock(){close(fd);}
};
fs::path Base() {const char* prefix=getenv("PREFIX");Require(prefix&&*prefix=='/',"PREFIX unavailable");return fs::path(prefix).parent_path()/"packages";}
#endif
}
extern "C" int HiSHPackageMain(char* const argv[]) {
    try {
#if defined(__aarch64__)
        std::vector<std::string> args;for(size_t i=1;argv[i];i++)args.push_back(argv[i]);
        if(args.empty()||args[0]=="help"||args[0]=="--help"){Say(Help());return 0;}
        const fs::path base=Base();fs::create_directories(base);
        if(args[0]=="--print-root"){Store store(base);const std::string id=store.Active();if(id.empty())return 1;Say(store.Dir(id).string());return 0;}
        Lock lock(base/"lock");const fs::path boot=Bootstrap(base);
        DeviceBackend io(base,boot);Context c(base,boot/"local",ReadText(boot/ManifestPath),io);
        return Command(c,args);
#else
        (void)argv;fputs("pkg: repository packages require an ARM64 device\n",stderr);return 1;
#endif
    } catch(const std::exception& e){fprintf(stderr,"pkg: %s\n",e.what());return 1;}
}
// Called by the app when a terminal session starts: imports the previous
// layout, applies a new app manifest to installed app-managed packages, and
// returns the active generation for this session's PATH.
extern "C" int HiSHPackageSession(char* buffer,unsigned capacity) {
#if defined(__aarch64__)
    try {
        const fs::path base=Base();fs::create_directories(base);
        try {
            Lock lock(base/"lock");const fs::path boot=Bootstrap(base);
            DeviceBackend io(base,boot);Context c(base,boot/"local",ReadText(boot/ManifestPath),io);std::string error;
            if(!Migrate(c,error))fprintf(stderr,"pkg: app package migration did not complete: %s\npkg: package changes stay blocked; the previous packages remain usable\n",error.c_str());
        } catch(const std::exception& e){fprintf(stderr,"pkg: package session setup skipped: %s\n",e.what());}
        // Until the previous layout is imported, sessions keep using it directly.
        Store store(base);const std::string id=store.Active();
        const std::string path=!id.empty()?store.Dir(id).string():fs::exists(base/"root"/".hish-package")?(base/"root").string():std::string();
        if(path.empty()||path.size()>=capacity)return -1;memcpy(buffer,path.c_str(),path.size()+1);return 0;
    } catch(...){return -1;}
#else
    (void)buffer;(void)capacity;return -1;
#endif
}
