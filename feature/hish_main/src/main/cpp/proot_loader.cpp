// Platform adapter for PRoot's public load-script protocol (loader/script.h).
// PRoot remains the syscall/path tracer. This executes its actual ELF inputs
// after a permitted kernel exec, using the same bounded RW -> RX mapping path.
#include "elf_code_mapping.h"
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <limits.h>
extern char** environ;
namespace {
[[noreturn]] void Invalid(const char* reason){dprintf(2,"HiSH PRoot loader: %s errno=%d\n",reason,errno);_exit(126);}
void Need(bool ok,const char* reason){if(!ok)Invalid(reason);}
#if defined(__aarch64__)
__attribute__((naked,noreturn)) void Enter(void*,void*){__asm__ volatile("mov x2,x0\nmov sp,x1\nmov x0,xzr\nbr x2");}
#endif
}
extern "C" [[noreturn]] void HiSHEnterPRootImage(const char* value)
{
#if defined(__aarch64__)
    unsigned long address=0;unsigned size=0;char tail=0;
    Need(sscanf(value,"%lx:%x%c",&address,&size,&tail)==2,"script pointer");
    const uintptr_t slot=reinterpret_cast<uintptr_t>(value)-strlen("HISH_PROOT_SCRIPT=");
    Need(size&&size<65536-64&&address>=slot+64&&address<=slot+65536-size&&!(address%8),"script bounds");
    const auto* script=reinterpret_cast<const uintptr_t*>(address);size_t words=size/8,at=0;
    int fd=-1;uintptr_t base=0;bool nextBase=true;size_t mapped=0;
    std::vector<std::pair<uintptr_t,uintptr_t>> ranges;
    auto string=[&](uintptr_t p){Need(p>=address&&p<address+size&&memchr(reinterpret_cast<void*>(p),0,address+size-p),"script string");return reinterpret_cast<const char*>(p);};
    while(at<words){const uintptr_t action=script[at++];
        if(action==0||action==1){Need(at<words,"open statement");if(fd>=0)close(fd);fd=open(string(script[at++]),O_RDONLY|O_CLOEXEC);Need(fd>=0,"open target");nextBase=true;}
        else if(action==2||action==3){
            Need(words-at>=5,"map statement");const uintptr_t target=script[at++],length=script[at++],protection=script[at++],offset=script[at++],clear=script[at++];
            Need(length&&length<=512*1024*1024&&mapped<=512*1024*1024-length&&target<=UINTPTR_MAX-length&&!(target%4096)&&!(length%4096)&&!(offset%4096)&&clear<=length&&!(protection&~7)&&!((protection&PROT_EXEC)&&(protection&PROT_WRITE)),"map bounds/protection");
            for(const auto& range:ranges)Need(target>=range.second||target+length<=range.first,"overlapping image maps");
            void* dest=reinterpret_cast<void*>(target);Need(mmap(dest,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==dest,"reserve target range");ranges.emplace_back(target,target+length);mapped+=length;
            if(action==2){
                struct stat st{};Need(fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>=0&&uint64_t(st.st_size)<=512*1024*1024&&offset<=uint64_t(st.st_size),"target file bounds");const size_t bytes=std::min<uint64_t>(length,st.st_size-offset);
                if(protection&PROT_EXEC){Need(!clear,"code BSS unsupported");const char* stage=nullptr;Need(HishMapELFCode(dest,length,protection,fd,offset,bytes,nullptr,&stage)==dest,stage?stage:"code mapping");}
                else {Need(mmap(dest,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==dest,"data mapping");size_t copied=0;while(copied<bytes){ssize_t n=pread(fd,reinterpret_cast<char*>(dest)+copied,bytes-copied,offset+copied);if(n<0&&errno==EINTR)continue;Need(n>0,"read target data");copied+=n;}if(clear)memset(reinterpret_cast<char*>(dest)+length-clear,0,clear);Need(!mprotect(dest,length,protection),"data protection");}
                if(nextBase){base=target;nextBase=false;}
            }else Need(mmap(dest,length,protection,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==dest,"BSS mapping");
        }else if(action==5||action==6){
            Need(action==6&&words-at>=7,"nested ptracer unsupported");const uintptr_t stack=script[at++],entry=script[at++],phdr=script[at++],phent=script[at++],phnum=script[at++],mainEntry=script[at++],execfn=script[at++];
            Need(stack&&!(stack%16)&&phent==sizeof(Elf64_Phdr)&&phnum&&phnum<128,"start bounds");string(execfn);
            bool entryMapped=false;for(const auto& r:ranges)if(entry>=r.first&&entry<r.second)entryMapped=true;Need(entryMapped,"entry outside image");
            auto* initial=reinterpret_cast<uintptr_t*>(stack);Need(initial[0]<1024,"argument count");auto** env=reinterpret_cast<char**>(initial+initial[0]+2);size_t count=0;
            const char* libraries=getenv("HISH_NATIVE_LIBDIR");Need(libraries&&libraries[0]=='/',"platform libraries");
            const char* family=getenv("HISH_PROOT_LIBC");const bool glibc=family&&!strcmp(family,"glibc");
            std::string preload="LD_PRELOAD="+std::string(libraries)+(glibc?"/libhish_linux_glibc_tty.so:":"/libhish_linux_identity_padded.so:")+libraries+(glibc?"/libhish_linux_glibc_compat.so":"/libhish_linux_musl_loader.so");
            std::string search="LD_LIBRARY_PATH="+std::string(libraries)+":/lib:/usr/lib";
            std::string identity="HISH_EXECUTABLE="+std::string(string(execfn));
            for(;env[count]&&count<4096;count++){
                if(!strncmp(env[count],"LD_PRELOAD=",11))env[count]=strdup(preload.c_str());
                else if(!strncmp(env[count],"LD_LIBRARY_PATH=",16))env[count]=strdup(search.c_str());
                else if(!strncmp(env[count],"HISH_EXECUTABLE=",16))env[count]=strdup(identity.c_str());
                else if(!strncmp(env[count],"HISH_PROOT_SCRIPT=",18))env[count]=strdup("HISH_PROOT_BOOTSTRAPPED=1");
            }
            Need(count<4096,"environment count");environ=env;auto* aux=reinterpret_cast<uintptr_t*>(env+count+1);
            for(size_t n=0;n<128;n++,aux+=2){if(!aux[0])break;switch(aux[0]){case AT_PHDR:aux[1]=phdr;break;case AT_PHENT:aux[1]=phent;break;case AT_PHNUM:aux[1]=phnum;break;case AT_ENTRY:aux[1]=mainEntry;break;case AT_BASE:aux[1]=base;break;case AT_EXECFN:aux[1]=initial[1];break;}}
            if(fd>=0)close(fd);const char* name=strrchr(string(execfn),'/');prctl(PR_SET_NAME,name?name+1:string(execfn),0,0,0);
            Enter(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(stack));
        }else Invalid("unsupported load action");
    }
    Invalid("missing start action");
#else
    (void)value;Invalid("AArch64 required");
#endif
}
