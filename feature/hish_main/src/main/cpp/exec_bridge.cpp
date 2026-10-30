#include "exec_bridge_shared.h"
#include "native_elf.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
extern "C" [[noreturn]] void HiSHEnterPRootImage(const char*);
__attribute__((noreturn)) static void InvalidRequest(const char* stage)
{
    dprintf(2,"HiSH exec bridge request: %s errno=%d\n",stage,errno);_exit(126);
}
__attribute__((constructor)) static void EnterRequestedImage()
{
    if(const char* script=getenv("HISH_PROOT_SCRIPT"))HiSHEnterPRootImage(script);
    const char* value=getenv("HISH_EXEC_BRIDGE_FD");if(!value)return;
    const bool diagnostic=getenv("HISH_SPAWN_DIAGNOSTICS")!=nullptr;
    if(diagnostic)dprintf(2,"EXEC_BOOTSTRAP_ENTER\n");
    char* end=nullptr;long number=strtol(value,&end,10);
    if(!end||*end||number<3||number>1048576)InvalidRequest("descriptor number");
    const int fd=number;
    const int seals=fcntl(fd,F_GET_SEALS),required=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    if(seals<0||(seals&required)!=required)InvalidRequest("seals");
    HishExecBridgeHeader header;
    if(pread(fd,&header,sizeof(header),0)!=sizeof(header))InvalidRequest("header read");
    if(header.magic!=HISH_EXEC_BRIDGE_MAGIC||header.version!=HISH_EXEC_BRIDGE_VERSION||!header.argc||header.argc>=1024||header.envc>4096||header.runtime>HISH_EXEC_BRIDGE_APPLET||header.target_fd<3||header.bytes<sizeof(header)||header.bytes>HISH_EXEC_BRIDGE_LIMIT)InvalidRequest("header");
    // This firmware allows sealed memfd reads across exec, but fstat(memfd)
    // returns EACCES in the OHOS bootstrap. Length comes from the bounded,
    // sealed header instead; every byte is still checked by exact pread.
    const size_t size=header.bytes;
    char* bytes=static_cast<char*>(malloc(size));if(!bytes)InvalidRequest("allocation");
    size_t at=0;
    while(at<size){ssize_t n=pread(fd,bytes+at,size-at,at);if(n<0&&errno==EINTR)continue;if(n<=0)InvalidRequest("read");at+=n;}
    close(fd);
    char** args=static_cast<char**>(calloc(header.argc+1,sizeof(char*)));
    char** environment=static_cast<char**>(calloc(header.envc+1,sizeof(char*)));
    if(!args||!environment)InvalidRequest("vectors");
    at=sizeof(header);
    char* executable=bytes+at;void* terminator=memchr(executable,0,size-at);
    if(!terminator)InvalidRequest("executable name");at=static_cast<char*>(terminator)-bytes+1;
    for(uint32_t i=0;i<header.argc+header.envc;i++){
        if(at>=size)InvalidRequest("string bounds");
        char* text=bytes+at;void* zero=memchr(text,0,size-at);if(!zero)InvalidRequest("string terminator");
        at=static_cast<char*>(zero)-bytes+1;
        if(i<header.argc)args[i]=text;else environment[i-header.argc]=text;
    }
    if(at!=size||fcntl(header.target_fd,F_SETFD,FD_CLOEXEC))InvalidRequest("target descriptor");
    environ=environment;
    unsetenv("HISH_EXEC_BRIDGE_FD");
    if(header.runtime==HISH_EXEC_BRIDGE_APPLET){
        close(header.target_fd);
        HiSHInitializeApplets();
        if(!HiSHHasApplet(args[0])){errno=ENOENT;InvalidRequest("applet unavailable");}
        HiSHRunApplet(args);
        InvalidRequest("applet returned");
    }
    if(diagnostic)dprintf(2,"EXEC_BOOTSTRAP_IMAGE\n");
    char path[80];snprintf(path,sizeof(path),"/proc/self/fd/%u",header.target_fd);
    if(header.runtime>=HISH_EXEC_BRIDGE_PTRACE){
#ifdef HISH_ENABLE_DIAGNOSTICS
        const unsigned options=header.runtime-HISH_EXEC_BRIDGE_PTRACE;
        const int result=HiSHRunPtraceFile(path,args,executable,options&1,options&2);
        if(result<0)dprintf(2,"HiSH experimental loader: %s (errno=%d)\n",HiSHExecError(),errno);
        _exit(result<0?126:result);
#else
        errno=ENOTSUP;InvalidRequest("experimental loading is not included");
#endif
    }
    HiSHExecPackagedImage(path,args,header.runtime,executable);
    dprintf(2,"HiSH exec bridge: %s (errno=%d)\n",HiSHExecError(),errno);_exit(126);
}
