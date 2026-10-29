#ifndef HISH_NATIVE_LIBRARY_TRACE_H
#define HISH_NATIVE_LIBRARY_TRACE_H
#include "native_elf.h"
#include "hish_tty_shared.h"
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>

// Optional test-build diagnosis only. Observe the actual kernel calls of one
// app-owned child; do not change syscall numbers/results, bytes or permissions.
template<class Log> static void HiSHLibraryTrace(const std::string& root, const std::string& previous, Log log, bool vim = false)
{
#if !defined(__aarch64__)
    (void)root;(void)previous;log("LIBRARY_TRACE_UNSUPPORTED_ARCH");
#else
    const std::string marker="DOWNLOAD_ROOT=";
    const auto at=previous.find(marker),end=at==std::string::npos?at:previous.find_first_of("\r\n",at);
    if(at==std::string::npos)throw std::runtime_error("trace requires an existing original-download report");
    const auto directory=previous.substr(at+marker.size(),end==std::string::npos?end:end-at-marker.size());
    const auto prefix=root+"/home/.hish-download-abi-";
    if(directory.rfind(prefix,0)||directory.size()!=prefix.size()+6||directory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",prefix.size())!=std::string::npos)throw std::runtime_error("invalid existing test directory");
    const auto output=directory+(vim?"/vim-syscall-trace-5.log":"/checksum-syscall-trace.log");
    const int fd=open(output.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
    if(fd<0)throw std::runtime_error("trace output exists or cannot be created errno="+std::to_string(errno));
    int pair[2]={-1,-1},ttyFd=-1;HishTtyState* tty=nullptr;
    if(vim){
        if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))throw std::runtime_error("trace socketpair");
        ttyFd=memfd_create("hish-vim-trace-terminal",0);
        if(ttyFd<0||ftruncate(ttyFd,4096))throw std::runtime_error("trace terminal memfd");
        void* memory=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,ttyFd,0);
        if(memory==MAP_FAILED)throw std::runtime_error("trace terminal mmap");
        tty=static_cast<HishTtyState*>(memory);memset(tty,0,sizeof(*tty));
        tty->magic=HISH_TTY_MAGIC;tty->version=HISH_TTY_VERSION;tty->columns=100;tty->rows=30;
        tty->iflag=ICRNL|IXON;tty->oflag=OPOST|ONLCR;tty->cflag=CS8|CREAD;tty->lflag=ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN;
        tty->cc[VINTR]=3;tty->cc[VQUIT]=28;tty->cc[VERASE]=127;tty->cc[VKILL]=21;tty->cc[VEOF]=4;tty->cc[VMIN]=1;
        tty->cc[VSTART]=17;tty->cc[VSTOP]=19;tty->cc[VSUSP]=26;tty->cc[VWERASE]=23;tty->input_speed=B38400;tty->output_speed=B38400;
        struct stat identity{};if(fstat(pair[1],&identity))throw std::runtime_error("trace terminal identity");tty->device=identity.st_dev;tty->inode=identity.st_ino;
        if(fcntl(pair[0],F_SETFL,O_NONBLOCK))throw std::runtime_error("trace terminal nonblocking");
    }
    const pid_t child=fork();
    if(child<0){close(fd);throw std::runtime_error("trace fork failed");}
    if(!child){
        if(vim){
            close(pair[0]);if(setsid()<0)_exit(125);tty->foreground=getpgrp();tty->session=getsid(0);
            if(dup2(pair[1],0)<0||dup2(pair[1],1)<0||dup2(pair[1],2)<0)_exit(125);
            const int io=fcntl(pair[1],F_DUPFD,256);if(io<0)_exit(125);close(pair[1]);
            setenv("HISH_TTY_STATE_FD",std::to_string(ttyFd).c_str(),1);setenv("HISH_TTY_IO_FD",std::to_string(io).c_str(),1);
            setenv("TERM","xterm-256color",1);setenv("TERMINFO",(directory+"/share/terminfo").c_str(),1);setenv("LD_LIBRARY_PATH",(directory+"/lib/musl").c_str(),1);
        }else if(dup2(fd,1)<0||dup2(fd,2)<0)_exit(125);
        close(fd);
        if(chdir(directory.c_str())||setenv("HOME",(root+"/home").c_str(),1)||setenv("PREFIX",(root+"/usr").c_str(),1)||setenv("TMPDIR",(root+"/tmp").c_str(),1)||setenv("HISH_ELF_DIAGNOSTICS","1",1))_exit(125);
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr))_exit(126);
        raise(SIGSTOP);
        char a[]="busybox",b[]="sha256sum",c[]="-c",d[]="checksums.sha256";char* args[]={a,b,c,d,nullptr};
        if(vim){
            char name[]="./bin/vim",u[]="-Nu",none[]="NONE",i[]="-i",n[]="-n",file[]="vim-trace.txt";
            char* vimArgs[]={name,u,none,i,none,n,file,nullptr};HiSHExecFile(name,vimArgs);_exit(126);
        }
        const int status=HiSHRunNativeCommand(args);_exit(status<0?126:status);
    }
    if(vim){close(pair[1]);close(ttyFd);}else close(fd);
    log("LIBRARY_TRACE_CHILD="+std::to_string(child));log("LIBRARY_TRACE_INPUT="+directory);log("LIBRARY_TRACE_OUTPUT="+output);
    bool options=false,entering=true,stopped=false,finished=false;int pending=0,status=0;uint64_t last=UINT64_MAX,count=0;
    std::vector<std::string> tail(64);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<deadline){
        if(vim){char bytes[8192];ssize_t n;while((n=read(pair[0],bytes,sizeof(bytes)))>0)if(write(fd,bytes,n)!=n)throw std::runtime_error("trace transcript write");}
        const pid_t got=waitpid(child,&status,WNOHANG|WUNTRACED);
        if(!got){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        if(got<0){if(errno==EINTR)continue;log("LIBRARY_TRACE_WAIT_ERRNO="+std::to_string(errno));break;}
        if(WIFEXITED(status)||WIFSIGNALED(status)){
            log("LIBRARY_TRACE_RESULT "+std::string(WIFEXITED(status)?"exit=":"signal=")+std::to_string(WIFEXITED(status)?WEXITSTATUS(status):WTERMSIG(status))+" last_syscall="+std::to_string(last));finished=true;break;
        }
        if(!WIFSTOPPED(status))continue;
        stopped=true;const int signal=WSTOPSIG(status);pending=0;
        if(!options){
            if(signal!=SIGSTOP||ptrace(PTRACE_SETOPTIONS,child,nullptr,reinterpret_cast<void*>(PTRACE_O_TRACESYSGOOD))){log("LIBRARY_TRACE_OPTIONS_ERRNO="+std::to_string(errno));break;}options=true;
        }else if(signal==(SIGTRAP|0x80)){
            struct Registers {uint64_t x[31],sp,pc,pstate;} regs{};iovec io{&regs,sizeof(regs)};
            if(ptrace(PTRACE_GETREGSET,child,reinterpret_cast<void*>(NT_PRSTATUS),&io)||io.iov_len!=sizeof(regs)){log("LIBRARY_TRACE_REGS_ERRNO="+std::to_string(errno));break;}
            std::string line;
            if(entering){last=regs.x[8];line="enter syscall="+std::to_string(last)+" pc="+std::to_string(regs.pc);for(int a=0;a<6;a++)line+=" a"+std::to_string(a)+"="+std::to_string(regs.x[a]);}
            else line="leave syscall="+std::to_string(last)+" result="+std::to_string(static_cast<int64_t>(regs.x[0]));
            tail[count++%tail.size()]=line;entering=!entering;
        }else{
            pending=signal;log("LIBRARY_TRACE_SIGNAL="+std::to_string(signal)+" last_syscall="+std::to_string(last));
        }
        if(ptrace(PTRACE_SYSCALL,child,nullptr,reinterpret_cast<void*>(static_cast<intptr_t>(pending)))){log("LIBRARY_TRACE_RESUME_ERRNO="+std::to_string(errno));break;}
        stopped=false;
    }
    for(uint64_t i=count>tail.size()?count-tail.size():0;i<count;i++)log("LIBRARY_TRACE "+tail[i%tail.size()]);
    if(!finished){
        // No EXITKILL and no forced termination. Detach only at a known stop.
        const int detached=stopped?ptrace(PTRACE_DETACH,child,nullptr,reinterpret_cast<void*>(static_cast<intptr_t>(pending))):-1;
        log("LIBRARY_TRACE_INCOMPLETE child="+std::to_string(child)+" detached="+std::to_string(stopped&&detached==0)+"; manual graceful cleanup may be required");
    }
    log("LIBRARY_TRACE_FINISHED="+std::to_string(finished));
    if(vim){char bytes[8192];ssize_t n;while((n=read(pair[0],bytes,sizeof(bytes)))>0)write(fd,bytes,n);close(pair[0]);close(fd);munmap(tty,4096);}
#endif
}
#endif
