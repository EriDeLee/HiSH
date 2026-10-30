#define _GNU_SOURCE
#include "hish_tty_shared.h"
#include "process_identity.h"
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

static HishTtyState* state;
static HishTtyState* terminal(void)
{
    HishTtyState* current=__atomic_load_n(&state,__ATOMIC_ACQUIRE);if(current)return current;
    const char* value=getenv("HISH_TTY_STATE_FD");if(!value)return NULL;
    char* end=NULL;long fd=strtol(value,&end,10);if(!end||*end||fd<3||fd>1048576)return NULL;
    void* mapped=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,(int)fd,0);if(mapped==MAP_FAILED)return NULL;
    HishTtyState* candidate=mapped;
    if(candidate->magic!=HISH_TTY_MAGIC||candidate->version!=HISH_TTY_VERSION){munmap(mapped,4096);return NULL;}
    HishTtyState* expected=NULL;
    if(!__atomic_compare_exchange_n(&state,&expected,candidate,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE)){munmap(mapped,4096);return expected;}
    return candidate;
}
static int is_terminal(int fd)
{
    HishTtyState* tty=terminal();if(!tty)return 0;
    struct stat status;if(fstat(fd,&status))return 0;
    return (uint64_t)status.st_dev==tty->device&&(uint64_t)status.st_ino==tty->inode;
}
static int background_access(int fd,int writing)
{
    if(!is_terminal(fd)||getsid(0)!=state->session)return 0;
    if(writing&&!(__atomic_load_n(&state->lflag,__ATOMIC_ACQUIRE)&TOSTOP))return 0;
    for(;;){
        pid_t group=getpgrp();if(group==__atomic_load_n(&state->foreground,__ATOMIC_ACQUIRE))return 0;
        int signal=writing?SIGTTOU:SIGTTIN;struct sigaction action;sigset_t mask;
        if(sigaction(signal,NULL,&action)||pthread_sigmask(SIG_SETMASK,NULL,&mask)){errno=EIO;return -1;}
        if(action.sa_handler==SIG_IGN||sigismember(&mask,signal)){
            if(writing)return 0;errno=EIO;return -1;
        }
        if(kill(-group,signal))return -1;
        if(action.sa_handler!=SIG_DFL){errno=EINTR;return -1;}
        // Default delivery stops the real process group; fg resumes it.
        // Orphaned groups cannot be stopped by job-control signals.
        if(group!=__atomic_load_n(&state->foreground,__ATOMIC_ACQUIRE)){errno=EIO;return -1;}
    }
}
#ifndef HISH_OHOS_TTY
static pthread_once_t io_once=PTHREAD_ONCE_INIT;
static ssize_t (*libc_read)(int,void*,size_t);
static ssize_t (*libc_readv)(int,const struct iovec*,int);
static ssize_t (*libc_write)(int,const void*,size_t);
static void resolve_io(void)
{
    libc_read=dlsym(RTLD_NEXT,"read");libc_readv=dlsym(RTLD_NEXT,"readv");libc_write=dlsym(RTLD_NEXT,"write");
}
#endif
static uint64_t milliseconds(void)
{
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
ssize_t read(int fd,void* bytes,size_t size)
{
    if(!is_terminal(fd)){
#ifdef HISH_OHOS_TTY
        // This object lives in the app's native linker namespace, where
        // RTLD_NEXT cannot resolve libc I/O. Do not use Linux preload lookup.
        return syscall(SYS_read,fd,bytes,size);
#else
        pthread_once(&io_once,resolve_io);if(!libc_read){errno=ENOSYS;return -1;}return libc_read(fd,bytes,size);
#endif
    }
    if(!size)return 0;if(background_access(fd,0))return -1;
    const int flags=fcntl(fd,F_GETFL);if(flags<0)return -1;
    const int canonical=__atomic_load_n(&state->lflag,__ATOMIC_ACQUIRE)&ICANON;
    const size_t minimum=canonical?1:(state->cc[VMIN]<size?state->cc[VMIN]:size);
    const unsigned timeout=canonical?0:state->cc[VTIME]*100;
    uint64_t deadline=!canonical&&!minimum&&timeout?milliseconds()+timeout:0;
    size_t total=0;
    for(;;){
#ifndef HISH_OHOS_TTY
        pthread_testcancel();
#endif
        if(HishTryInputLock(state)){
            int available=0;ssize_t count=-1;int saved=EAGAIN;size_t wanted=size-total;int boundary=0;
            if(syscall(SYS_ioctl,fd,FIONREAD,&available))saved=errno;
            else {
                uint64_t consumed=state->input_written>=(uint64_t)available?state->input_written-available:0;
                if(state->eof_head!=state->eof_tail){
                    uint64_t at=state->eof_at[state->eof_head%16];
                    if(at<=consumed){
                        if(!total)state->eof_head++;
                        boundary=1;
                    }else if(at-consumed<wanted)wanted=at-consumed;
                }
                if(!boundary){count=recv(fd,(char*)bytes+total,wanted,MSG_DONTWAIT);saved=errno;}
            }
            HishUnlockInput(state);
            if(boundary)return total;
            if(count>0){
                total+=count;if(total>=minimum||total==size||canonical||(flags&O_NONBLOCK))return total;
                if(timeout)deadline=milliseconds()+timeout;
                continue;
            }
            if(count==0)return total;
            if(saved!=EAGAIN&&saved!=EWOULDBLOCK){if(total)return total;errno=saved;return -1;}
        }
        if(flags&O_NONBLOCK){if(total)return total;errno=EAGAIN;return -1;}
        if(!canonical&&!minimum&&!timeout)return total;
        uint64_t now=milliseconds();if(deadline&&now>=deadline)return total;
        int wait=20;if(deadline&&deadline-now<(uint64_t)wait)wait=(int)(deadline-now);
        struct pollfd ready={.fd=fd,.events=POLLIN};
        if(poll(&ready,1,wait)<0){if(total)return total;return -1;}
        if(background_access(fd,0)){if(total)return total;return -1;}
    }
}
ssize_t readv(int fd,const struct iovec* vectors,int count)
{
    if(!is_terminal(fd)){
#ifdef HISH_OHOS_TTY
        return syscall(SYS_readv,fd,vectors,count);
#else
        pthread_once(&io_once,resolve_io);if(!libc_readv){errno=ENOSYS;return -1;}return libc_readv(fd,vectors,count);
#endif
    }
    if(count<0||count>IOV_MAX){errno=EINVAL;return -1;}size_t size=0;
    for(int i=0;i<count;i++){if(vectors[i].iov_len>(size_t)SSIZE_MAX-size){errno=EINVAL;return -1;}size+=vectors[i].iov_len;}
    if(!size)return 0;unsigned char* buffer=malloc(size);if(!buffer){errno=ENOMEM;return -1;}
    ssize_t result;int saved;
    pthread_cleanup_push(free,buffer);
    result=read(fd,buffer,size);size_t at=0;
    if(result>0)for(int i=0;i<count&&at<(size_t)result;i++){size_t n=vectors[i].iov_len;if(n>(size_t)result-at)n=result-at;memcpy(vectors[i].iov_base,buffer+at,n);at+=n;}
    saved=errno;pthread_cleanup_pop(1);errno=saved;return result;
}
ssize_t write(int fd,const void* bytes,size_t size)
{
    if(background_access(fd,1))return -1;
#ifdef HISH_OHOS_TTY
    return syscall(SYS_write,fd,bytes,size);
#else
    pthread_once(&io_once,resolve_io);if(!libc_write){errno=ENOSYS;return -1;}return libc_write(fd,bytes,size);
#endif
}
static void read_termios(struct termios* value)
{
    memset(value,0,sizeof(*value));value->c_iflag=state->iflag;value->c_oflag=state->oflag;
    value->c_cflag=state->cflag;value->c_lflag=__atomic_load_n(&state->lflag,__ATOMIC_ACQUIRE);
    memcpy(value->c_cc,state->cc,sizeof(value->c_cc)<32?sizeof(value->c_cc):32);
    cfsetispeed(value,state->input_speed);cfsetospeed(value,state->output_speed);
}
static void write_termios(const struct termios* value)
{
    state->iflag=value->c_iflag;state->oflag=value->c_oflag;state->cflag=value->c_cflag;
    memcpy(state->cc,value->c_cc,sizeof(value->c_cc)<32?sizeof(value->c_cc):32);
    state->input_speed=cfgetispeed(value);state->output_speed=cfgetospeed(value);
    __atomic_store_n(&state->lflag,value->c_lflag,__ATOMIC_RELEASE);
}
int isatty(int fd)
{
    if(is_terminal(fd))return 1;
    unsigned char kernel_termios[64];return syscall(SYS_ioctl,fd,TCGETS,kernel_termios)==0;
}
int tcgetattr(int fd,struct termios* value)
{
    if(is_terminal(fd)){if(!value){errno=EFAULT;return -1;}read_termios(value);return 0;}
    // Both supported ABIs use the first 36 bytes for the Linux kernel termios.
    unsigned char raw[36]={0};if(syscall(SYS_ioctl,fd,TCGETS,raw))return -1;
    memset(value,0,sizeof(*value));memcpy(value,raw,36);return 0;
}
int tcsetattr(int fd,int action,const struct termios* value)
{
    if(action!=TCSANOW&&action!=TCSADRAIN&&action!=TCSAFLUSH){errno=EINVAL;return -1;}
    if(is_terminal(fd)){if(!value){errno=EFAULT;return -1;}if(action==TCSAFLUSH&&tcflush(fd,TCIFLUSH))return -1;write_termios(value);return 0;}
    return syscall(SYS_ioctl,fd,action==TCSANOW?TCSETS:action==TCSADRAIN?TCSETSW:TCSETSF,value);
}
pid_t tcgetpgrp(int fd)
{
    if(is_terminal(fd))return __atomic_load_n(&state->foreground,__ATOMIC_ACQUIRE);
    int group=-1;return syscall(SYS_ioctl,fd,TIOCGPGRP,&group)?-1:group;
}
pid_t tcgetsid(int fd)
{
    if(is_terminal(fd)){
        if(getsid(0)!=state->session){errno=ENOTTY;return -1;}
        return state->session;
    }
    int session=-1;return syscall(SYS_ioctl,fd,TIOCGSID,&session)?-1:session;
}
int tcsetpgrp(int fd,pid_t group)
{
    if(is_terminal(fd)){
        if(group<=1||getsid(group)!=state->session){errno=EPERM;return -1;}
        __atomic_store_n(&state->foreground,group,__ATOMIC_RELEASE);return 0;
    }
    return syscall(SYS_ioctl,fd,TIOCSPGRP,&group);
}
int tcdrain(int fd){return is_terminal(fd)?0:syscall(SYS_ioctl,fd,TCSBRK,1);}
int tcflush(int fd,int action)
{
    if(!is_terminal(fd))return syscall(SYS_ioctl,fd,TCFLSH,action);
    if(action!=TCIFLUSH&&action!=TCOFLUSH&&action!=TCIOFLUSH){errno=EINVAL;return -1;}
    if(action!=TCOFLUSH){
        int locked=0;for(int i=0;i<1000&&!locked;i++)locked=HishTryInputLock(state);
        if(!locked){errno=EAGAIN;return -1;}
        char bytes[256];ssize_t count;do{count=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);}while(count>0||(count<0&&errno==EINTR));
        int saved=count<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK?errno:0;
        if(!saved)state->eof_head=state->eof_tail;
        HishUnlockInput(state);
        if(saved){errno=saved;return -1;}
    }
    return 0;
}
int open(const char* path,int flags,...)
{
    // This app domain can create unnamed temporary inodes but cannot publish
    // them with linkat(/proc/self/fd/N). Advertise the unsupported operation
    // before creating one so upstream callers use their named-temp fallback.
    if((flags&O_TMPFILE)==O_TMPFILE){errno=EOPNOTSUPP;return -1;}
    mode_t mode=0;if((flags&O_CREAT)||(flags&O_TMPFILE)==O_TMPFILE){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    if(!strcmp(path,"/dev/tty")&&terminal()){
        const char* value=getenv("HISH_TTY_IO_FD");if(!value){errno=ENXIO;return -1;}const int fd=atoi(value);
        if(!is_terminal(fd)){errno=ENXIO;return -1;}return fcntl(fd,flags&O_CLOEXEC?F_DUPFD_CLOEXEC:F_DUPFD,3);
    }
    return syscall(SYS_openat,AT_FDCWD,HishExecutablePath(path),flags,mode);
}
int openat(int directory,const char* path,int flags,...)
{
    if((flags&O_TMPFILE)==O_TMPFILE){errno=EOPNOTSUPP;return -1;}
    mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    return syscall(SYS_openat,directory,HishExecutablePath(path),flags,mode);
}
ssize_t readlinkat(int directory,const char* path,char* buffer,size_t size)
{
    char combined[PATH_MAX],descriptor[64],parent[PATH_MAX];const char* requested=path;
    if(path[0]!='/'&&directory!=AT_FDCWD){
        snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",directory);
        ssize_t n=syscall(SYS_readlinkat,AT_FDCWD,descriptor,parent,sizeof(parent)-1);
        if(n>0&&(size_t)n<sizeof(parent)){parent[n]=0;int length=snprintf(combined,sizeof(combined),"%s/%s",parent,path);if(length>0&&(size_t)length<sizeof(combined))requested=combined;}
    }
    const char* image=HishExecutablePath(requested);
    if(image!=requested){
        if(!size){errno=EINVAL;return -1;}size_t n=strlen(image);if(n>size)n=size;memcpy(buffer,image,n);return n;
    }
    return syscall(SYS_readlinkat,directory,path,buffer,size);
}
ssize_t readlink(const char* path,char* buffer,size_t size) { return readlinkat(AT_FDCWD,path,buffer,size); }
#ifdef __GLIBC__
int ioctl(int fd,unsigned long request,...)
#else
int ioctl(int fd,int request,...)
#endif
{
    va_list args;va_start(args,request);void* argument=va_arg(args,void*);va_end(args);
    if(is_terminal(fd)){
        if((request==TIOCGWINSZ||request==TIOCSWINSZ||request==TIOCGPGRP||request==TIOCSPGRP||request==TIOCGSID||request==TCGETS||request==TCSETS||request==TCSETSW||request==TCSETSF)&&!argument){errno=EFAULT;return -1;}
        if(request==TIOCGWINSZ){struct winsize* size=argument;memset(size,0,sizeof(*size));size->ws_col=state->columns;size->ws_row=state->rows;return 0;}
        if(request==TIOCSWINSZ){const struct winsize* size=argument;int changed=state->columns!=size->ws_col||state->rows!=size->ws_row;state->columns=size->ws_col;state->rows=size->ws_row;
            if(changed){pid_t group=__atomic_load_n(&state->foreground,__ATOMIC_ACQUIRE);if(group>1)kill(-group,SIGWINCH);}return 0;}
        if(request==TIOCGPGRP){*(int*)argument=tcgetpgrp(fd);return 0;}
        if(request==TIOCSPGRP)return tcsetpgrp(fd,*(int*)argument);
        if(request==TIOCGSID){pid_t session=tcgetsid(fd);if(session<0)return -1;*(int*)argument=session;return 0;}
        if(request==TIOCSCTTY){if(getsid(0)!=state->session){errno=ENOTSUP;return -1;}return 0;}
        if(request==TCGETS){uint32_t* flags=argument;flags[0]=state->iflag;flags[1]=state->oflag;flags[2]=state->cflag;flags[3]=state->lflag;((unsigned char*)argument)[16]=0;memcpy((unsigned char*)argument+17,state->cc,19);return 0;}
        if(request==TCSETS||request==TCSETSW||request==TCSETSF){if(request==TCSETSF&&tcflush(fd,TCIFLUSH))return -1;const uint32_t* flags=argument;state->iflag=flags[0];state->oflag=flags[1];state->cflag=flags[2];memcpy(state->cc,(const unsigned char*)argument+17,19);__atomic_store_n(&state->lflag,flags[3],__ATOMIC_RELEASE);return 0;}
    }
    return syscall(SYS_ioctl,fd,request,argument);
}
