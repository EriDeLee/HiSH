#include "native_elf.h"
#include "exec_bridge_shared.h"
#include "hish_tty_shared.h"
#include "native_tools.h"
#include "hish_elf_fault.h"
#include "shell_script.h"
#include "process_identity.h"
#include "native_ptrace_loader.h"
#include "elf_code_mapping.h"
#include "elf_dormant_heap.h"
#include "elf_execution_access.h"
#include "linux_library_path.h"
#include "elf_abi.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <string>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>
#include <vector>
#include <set>
#include <sstream>
#include <sys/syscall.h>
extern char** environ;
namespace {
std::string failure;
enum class Runtime { Automatic, Musl, Glibc, Ohos, Static };
struct Tool { const char* name; const char* file; Runtime runtime; };
constexpr Tool tools[] = {
#define TOOL(name,file,runtime) {name,file,Runtime::runtime},
    HISH_NATIVE_TOOLS(TOOL)
#undef TOOL
    {nullptr,nullptr,Runtime::Automatic}
};
bool Fail(const char* stage, int error) { errno=error; failure=std::string(stage)+": "+strerror(error); return false; }
// A terminal session is a fork copy of the app process, so it inherits the
// static ArkTS heap reservation even though no ArkTS code runs in it again.
// A fixed-address image may therefore reclaim the overlap. Only verified parts
// of such a dormant copy are released, and only inside a session copy: the app
// process itself keeps its live heap. Programs that do not collide never read
// the map, so they pay nothing.
bool ReleaseDormantArkTsCopy(uintptr_t low, uintptr_t high)
{
    if (!getenv("HISH_SESSION_COPY")) return false;
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string text; char chunk[65536];
    for (;;) {
        const ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        text.append(chunk, static_cast<size_t>(count));
    }
    close(fd);
    std::vector<hish::DormantRange> release;
    if (!hish::PlanDormantArkTsRelease(text.data(), text.size(), low, high, release)) return false;
    for (const auto& range : release) if (munmap(reinterpret_cast<void*>(range.begin), range.end - range.begin)) return false;
    return true;
}
std::string LibraryDir()
{
    Dl_info info{};
    if(!dladdr(reinterpret_cast<void*>(&HiSHExecFile),&info)||!info.dli_fname)return {};
    const std::string file(info.dli_fname);return file.substr(0,file.find_last_of('/'));
}
const Tool* Lookup(const char* name)
{
    if(!name)return nullptr;
    for(const auto& tool:tools)if(tool.name&&!strcmp(name,tool.name))return &tool;
    return nullptr;
}
bool Range(uint64_t start,uint64_t count,uint64_t size){return start<=size&&count<=size-start;}
struct Image {
    int fd=-1; std::vector<unsigned char> bytes; Elf64_Ehdr header{};std::vector<Elf64_Phdr> ph;std::string interpreter;
    ~Image(){if(fd>=0)close(fd);}
    bool Read(const std::string& file)
    {
        fd=open(file.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return Fail("open ELF",errno);
        struct stat st{};if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<sizeof(header)||st.st_size>128*1024*1024)return Fail("ELF file bounds",ENOEXEC);
        bytes.resize(st.st_size);size_t at=0;
        while(at<bytes.size()){const auto n=pread(fd,bytes.data()+at,bytes.size()-at,at);if(n<0&&errno==EINTR)continue;if(n<=0)return Fail("read ELF",n<0?errno:ENOEXEC);at+=n;}
        memcpy(&header,bytes.data(),sizeof(header));
        if(memcmp(header.e_ident,ELFMAG,SELFMAG)||header.e_ident[EI_CLASS]!=ELFCLASS64||header.e_ident[EI_DATA]!=ELFDATA2LSB||header.e_ident[EI_VERSION]!=EV_CURRENT||
            header.e_version!=EV_CURRENT||header.e_machine!=EM_AARCH64||(header.e_type!=ET_DYN&&header.e_type!=ET_EXEC)||header.e_phentsize!=sizeof(Elf64_Phdr)||
            !header.e_phnum||header.e_phnum>128||!Range(header.e_phoff,uint64_t(header.e_phnum)*sizeof(Elf64_Phdr),bytes.size()))return Fail("unsupported ELF format",ENOEXEC);
        ph.resize(header.e_phnum);memcpy(ph.data(),bytes.data()+header.e_phoff,ph.size()*sizeof(Elf64_Phdr));
        for(const auto& p:ph){
            if(p.p_type==PT_INTERP){if(!interpreter.empty()||!p.p_filesz||p.p_filesz>4096||!Range(p.p_offset,p.p_filesz,bytes.size())||bytes[p.p_offset+p.p_filesz-1])return Fail("ELF interpreter",ENOEXEC);interpreter.assign(reinterpret_cast<char*>(bytes.data()+p.p_offset),p.p_filesz-1);}
            if(p.p_type==PT_GNU_STACK&&(p.p_flags&PF_X))return Fail("executable stack unsupported",EACCES);
        }
        return true;
    }
};
struct Mapping {
    void* area=MAP_FAILED;size_t size=0;uintptr_t bias=0,entry=0,phdr=0;
#ifdef HISH_ENABLE_DIAGNOSTICS
    std::vector<hish::ExecutablePatch> patches;
#endif
    ~Mapping(){if(area!=MAP_FAILED)munmap(area,size);}
    bool Load(const Image& image, bool patchExecutable=false)
    {
#ifndef HISH_ENABLE_DIAGNOSTICS
        (void)patchExecutable;
#endif
        const size_t page=sysconf(_SC_PAGESIZE);uint64_t low=UINT64_MAX,high=0,alignment=page;
        bool executableEntry=false;
        for(const auto& p:image.ph)if(p.p_type==PT_LOAD&&p.p_memsz){
            if(p.p_filesz>p.p_memsz||!Range(p.p_offset,p.p_filesz,image.bytes.size())||p.p_memsz>UINT64_MAX-page||p.p_vaddr>UINT64_MAX-p.p_memsz-page||p.p_align>(1ULL<<24)||
                (p.p_align&&((p.p_align&(p.p_align-1))||p.p_offset%p.p_align!=p.p_vaddr%p.p_align))||p.p_offset%page!=p.p_vaddr%page)return Fail("ELF segment bounds/alignment",ENOEXEC);
            if((p.p_flags&PF_X)&&((p.p_flags&PF_W)||p.p_filesz!=p.p_memsz))return Fail("writable or zero-filled executable segment unsupported",EACCES);
            low=std::min(low,p.p_vaddr&~uint64_t(page-1));high=std::max(high,(p.p_vaddr+p.p_memsz+page-1)&~uint64_t(page-1));alignment=std::max(alignment,p.p_align);
            if((p.p_flags&PF_X)&&image.header.e_entry>=p.p_vaddr&&image.header.e_entry-p.p_vaddr<p.p_filesz)executableEntry=true;
        }
        if(high<=low||high-low>256*1024*1024||!executableEntry)return Fail("ELF image size/entry",ENOEXEC);
        size=high-low;
        if(image.header.e_type==ET_EXEC){
            const int reservationFlags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE;
            area=mmap(reinterpret_cast<void*>(low),size,PROT_NONE,reservationFlags,-1,0);
            if(area==MAP_FAILED&&errno==EEXIST&&ReleaseDormantArkTsCopy(low,low+size))
                area=mmap(reinterpret_cast<void*>(low),size,PROT_NONE,reservationFlags,-1,0);
            if(area==MAP_FAILED){
                const int error=errno;
#ifdef HISH_ENABLE_DIAGNOSTICS
                if(patchExecutable){
                    dprintf(2,"HISH_RUN_FIXED_RANGE low=%#llx size=%zu errno=%d\n",static_cast<unsigned long long>(low),size,error);
                    FILE* maps=fopen("/proc/self/maps","r");char line[1024];
                    if(maps){while(fgets(line,sizeof(line),maps)){unsigned long long start=0,end=0;if(sscanf(line,"%llx-%llx",&start,&end)==2&&start<high&&end>low)dprintf(2,"HISH_RUN_ADDRESS_CONFLICT %s",line);}fclose(maps);}
                }
#endif
                return Fail("fixed ELF address unavailable",error);
            }
            if(reinterpret_cast<uintptr_t>(area)!=low)return Fail("fixed ELF address not honored",ENOMEM);
        }else{
            void* reserved=mmap(nullptr,size+alignment,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(reserved==MAP_FAILED)return Fail("reserve ELF image",errno);
            const uintptr_t start=reinterpret_cast<uintptr_t>(reserved),aligned=start+((low-start)&uintptr_t(alignment-1));
            const size_t before=aligned-start,after=size+alignment-before-size;
            if(before)munmap(reserved,before);if(after)munmap(reinterpret_cast<void*>(aligned+size),after);area=reinterpret_cast<void*>(aligned);
        }
        bias=reinterpret_cast<uintptr_t>(area)-low;
        std::vector<std::pair<uint64_t,uint64_t>> occupied;
        for(const auto& p:image.ph)if(p.p_type==PT_LOAD&&p.p_memsz){
            const uint64_t begin=p.p_vaddr&~uint64_t(page-1),end=(p.p_vaddr+p.p_memsz+page-1)&~uint64_t(page-1);
            for(const auto& interval:occupied)if(begin<interval.second&&end>interval.first)return Fail("overlapping ELF segment pages unsupported",ENOEXEC);
            occupied.push_back({begin,end});
            const int protection=(p.p_flags&PF_R?PROT_READ:0)|(p.p_flags&PF_W?PROT_WRITE:0)|(p.p_flags&PF_X?PROT_EXEC:0);
            void* target=reinterpret_cast<void*>(bias+begin);void* result;
            // Fixed mappings replace only pages in this newly reserved image.
#ifdef HISH_ENABLE_DIAGNOSTICS
            if((p.p_flags&PF_X)&&patchExecutable){
                if(end-begin>16*1024*1024)return Fail("experimental executable segment limit",E2BIG);
                if(!hish::MapExecutableTemplate(target,end-begin))return Fail("map installed RX template",errno);
                // Fill the complete page from this ELF, including its page prefix.
                // Bounds/alignment were checked before mapping, and the rest stays zero.
                hish::ExecutablePatch patch{reinterpret_cast<uintptr_t>(target),std::vector<unsigned char>(end-begin)};
                const uint64_t offset=p.p_offset&~uint64_t(page-1),bytes=p.p_offset-offset+p.p_filesz;
                memcpy(patch.bytes.data(),image.bytes.data()+offset,bytes);patches.push_back(std::move(patch));result=target;
            }
            else
#endif
            if(p.p_flags&PF_X){
                const uint64_t offset=p.p_offset&~uint64_t(page-1),bytes=p.p_offset-offset+p.p_filesz;
                const char* stage=nullptr;
                result=HishMapELFCode(target,end-begin,protection,image.fd,offset,bytes,image.bytes.data()+offset,&stage);
                if(result==MAP_FAILED)return Fail(stage,errno);
            }
            else{
                result=mmap(target,end-begin,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
                if(result!=MAP_FAILED){memcpy(reinterpret_cast<void*>(bias+p.p_vaddr),image.bytes.data()+p.p_offset,p.p_filesz);if(mprotect(result,end-begin,protection))return Fail("protect ELF data",errno);}
            }
            if(result==MAP_FAILED)return Fail("map ELF segment",errno);
            if(image.header.e_phoff>=p.p_offset&&Range(image.header.e_phoff-p.p_offset,uint64_t(image.header.e_phnum)*sizeof(Elf64_Phdr),p.p_filesz))phdr=bias+p.p_vaddr+image.header.e_phoff-p.p_offset;
        }
        for(const auto& p:image.ph)if(p.p_type==PT_PHDR){if(p.p_vaddr<low||!Range(p.p_vaddr-low,uint64_t(image.header.e_phnum)*sizeof(Elf64_Phdr),size))return Fail("ELF PT_PHDR bounds",ENOEXEC);phdr=bias+p.p_vaddr;}
        if(!phdr)return Fail("ELF program headers not mapped",ENOEXEC);
        entry=bias+image.header.e_entry;return true;
    }
};
#if defined(__aarch64__)
__attribute__((naked,noreturn)) void Enter(void*,void*){__asm__ volatile("mov x2,x0\nmov sp,x1\nmov x0,xzr\nbr x2");}
#endif
void ExecSignalsAndDescriptors()
{
    for(int signal=1;signal<NSIG;signal++){
        if(signal==SIGKILL||signal==SIGSTOP)continue;struct sigaction old{},action{};
        if(!sigaction(signal,nullptr,&old)&&old.sa_handler!=SIG_IGN){action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);sigaction(signal,&action,nullptr);}
    }
    // Closing these only after all preparation avoids reusing inherited runtime FDs before an OHOS fork.
    DIR* directory=opendir("/proc/self/fd");if(!directory)return;
    const int own=dirfd(directory);std::vector<int> closeList;
    while(auto* item=readdir(directory)){char* end=nullptr;const long fd=strtol(item->d_name,&end,10);if(!end||*end||fd<0||fd==own||fd>INT32_MAX)continue;const int flags=fcntl(fd,F_GETFD);if(flags>=0&&(flags&FD_CLOEXEC))closeList.push_back(fd);}
    closedir(directory);for(const int fd:closeList)close(fd);
}
int Execute(const std::string& path,char* const argv[],Runtime runtime,const char* executable=nullptr,bool ptraceLoad=false,bool diagnostic=false,bool notify=false)
{
#if !defined(__aarch64__)
    (void)path;(void)argv;(void)runtime;(void)executable;(void)ptraceLoad;(void)diagnostic;(void)notify;Fail("native ELF loader requires AArch64",ENOTSUP);return -1;
#else
    Image program;if(!program.Read(path))return -1;Mapping main;if(!main.Load(program,ptraceLoad))return -1;
    const auto libraries=LibraryDir();Image interpreter;Mapping loader;
    if(!program.interpreter.empty()){
        if(runtime==Runtime::Automatic){
            const auto abi=HishInterpreterAbi(program.interpreter.c_str());
            runtime=abi==HishAbiGlibc?Runtime::Glibc:abi==HishAbiMusl?Runtime::Musl:Runtime::Automatic;
        }
        const std::string interpreterPath=runtime==Runtime::Musl?libraries+"/libhish_linux_musl_loader.so":runtime==Runtime::Glibc?libraries+"/libhish_linux_glibc_loader.so":runtime==Runtime::Ohos?"/lib/ld-musl-aarch64.so.1":"";
        if(interpreterPath.empty()){Fail("unsupported ELF interpreter/ABI",ENOEXEC);failure+=" ("+program.interpreter+")";return -1;}
        if(!interpreter.Read(interpreterPath)||!loader.Load(interpreter))return -1;
    }
    std::vector<std::string> arguments,environment;size_t total=0;
    for(size_t i=0;argv&&argv[i];i++){if(i>=1024){Fail("argument count",E2BIG);return -1;}const size_t length=strnlen(argv[i],1024*1024);if(length>=1024*1024||total+length+1>1024*1024){Fail("argument size",E2BIG);return -1;}arguments.emplace_back(argv[i],length);total+=length+1;}
    if(arguments.empty()){Fail("empty argument vector",EINVAL);return -1;}
    for(size_t i=0;environ&&environ[i];i++){
        if(i>=4096){Fail("environment count",E2BIG);return -1;}const size_t length=strnlen(environ[i],1024*1024);if(length>=1024*1024||total+length+1>2*1024*1024){Fail("environment size",E2BIG);return -1;}
        const std::string value(environ[i],length);if(value.rfind("LD_LIBRARY_PATH=",0)&&value.rfind("LD_PRELOAD=",0)&&value.rfind("LD_AUDIT=",0)&&value.rfind("GLIBC_TUNABLES=",0)&&value.rfind("HISH_NATIVE_LIBDIR=",0)&&value.rfind("HISH_EXECUTABLE=",0))environment.push_back(value);total+=length+1;
    }
    char muslSearch[HISH_LIBRARY_PATH_LIMIT];
    if(runtime==Runtime::Musl&&HishMuslLibraryPath(muslSearch,sizeof(muslSearch),libraries.c_str(),environ)){Fail("Linux musl library path",errno);return -1;}
    environment.push_back("LD_LIBRARY_PATH="+(runtime==Runtime::Musl?std::string(muslSearch):libraries+(runtime==Runtime::Ohos?":/system/lib64/ndk":"")));
    environment.push_back("HISH_NATIVE_LIBDIR="+libraries);
    char canonical[PATH_MAX];environment.push_back("HISH_EXECUTABLE="+std::string(HishImageName(path.c_str(),canonical,sizeof(canonical))));
    // Compatibility definitions must precede libc in musl's symbol scope.
    // Otherwise e.g. BusyBox's same-identity setgid reaches the denied syscall.
    if(runtime==Runtime::Musl)environment.push_back("LD_PRELOAD="+libraries+"/libhish_linux_identity_padded.so:"+libraries+"/libhish_linux_musl_loader.so");
    if(runtime==Runtime::Glibc)environment.push_back("LD_PRELOAD="+libraries+"/libhish_linux_glibc_tty.so:"+libraries+"/libhish_linux_glibc_compat.so");
    const char* tunables=getenv("GLIBC_TUNABLES");environment.push_back("GLIBC_TUNABLES="+std::string(tunables?tunables:"")+":glibc.pthread.rseq=0");
    const size_t page=sysconf(_SC_PAGESIZE),length=4*1024*1024;
    void* stack=mmap(nullptr,length+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(stack==MAP_FAILED){Fail("allocate ELF stack",errno);return -1;}
    if(mprotect(stack,page,PROT_NONE)){const int error=errno;munmap(stack,length+page);Fail("protect ELF stack guard",error);return -1;}
    uintptr_t top=reinterpret_cast<uintptr_t>(stack)+length+page;
    auto put=[&](const std::string& value){top-=value.size()+1;memcpy(reinterpret_cast<void*>(top),value.c_str(),value.size()+1);return top;};
    std::vector<uintptr_t> words{arguments.size()},envPointers;
    for(const auto& arg:arguments)words.push_back(put(arg));words.push_back(0);
    for(const auto& value:environment)envPointers.push_back(put(value));words.insert(words.end(),envPointers.begin(),envPointers.end());words.push_back(0);
    const auto execName=put(executable?executable:path);top-=16;
    if(getentropy(reinterpret_cast<void*>(top),16)){const int error=errno;munmap(stack,length+page);Fail("ELF random bytes",error);return -1;}const auto random=top;
    const uintptr_t aux[]={AT_PAGESZ,page,AT_PHDR,main.phdr,AT_PHENT,sizeof(Elf64_Phdr),AT_PHNUM,program.header.e_phnum,AT_ENTRY,main.entry,AT_BASE,loader.bias,
        AT_UID,getuid(),AT_EUID,geteuid(),AT_GID,getgid(),AT_EGID,getegid(),AT_SECURE,0,AT_EXECFN,execName,AT_RANDOM,random,
        AT_HWCAP,getauxval(AT_HWCAP),AT_HWCAP2,getauxval(AT_HWCAP2),AT_SYSINFO_EHDR,getauxval(AT_SYSINFO_EHDR),AT_CLKTCK,getauxval(AT_CLKTCK),AT_NULL,0};
    words.insert(words.end(),std::begin(aux),std::end(aux));top=(top-words.size()*sizeof(uintptr_t))&~uintptr_t(15);memcpy(reinterpret_cast<void*>(top),words.data(),words.size()*sizeof(uintptr_t));
    auto enter=[&]{ExecSignalsAndDescriptors();HishInstallELFDiagnostics();Enter(reinterpret_cast<void*>(program.interpreter.empty()?main.entry:loader.entry),reinterpret_cast<void*>(top));};
#ifdef HISH_ENABLE_DIAGNOSTICS
    if(ptraceLoad){
        const int result=hish::RunPatchedImage(main.patches,enter,diagnostic,notify);const int error=errno;munmap(stack,length+page);
        if(result<0)Fail("experimental ptrace loader",error);return result;
    }
#endif
    enter();__builtin_unreachable();
#endif
}
}
extern "C" const char* HiSHExecError(){return failure.c_str();}
extern "C" int HiSHNativeLibraryDirectory(char* buffer,unsigned capacity)
{
    const auto dir=LibraryDir();if(dir.empty()){errno=ENOENT;return -1;}if(!buffer||dir.size()>=capacity){errno=ENAMETOOLONG;return -1;}memcpy(buffer,dir.c_str(),dir.size()+1);return 0;
}
#ifdef HISH_ENABLE_DIAGNOSTICS
extern "C" int HiSHRunPtraceFile(const char* path,char* const argv[],const char* executable,int diagnostic,int notify)
{
    try{return Execute(path,argv,Runtime::Automatic,executable,true,diagnostic,notify);}catch(...){Fail("experimental loader allocation",ENOMEM);return -1;}
}
#endif
extern "C" int HiSHExecPackagedImage(const char* path,char* const argv[],unsigned runtime,const char* executable)
{
    const Runtime hints[]={Runtime::Automatic,Runtime::Static,Runtime::Musl,Runtime::Glibc,Runtime::Ohos};
    if(runtime>=std::size(hints)){Fail("bridge runtime",EINVAL);return -1;}
    try{return Execute(path,argv,hints[runtime],executable);}catch(...){Fail("bridge image allocation",ENOMEM);return -1;}
}
extern "C" int HiSHPackageMain(char* const argv[]);
extern "C" int HiSHHasNativeCommand(const char* name)
{
    if(!name)return 0;if(!strcmp(name,"hish-tools")||!strcmp(name,"pkg"))return 1;
    if(HishDisabledTool(name))return 1;
#ifdef HISH_ENABLE_DIAGNOSTICS
    if(!strcmp(name,"hish-run"))return 1;
#endif
    const auto* tool=Lookup(name);return tool&&access((LibraryDir()+"/"+tool->file).c_str(),R_OK)==0;
}
extern "C" int HiSHRunNativeCommand(char* const argv[])
{
    failure.clear();
    if(!strcmp(argv[0],"pkg"))return HiSHPackageMain(argv);
    if(HishDisabledTool(argv[0])){Fail("diagnostic command is not included in this build",ENOTSUP);return -1;}
#ifdef HISH_ENABLE_DIAGNOSTICS
    if(!strcmp(argv[0],"hish-run")){
        size_t at=1;bool diagnostic=false,notify=false;
        while(argv[at]){if(!strcmp(argv[at],"--diagnostic"))diagnostic=true;else if(!strcmp(argv[at],"--notify"))notify=true;else break;++at;}
        if(argv[at]&&!strcmp(argv[at],"--"))++at;
        if(!argv[at]){puts("Usage: hish-run [--diagnostic] [--notify] [--] ELF [ARG...]\nExperimental AArch64 loader; starts one supervised child, not PID-preserving exec.\n--notify requires seccomp user notification; mediates getppid with kernel CONTINUE.");return 2;}
        if(access(argv[at],X_OK)){Fail("experimental target executable permission",errno);return -1;}
        return HiSHRunPtraceFile(argv[at],argv+at,nullptr,diagnostic,notify);
    }
#endif
    if(!strcmp(argv[0],"hish-tools")){
        if(argv[1]&&!strcmp(argv[1],"--path")&&argv[2]){const auto* tool=Lookup(argv[2]);if(!tool||!HiSHHasNativeCommand(tool->name)){errno=ENOENT;failure="tool unavailable";return -1;}puts((LibraryDir()+"/"+tool->file).c_str());return 0;}
        puts("HiSH base: mksh, sh and built-in/system utilities.\nUse command -v NAME or whence NAME for command lookup.\nUse pkg help for signed Alpine packages and the qualified application catalog.");
#ifdef HISH_ENABLE_DIAGNOSTICS
        puts("Diagnostics enabled; experimental command: hish-run");
#endif
        for(const auto& tool:tools)if(tool.name&&HiSHHasNativeCommand(tool.name))printf("%s\n",tool.name);return 0;
    }
    const auto* tool=Lookup(argv[0]);if(!tool){errno=ENOENT;return -1;}
    try{return Execute(LibraryDir()+"/"+tool->file,argv,tool->runtime);}catch(...){Fail("ELF preparation allocation",ENOMEM);return -1;}
}
// A fresh OHOS bootstrap is essential when executing another embedded shell:
// calling mksh main twice would reuse the parent's parser/job/history globals.
extern "C" int HiSHExecNativeApplet(char* const argv[])
{
    if(!argv||!argv[0]||!HiSHHasApplet(argv[0])){errno=ENOENT;return -1;}
    const auto libraries=LibraryDir(),file=libraries+"/libhish_shell.so";
    std::vector<std::string> args,environment;
    size_t total=sizeof(HishExecBridgeHeader)+file.size()+1;
    for(size_t i=0;argv[i];i++){if(i>=1023){errno=E2BIG;return -1;}args.emplace_back(argv[i]);total+=args.back().size()+1;}
    for(size_t i=0;environ&&environ[i];i++){if(i>=4096){errno=E2BIG;return -1;}environment.emplace_back(environ[i]);total+=environment.back().size()+1;}
    if(total>HISH_EXEC_BRIDGE_LIMIT){errno=E2BIG;return -1;}
    int target=open(file.c_str(),O_RDONLY|O_CLOEXEC);if(target<0)return -1;
    int pinned=fcntl(target,F_DUPFD,3),saved=errno;close(target);if(pinned<0){errno=saved;return -1;}
    int request=memfd_create("hish-applet-request",MFD_ALLOW_SEALING);
    if(request>=0&&request<3){int higher=fcntl(request,F_DUPFD,3);saved=errno;close(request);request=higher;errno=saved;}
    if(request<0){saved=errno;close(pinned);errno=saved;return -1;}
    HishExecBridgeHeader header{HISH_EXEC_BRIDGE_MAGIC,HISH_EXEC_BRIDGE_VERSION,HISH_EXEC_BRIDGE_APPLET,
        static_cast<uint32_t>(args.size()),static_cast<uint32_t>(environment.size()),static_cast<uint32_t>(pinned),static_cast<uint32_t>(total)};
    std::vector<char> bytes(total);memcpy(bytes.data(),&header,sizeof(header));size_t at=sizeof(header);
    auto append=[&](const std::string& text){memcpy(bytes.data()+at,text.c_str(),text.size()+1);at+=text.size()+1;};
    append(file);for(const auto& arg:args)append(arg);for(const auto& value:environment)append(value);
    at=0;while(at<total){ssize_t n=write(request,bytes.data()+at,total-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=n;}
    if(at!=total||fcntl(request,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){
        saved=errno;close(request);close(pinned);errno=saved;return -1;
    }
    std::vector<std::string> clean;
    for(const auto& value:environment)if(value.rfind("LD_",0)&&value.rfind("GLIBC_TUNABLES=",0)&&value.rfind("HISH_EXEC_BRIDGE_FD=",0))clean.push_back(value);
    clean.push_back("LD_PRELOAD="+libraries+"/libhish_exec_bridge.so");
    clean.push_back("LD_LIBRARY_PATH="+libraries+":/system/lib64/ndk");
    clean.push_back("HISH_EXEC_BRIDGE_FD="+std::to_string(request));
    std::vector<char*> pointers;for(auto& value:clean)pointers.push_back(value.data());pointers.push_back(nullptr);
    char* bootstrap[]={const_cast<char*>("toybox"),const_cast<char*>("hish-exec-bridge-not-entered"),nullptr};
    execve("/system/bin/toybox",bootstrap,pointers.data());saved=errno;close(request);close(pinned);errno=saved;return -1;
}
extern "C" int HiSHExecFile(const char* path,char* const argv[])
{
    failure.clear();
    // Only our exact, app-created wrapper is an applet entry. A user script
    // with the same filename but different bytes keeps its ordinary semantics.
    if(const char* prefix=getenv("PREFIX")){
        const std::string directory=std::string(prefix)+"/bin/",requested=path;
        if(requested.rfind(directory,0)==0&&requested.find('/',directory.size())==std::string::npos){
            const auto name=requested.substr(directory.size());
            if(access(path,X_OK)==0){
                const std::string expected="#!/bin/sh\nexec "+name+" \"$@\"\n";
                char actual[512];int fd=open(path,O_RDONLY|O_CLOEXEC);ssize_t n=fd<0?-1:read(fd,actual,sizeof(actual));if(fd>=0)close(fd);
#ifndef HISH_ENABLE_DIAGNOSTICS
                if(n>0&&HishRetiredApplicationWrapper(name.c_str(),actual,static_cast<size_t>(n))){
                    Fail("former bundled application wrapper; an independent package is required",ENOENT);return -1;
                }
#endif
                if(HiSHHasApplet(name.c_str())&&n==static_cast<ssize_t>(expected.size())&&!memcmp(actual,expected.data(),n)){
                    std::vector<char*> arguments{const_cast<char*>(name.c_str())};for(size_t i=1;argv[i];i++){if(i>=1023){errno=E2BIG;return -1;}arguments.push_back(argv[i]);}arguments.push_back(nullptr);
                    // Apply a real kernel exec for native applets too: caught
                    // signal handlers and CLOEXEC FDs must not leak from mksh.
                    return HiSHExecNativeApplet(arguments.data());
                }
            }
        }
    }
    // Choose our script interpreter before kernel exec: permitted shebang
    // execution on another firmware must not switch to its system /bin/sh.
    try{
        const int fd=open(path,O_RDONLY|O_CLOEXEC);char header[512]{};const ssize_t count=fd<0?-1:read(fd,header,sizeof(header)-1);if(fd>=0)close(fd);
        if(count>0&&memcmp(header,ELFMAG,std::min<size_t>(count,SELFMAG))&&access(path,X_OK)==0&&!memchr(header,0,count)){
            const char* interpreter;char* option;
            if(HishShellScript(header,count,&interpreter,&option)<0){Fail("script interpreter unavailable",errno);return -1;}
            std::vector<char*> args {const_cast<char*>(interpreter)};if(option)args.push_back(option);args.push_back(const_cast<char*>(path));
            for(size_t i=1;argv[i];i++){if(i>=1023){Fail("script argument count",E2BIG);return -1;}args.push_back(argv[i]);}args.push_back(nullptr);
            return HiSHExecNativeApplet(args.data());
        }
    }catch(...){Fail("script preparation allocation",ENOMEM);return -1;}
    // A permitted OHOS executable must never inherit Linux loader settings.
    char* kernelEnvironment[4097];size_t count=0;
    for(size_t i=0;environ&&environ[i];i++){
        if(i>=4096){Fail("environment count",E2BIG);return -1;}
        if(!strncmp(environ[i],"LD_LIBRARY_PATH=",16)||!strncmp(environ[i],"LD_PRELOAD=",11)||
            !strncmp(environ[i],"LD_AUDIT=",9)||!strncmp(environ[i],"GLIBC_TUNABLES=",14))continue;
        kernelEnvironment[count++]=environ[i];
    }
    kernelEnvironment[count]=nullptr;execve(path,argv,kernelEnvironment);const int kernelError=errno;
    if(kernelError!=EACCES&&kernelError!=ENOEXEC&&kernelError!=ENOENT)return -1;
    try{
        if(HishELFExecutionAccess(path,LibraryDir().c_str())){Fail("target executable permission",errno);return -1;}
        return Execute(path,argv,Runtime::Automatic);
    }catch(...){Fail("ELF preparation allocation",ENOMEM);return -1;}
}
#ifdef HISH_ENABLE_DIAGNOSTICS
extern "C" void HiSHPrepareNetworkEnvironment();
extern "C" void HiSHPrepareCompilerEnvironment();
#endif
extern "C" void HiSHPrepareToolWrappers()
{
    HiSHInitializeApplets();
#ifdef HISH_ENABLE_DIAGNOSTICS
    HiSHPrepareNetworkEnvironment();
    HiSHPrepareCompilerEnvironment();
#endif
    const char* prefix=getenv("PREFIX");if(!prefix)return;
#ifdef HISH_ENABLE_DIAGNOSTICS
    {
        const std::string file=std::string(prefix)+"/bin/hish-run",script="#!/bin/sh\nexec hish-run \"$@\"\n";
        const int fd=open(file.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0700);
        if(fd>=0){const bool good=write(fd,script.data(),script.size())==static_cast<ssize_t>(script.size());close(fd);if(!good)unlink(file.c_str());}
    }
#endif
    std::set<std::string> names{"sh","mksh","hish-tools","pkg","cat","grep","sed","ls","mkdir","sleep","printf","test","["};
    for(const auto& tool:tools)if(tool.name&&HiSHHasNativeCommand(tool.name))names.insert(tool.name);
    std::istringstream system(std::string(HiSHSystemApplets())+" "+HiSHBuiltinApplets());std::string item;
    while(system>>item){const auto slash=item.find_last_of('/');names.insert(item.substr(slash==std::string::npos?0:slash+1));}
    for(const auto& name:names)if(HiSHHasApplet(name.c_str())&&!HishDisabledTool(name.c_str())){
        const std::string file=std::string(prefix)+"/bin/"+name,script=std::string("#!/bin/sh\nexec ")+name+" \"$@\"\n";
        const int fd=open(file.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0700);if(fd<0)continue;
        const bool good=write(fd,script.data(),script.size())==static_cast<ssize_t>(script.size());close(fd);if(!good)unlink(file.c_str());
    }
}
extern "C" void HiSHResetVirtualTerminal()
{
    const char* value=getenv("HISH_TTY_STATE_FD");if(!value)return;
    void* memory=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,atoi(value),0);if(memory==MAP_FAILED)return;
    auto* tty=static_cast<HishTtyState*>(memory);
    if(tty->magic==HISH_TTY_MAGIC&&tty->version==HISH_TTY_VERSION){
        tty->iflag=ICRNL|IXON;tty->oflag=OPOST|ONLCR;tty->foreground=getpgrp();
        __atomic_store_n(&tty->lflag,uint32_t(ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN),__ATOMIC_RELEASE);
    }
    munmap(memory,4096);
}
