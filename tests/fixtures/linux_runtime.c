#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
extern int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t*,const char*);
extern int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t*,int);
static void* system_worker(void* argument) { int result=system(argument);if(result!=23*256)dprintf(2,"PROCESS_SYSTEM_RESULT status=%d errno=%d command=%s\n",result,errno,(char*)argument);return (void*)(intptr_t)result; }
static int process_extensions(const char* peer)
{
    int (*closefrom_action)(posix_spawn_file_actions_t*,int)=dlsym(RTLD_DEFAULT,"posix_spawn_file_actions_addclosefrom_np");
    if(!closefrom_action)return 129;
    pthread_t threads[2];void* results[2];
    struct sigaction action={0},saved,after;action.sa_handler=SIG_IGN;sigemptyset(&action.sa_mask);
    if(sigaction(SIGQUIT,&action,&saved))return 130;
    for(int i=0;i<2;i++)if(pthread_create(&threads[i],NULL,system_worker,"sleep 0.1; exit 23"))return 131;
    for(int i=0;i<2;i++)if(pthread_join(threads[i],&results[i])||!WIFEXITED((int)(intptr_t)results[i])||WEXITSTATUS((int)(intptr_t)results[i])!=23)return 132;
    if(sigaction(SIGQUIT,NULL,&after)||after.sa_handler!=SIG_IGN)return 133;sigaction(SIGQUIT,&saved,NULL);
    puts("PROCESS_CONCURRENT_SYSTEM_OK");
    char file[4096],command[8192];const char* home=getenv("HOME");if(!home)return 134;
    snprintf(file,sizeof(file),"%s/.hish-system-cancel-%d",home,getpid());
    snprintf(command,sizeof(command),"echo $$ > '%s'; exec sleep 30",file);
    if(pthread_create(&threads[0],NULL,system_worker,command))return 135;
    pid_t child=-1;
    for(int i=0;i<500&&child<1;i++){FILE* input=fopen(file,"r");if(input){if(fscanf(input,"%d",&child)!=1)child=-1;fclose(input);}if(child<1)usleep(10000);}
    if(child<1)return 136;
    pthread_cancel(threads[0]);if(pthread_join(threads[0],&results[0])||results[0]!=PTHREAD_CANCELED)return 137;
    unlink(file);if(kill(child,0)!=-1||errno!=ESRCH)return 138;
    puts("PROCESS_SYSTEM_CANCEL_REAP_OK");
    FILE* pipe=popen("printf 'native popen'; exit 17","re");char text[64]={0};
    if(!pipe||!(fcntl(fileno(pipe),F_GETFD)&FD_CLOEXEC)||!fgets(text,sizeof(text),pipe)||strcmp(text,"native popen"))return 139;
    int status=pclose(pipe);if(!WIFEXITED(status)||WEXITSTATUS(status)!=17)return 140;
    pipe=popen("printf hold; sleep 0.2","r");if(!pipe||(fcntl(fileno(pipe),F_GETFD)&FD_CLOEXEC))return 141;
    struct stat identity;if(fstat(fileno(pipe),&identity))return 142;
    snprintf(command,sizeof(command),"%s --assert-pipe-closed %d %llu %llu",peer,fileno(pipe),(unsigned long long)identity.st_dev,(unsigned long long)identity.st_ino);
    FILE* second=popen(command,"r");if(!second||pclose(second))return 143;
    if(!fgets(text,sizeof(text),pipe)||strcmp(text,"hold")||pclose(pipe))return 144;
    snprintf(command,sizeof(command),"cat > '%s'",file);pipe=popen(command,"we");
    if(!pipe||fputs("native write pipe",pipe)<0||pclose(pipe))return 145;
    FILE* input=fopen(file,"r");memset(text,0,sizeof(text));if(!input||!fgets(text,sizeof(text),input)||strcmp(text,"native write pipe"))return 146;
    fclose(input);unlink(file);errno=0;if(popen("true","r+")||errno!=EINVAL)return 147;
    puts("PROCESS_POPEN_READ_WRITE_CLOSE_OK");
    int old=open("/dev/null",O_RDONLY);if(old<0)return 148;int high=fcntl(old,F_DUPFD,81);close(old);if(high<0)return 149;
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    if(closefrom_action(&actions,3)||posix_spawn_file_actions_addopen(&actions,80,"/dev/null",O_RDONLY,0))return 150;
    char number[32];snprintf(number,sizeof(number),"%d",high);char* args[]={(char*)peer,"--assert-closefrom",number,NULL};pid_t pid=-1;
    int result=posix_spawnp(&pid,peer,&actions,NULL,args,environ);posix_spawn_file_actions_destroy(&actions);close(high);
    if(result||waitpid(pid,&status,0)!=pid||status)return 151;
    posix_spawn_file_actions_init(&actions);closefrom_action(&actions,0);
    posix_spawn_file_actions_addopen(&actions,1,"/nonexistent/hish/no-file",O_RDONLY,0);pid=-17;
    result=posix_spawnp(&pid,peer,&actions,NULL,args,environ);posix_spawn_file_actions_destroy(&actions);
    if(result!=ENOENT||pid!=-17)return 152;
    puts("PROCESS_CLOSEFROM_ERROR_CHANNEL_OK");
    struct sched_param observed={0};errno=0;int observed_policy=sched_getscheduler(0),policy_errno=errno;
    errno=0;int observed_result=sched_getparam(0,&observed),parameter_errno=errno;
    struct sched_param raw_parameter={0};errno=0;long raw_policy=syscall(SYS_sched_getscheduler,0),raw_policy_errno=errno;
    errno=0;long raw_result=syscall(SYS_sched_getparam,0,&raw_parameter),raw_parameter_errno=errno;
    printf("PROCESS_SCHEDULER_STATE policy=%d errno=%d parameter=%d errno=%d priority=%d raw_policy=%ld errno=%ld raw_parameter=%ld errno=%ld raw_priority=%d\n",observed_policy,policy_errno,observed_result,parameter_errno,observed.sched_priority,raw_policy,raw_policy_errno,raw_result,raw_parameter_errno,raw_parameter.sched_priority);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);struct sched_param parameter={0};
    parameter.sched_priority=7;int roundtrip_policy=-1;struct sched_param roundtrip={0};
    if(posix_spawnattr_setschedparam(&attributes,&parameter)||posix_spawnattr_getschedparam(&attributes,&roundtrip)||roundtrip.sched_priority!=7||
       posix_spawnattr_setschedpolicy(&attributes,SCHED_FIFO)||posix_spawnattr_getschedpolicy(&attributes,&roundtrip_policy)||roundtrip_policy!=SCHED_FIFO||
       posix_spawnattr_setschedpolicy(&attributes,-1)!=EINVAL)return 153;
    parameter.sched_priority=0;
    if(posix_spawnattr_setschedparam(&attributes,&parameter)||posix_spawnattr_setschedpolicy(&attributes,SCHED_OTHER)||
       posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSCHEDPARAM|POSIX_SPAWN_SETSCHEDULER))return 153;
    char* scheduler[]={(char*)peer,"--assert-scheduler",NULL};pid=-1;result=posix_spawnp(&pid,peer,NULL,&attributes,scheduler,environ);posix_spawnattr_destroy(&attributes);
    if(result==ENOSYS||result==EPERM||result==EACCES){
        if(pid!=-1)return 153;printf("PROCESS_SCHEDULER_UNAVAILABLE errno=%d\n",result);
    }else if(result||waitpid(pid,&status,0)!=pid||status){dprintf(2,"PROCESS_SCHEDULER_FAIL result=%d status=%d errno=%d\n",result,status,errno);return 153;}
    else puts("PROCESS_SCHEDULER_APPLIED_OK");
    puts("PROCESS_SCHEDULER_RESULT_CHECKED");return 0;
}

// Volatile prevents a whole-TU optimizer from replacing the TLS test with constants.
static _Thread_local volatile int local_value = 41;
static volatile int* main_local;
static volatile sig_atomic_t signal_seen;
__attribute__((constructor)) static void initialize(void) { local_value++; }
static void received(int signal) { signal_seen = signal; }
static int terminal_jobs(const char* peer)
{
    struct termios saved,mode;if(tcgetattr(0,&saved))return 170;mode=saved;mode.c_lflag|=TOSTOP;if(tcsetattr(0,TCSANOW,&mode))return 171;
    pid_t child=fork();int status;if(child<0)return 172;
    if(!child){
        if(setpgid(0,0))_exit(173);signal(SIGTTIN,SIG_IGN);char byte;
        if(read(0,&byte,1)!=-1||errno!=EIO)_exit(174);
        signal(SIGTTOU,received);if(write(1,"UNEXPECTED_BACKGROUND_WRITE",27)!=-1||errno!=EINTR||signal_seen!=SIGTTOU)_exit(175);
        _exit(0);
    }
    if(waitpid(child,&status,0)!=child||status||tcsetattr(0,TCSANOW,&saved))return 176;
    puts("INPUT_BACKGROUND_READ_WRITE_OK");fflush(stdout);
    child=fork();if(child<0)return 177;
    if(!child){
        if(setpgid(0,0))_exit(178);signal(SIGTTIN,SIG_DFL);char bytes[8];
        if(read(0,bytes,sizeof(bytes))!=2||memcmp(bytes,"q\n",2))_exit(179);_exit(0);
    }
    pid_t waited;do{waited=waitpid(child,&status,WUNTRACED);}while(waited<0&&errno==EINTR);
    if(waited!=child||!WIFSTOPPED(status)||(WSTOPSIG(status)!=SIGTTIN&&WSTOPSIG(status)!=SIGSTOP)){kill(child,SIGKILL);waitpid(child,NULL,0);return 180;}
    printf("INPUT_BACKGROUND_STOP_SIGNAL=%d\n",WSTOPSIG(status));
    if(tcsetpgrp(0,child)||kill(child,SIGCONT))return 181;
    puts("INPUT_BACKGROUND_FEED_READY");fflush(stdout);
    if(waitpid(child,&status,0)!=child||status||tcsetpgrp(0,getpgrp()))return 182;
    puts("INPUT_BACKGROUND_FG_RESUME_OK");
    int (*foreground_action)(posix_spawn_file_actions_t*,int)=dlsym(RTLD_DEFAULT,"posix_spawn_file_actions_addtcsetpgrp_np");if(!foreground_action)return 183;
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);foreground_action(&actions,0);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);posix_spawnattr_setpgroup(&attributes,0);posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
    char* args[]={(char*)peer,"--assert-foreground",NULL};int result=posix_spawnp(&child,peer,&actions,&attributes,args,environ);
    posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
    if(result||waitpid(child,&status,0)!=child||status||tcsetpgrp(0,getpgrp()))return 184;
    puts("INPUT_SPAWN_FOREGROUND_OK");fflush(stdout);return 0;
}
static void* blocked_worker(void* argument)
{
    char byte;const int fd=*(int*)argument;
    while(read(fd,&byte,1)<0&&errno==EINTR){}
    return NULL;
}
static void* worker(void* unused)
{
    (void)unused;
    if (&local_value == main_local || local_value != 41) return (void*)1;
    local_value = 99;
    return local_value == 99 ? (void*)99 : (void*)1;
}
int main(int argc, char** argv)
{
    if(argc==2&&!strcmp(argv[1],"--self-exec")){
        const char* image=getenv("HISH_EXECUTABLE");char link[4096]={0},at[4096]={0},raw[4096]={0};
        ssize_t n=readlink("/proc/self/exe",link,sizeof(link)-1);int directory=open("/proc/self",O_DIRECTORY|O_RDONLY|O_CLOEXEC);
        ssize_t m=directory<0?-1:readlinkat(directory,"exe",at,sizeof(at)-1);if(directory>=0)close(directory);
        if(!image||image[0]!='/'||n<0||m<0||strcmp(link,image)||strcmp(at,image))return 186;
        int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);struct stat a,b;
        if(fd<0||fstat(fd,&a)||stat(image,&b)||a.st_dev!=b.st_dev||a.st_ino!=b.st_ino)return 187;close(fd);
        syscall(SYS_readlinkat,AT_FDCWD,"/proc/self/exe",raw,sizeof(raw)-1);printf("PROCESS_KERNEL_EXECUTABLE=%s\n",raw);fflush(stdout);
        char* args[]={"self-image","--echo","PROCESS_SELF_REEXEC_OK",NULL};execv("/proc/self/exe",args);return 188;
    }
    if(argc==2&&!strcmp(argv[1],"--assert-foreground")){return tcgetpgrp(0)==getpgrp()?0:185;}
    if(argc==3&&!strcmp(argv[1],"--tty-input")){
        struct termios original,mode;if(tcgetattr(0,&original))return 157;mode=original;mode.c_lflag|=ICANON;mode.c_lflag&=~ECHO;
        if(tcsetattr(0,TCSANOW,&mode))return 158;
        puts("INPUT_EOF_READY");fflush(stdout);
        char a[8]={0},b[8]={0};struct iovec vectors[]={{a,1},{b,7}};
        if(readv(0,vectors,2)!=2||a[0]!='a'||b[0]!='b'||read(0,a,8)!=0)return 159;
        if(read(0,a,8)!=2||memcmp(a,"cd",2)||read(0,a,8)!=0)return 160;
        if(read(0,a,8)!=2||memcmp(a,"e\n",2))return 161;
        puts("INPUT_REPEATABLE_EOF_READV_OK");
        puts("INPUT_FLUSH_READY");fflush(stdout);usleep(100000);
        if(tcflush(0,TCIFLUSH))return 189;
        puts("INPUT_AFTER_FLUSH_READY");fflush(stdout);
        if(read(0,a,8)!=2||memcmp(a,"f\n",2))return 190;
        puts("INPUT_FLUSH_EOF_OK");
        cfmakeraw(&mode);mode.c_cc[VMIN]=0;mode.c_cc[VTIME]=1;if(tcsetattr(0,TCSANOW,&mode))return 162;
        struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);
        if(read(0,a,8)!=0)return 163;clock_gettime(CLOCK_MONOTONIC,&after);
        long elapsed=(after.tv_sec-before.tv_sec)*1000+(after.tv_nsec-before.tv_nsec)/1000000;
        if(elapsed<70||elapsed>1500)return 164;
        mode.c_cc[VMIN]=2;mode.c_cc[VTIME]=1;if(tcsetattr(0,TCSANOW,&mode))return 165;
        puts("INPUT_INTERBYTE_READY");fflush(stdout);
        if(read(0,a,8)!=1||a[0]!='x')return 166;
        mode.c_cc[VMIN]=0;mode.c_cc[VTIME]=0;if(tcsetattr(0,TCSANOW,&mode)||read(0,a,8)!=0)return 167;
        if(tcsetattr(0,TCSANOW,&original))return 168;
        puts("INPUT_VMIN_VTIME_OK");fflush(stdout);
        int result=terminal_jobs(argv[2]);if(result){dprintf(2,"INPUT_JOBS_FAIL=%d errno=%d\n",result,errno);return result;}
        puts("INPUT_COMPLETE");fflush(stdout);return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--process-extensions"))return process_extensions(argv[2]);
    if(argc==5&&!strcmp(argv[1],"--assert-pipe-closed")){
        struct stat info;if(!fstat(atoi(argv[2]),&info)&&info.st_dev==strtoull(argv[3],NULL,10)&&info.st_ino==strtoull(argv[4],NULL,10))return 154;return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--assert-closefrom")){
        if(fcntl(80,F_GETFD)<0||fcntl(atoi(argv[2]),F_GETFD)!=-1||errno!=EBADF)return 155;return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--assert-scheduler")){
        struct sched_param parameter;if(sched_getscheduler(0)!=SCHED_OTHER||sched_getparam(0,&parameter)||parameter.sched_priority)return 156;return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--execution-policy")) {
        int fd=open(argv[2],O_RDONLY|O_CLOEXEC);struct stat info;
        if(fd<0||fstat(fd,&info)||info.st_size<SELFMAG)return 103;
        void* map=mmap(NULL,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0);int map_error=map==MAP_FAILED?errno:0;
        if(map!=MAP_FAILED)munmap(map,4096);close(fd);
        pid_t child=fork();if(child<0)return 104;
        if(!child){char* args[]={argv[2],NULL};syscall(SYS_execve,argv[2],args,environ);int saved=errno;dprintf(1,"POLICY_KERNEL_EXEC_ERRNO=%d\n",saved);_exit(saved==EACCES?0:105);}
        int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 106;
        printf("POLICY_RX_MMAP_ERRNO=%d size=%lld\n",map_error,(long long)info.st_size);
        return WIFEXITED(status)&&!WEXITSTATUS(status)&&map_error==EACCES?0:107;
    }
    if(argc==4&&!strcmp(argv[1],"--fd-exec")) {
        int fd=open(argv[3],O_RDONLY|O_CLOEXEC);if(fd<0)return 108;
        int (*exec_at)(int,const char*,char* const[],char* const[],int)=dlsym(RTLD_DEFAULT,"execveat");
        if(!exec_at)return 113;
        char* args[]={argv[3],"--echo","LINUX_FD_EXEC_OK",NULL};
        if(!strcmp(argv[2],"fexecve"))fexecve(fd,args,environ);
        else if(!strcmp(argv[2],"empty"))exec_at(fd,"",args,environ,AT_EMPTY_PATH);
        else if(!strcmp(argv[2],"relative")){
            char* slash=strrchr(argv[3],'/');if(!slash)return 109;
            *slash=0;int directory=open(argv[3],O_PATH|O_DIRECTORY|O_CLOEXEC);*slash='/';
            if(directory<0)return 110;exec_at(directory,slash+1,args,environ,AT_SYMLINK_NOFOLLOW);
        }else return 111;
        printf("LINUX_FD_EXEC_ERROR=%d\n",errno);return 112;
    }
    if(argc==4&&!strcmp(argv[1],"--make-bad-elf")) {
        unsigned char data[8192]={0};Elf64_Ehdr* eh=(Elf64_Ehdr*)data;Elf64_Phdr* ph=(Elf64_Phdr*)(data+sizeof(*eh));
        memcpy(eh->e_ident,ELFMAG,SELFMAG);eh->e_ident[EI_CLASS]=ELFCLASS64;eh->e_ident[EI_DATA]=ELFDATA2LSB;eh->e_ident[EI_VERSION]=EV_CURRENT;
        eh->e_version=EV_CURRENT;eh->e_type=ET_DYN;eh->e_machine=EM_AARCH64;eh->e_entry=4096;eh->e_phoff=sizeof(*eh);eh->e_phentsize=sizeof(*ph);eh->e_phnum=1;
        ph->p_type=PT_LOAD;ph->p_flags=PF_R|PF_X;ph->p_offset=4096;ph->p_vaddr=4096;ph->p_filesz=4;ph->p_memsz=4;ph->p_align=4096;
        if(!strcmp(argv[2],"overflow"))ph->p_memsz=UINT64_MAX;
        else if(!strcmp(argv[2],"align"))ph->p_align=3;
        else if(!strcmp(argv[2],"phdr"))eh->e_phoff=UINT64_MAX;
        else if(!strcmp(argv[2],"rwx"))ph->p_flags|=PF_W;
        else return 85;
        int fd=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0700);if(fd<0){printf("MALFORMED_CREATE path=%s errno=%d\n",argv[3],errno);return 86;}
        const int ok=write(fd,data,sizeof(data))==sizeof(data);close(fd);return ok?0:87;
    }
    if(argc==4&&!strcmp(argv[1],"--reject-elf")) {
        char* args[]={argv[3],NULL};execv(argv[3],args);
        if(errno!=(!strcmp(argv[2],"rwx")?EACCES:ENOEXEC))return 88;
        puts("LINUX_MALFORMED_REJECTED_OK");return 0;
    }
    if(argc>=3&&!strcmp(argv[1],"--spawn")) {
        pid_t child;const int result=posix_spawnp(&child,argv[2],NULL,NULL,argv+2,environ);
        if(result){printf("LINUX_SPAWN_ERROR=%d\n",result);return 72;}
        int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 73;
        return WIFEXITED(status)?WEXITSTATUS(status):74;
    }
    if(argc==3&&!strcmp(argv[1],"--spawn-contract")) {
        posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
        sigset_t mask,defaults;sigemptyset(&mask);sigaddset(&mask,SIGALRM);sigemptyset(&defaults);sigaddset(&defaults,SIGUSR1);
        signal(SIGUSR1,SIG_IGN);posix_spawnattr_setsigmask(&attributes,&mask);posix_spawnattr_setsigdefault(&attributes,&defaults);
        posix_spawnattr_setpgroup(&attributes,0);posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
        char* args[]={argv[2],"--assert-spawn",NULL};pid_t child;int result=posix_spawnp(&child,argv[2],NULL,&attributes,args,environ);posix_spawnattr_destroy(&attributes);
        if(result)return 75;int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 76;
        return WIFEXITED(status)?WEXITSTATUS(status):77;
    }
    if(argc==2&&!strcmp(argv[1],"--assert-spawn")) {
        sigset_t mask;struct sigaction action;sigprocmask(SIG_SETMASK,NULL,&mask);sigaction(SIGUSR1,NULL,&action);
        if(getpgrp()!=getpid()||!sigismember(&mask,SIGALRM)||action.sa_handler!=SIG_DFL)return 78;
        puts("LINUX_SPAWN_CONTRACT_OK");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--spawn-negative")) {
        pid_t child;char* args[]={".hish-command-does-not-exist",NULL};
        if(posix_spawnp(&child,args[0],NULL,NULL,args,environ)!=ENOENT)return 79;
        posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
        int result=posix_spawnp(&child,args[0],&actions,NULL,args,environ);posix_spawn_file_actions_destroy(&actions);
        if(result!=ENOENT)return 80;
        if(waitpid(-1,NULL,WNOHANG)!=-1||errno!=ECHILD)return 81;
        puts("LINUX_SPAWN_ERROR_CLEANUP_OK");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--assert-file-actions")) {
        char data[4],cwd[512];
        if(read(0,data,sizeof(data))!=sizeof(data)||memcmp(data,"data",4)||!getcwd(cwd,sizeof(cwd))||
            !strstr(cwd,".hish-spawn-")||fcntl(10,F_GETFD)<0||(fcntl(10,F_GETFD)&FD_CLOEXEC))return 89;
        int fd=open("relative",O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return 90;close(fd);
        if(write(1,"output\n",7)!=7||write(2,"error\n",6)!=6)return 91;return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--spawn-files")) {
        const char* home=getenv("HOME");if(!home)return 92;
        char directory[512],input[560],output[560],relative[560];
        snprintf(directory,sizeof(directory),"%s/.hish-spawn-XXXXXX",home);if(!mkdtemp(directory))return 93;
        snprintf(input,sizeof(input),"%s/input",directory);snprintf(output,sizeof(output),"%s/output",directory);snprintf(relative,sizeof(relative),"%s/relative",directory);
        int fd=open(input,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0||write(fd,"data",4)!=4)return 94;close(fd);
        int dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC),wake[2];pthread_t thread;
        if(dir<0||pipe(wake)||pthread_create(&thread,NULL,blocked_worker,&wake[0]))return 95;
        int failed=0;
        for(int mode=0;mode<2&&!failed;mode++){
            posix_spawn_file_actions_t actions;failed=posix_spawn_file_actions_init(&actions);
            if(!failed)failed=mode?posix_spawn_file_actions_addfchdir_np(&actions,dir):posix_spawn_file_actions_addchdir_np(&actions,directory);
            // Deliberately collide with the usual private pipe fd numbers.
            for(int i=3;i<32&&!failed;i++)failed=posix_spawn_file_actions_addclose(&actions,i);
            if(!failed)failed=posix_spawn_file_actions_addopen(&actions,0,"input",O_RDONLY,0);
            if(!failed)failed=posix_spawn_file_actions_addopen(&actions,1,"output",O_WRONLY|O_CREAT|O_TRUNC,0600);
            if(!failed)failed=posix_spawn_file_actions_adddup2(&actions,1,2);
            if(!failed)failed=posix_spawn_file_actions_addopen(&actions,10,"input",O_RDONLY|O_CLOEXEC,0);
            if(!failed)failed=posix_spawn_file_actions_adddup2(&actions,10,10);
            char* args[]={argv[2],"--assert-file-actions",NULL};pid_t child=-1;
            if(!failed)failed=posix_spawnp(&child,args[0],&actions,NULL,args,environ);
            posix_spawn_file_actions_destroy(&actions);
            int status=0;if(!failed){while(waitpid(child,&status,0)<0)if(errno!=EINTR){failed=96;break;}}
            if(!failed&&(!WIFEXITED(status)||WEXITSTATUS(status)))failed=97;
            char bytes[32]={0};fd=open(output,O_RDONLY);ssize_t n=fd<0?-1:read(fd,bytes,sizeof(bytes));if(fd>=0)close(fd);
            if(!failed&&(n!=13||memcmp(bytes,"output\nerror\n",13)))failed=98;
            if(!failed&&unlink(relative))failed=99;
        }
        char byte=0;write(wake[1],&byte,1);pthread_join(thread,NULL);close(wake[0]);close(wake[1]);close(dir);
        unlink(input);unlink(output);unlink(relative);rmdir(directory);
        if(failed){printf("LINUX_SPAWN_FILES_ERROR=%d\n",failed);return 100;}
        posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
        if(posix_spawn_file_actions_addclose(&actions,-1)!=EBADF||posix_spawn_file_actions_adddup2(&actions,0,-1)!=EBADF)return 101;
        for(int i=3;i<32;i++)posix_spawn_file_actions_addclose(&actions,i);
        posix_spawn_file_actions_addopen(&actions,1,"/nonexistent/hish/output",O_WRONLY,0);
        char* args[]={argv[2],"--echo","UNEXPECTED_SPAWN",NULL};pid_t child=-7;
        int result=posix_spawnp(&child,args[0],&actions,NULL,args,environ);posix_spawn_file_actions_destroy(&actions);
        if(result!=ENOENT||child!=-7||waitpid(-1,NULL,WNOHANG)!=-1||errno!=ECHILD)return 102;
        puts("LINUX_SPAWN_FILES_THREADED_OK");return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--system")) {
        if(!system(NULL))return 82;int status=system(argv[2]);
        return status>=0&&WIFEXITED(status)?WEXITSTATUS(status):83;
    }
    if(argc==3&&!strcmp(argv[1],"--execl")) {
        execl(argv[2],argv[2],"--echo","LINUX_EXECL_OK",(char*)NULL);return 84;
    }
    if(argc==3&&!strcmp(argv[1],"--threaded-exec")) {
        // Exercise the explicitly selected direct-jump fallback's boundary.
        setenv("HISH_KERNEL_EXEC_BRIDGE","0",1);
        int pipefd[2];pthread_t thread;
        if(pipe(pipefd)||pthread_create(&thread,NULL,blocked_worker,&pipefd[0]))return 67;
        char* args[]={argv[2],"--echo","UNEXPECTED_MULTITHREAD_EXEC",NULL};
        execv(argv[2],args);const int rejected=errno==ENOTSUP;
        const char byte=0;write(pipefd[1],&byte,1);pthread_join(thread,NULL);close(pipefd[0]);close(pipefd[1]);
        if(!rejected)return 68;
        puts("LINUX_THREADED_EXEC_REJECTED_OK");return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--bridge-contract")) {
        int wake[2],fd=memfd_create("hish-old-image-contract",MFD_CLOEXEC);pthread_t thread;
        if(fd<0||ftruncate(fd,4096)||mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED||pipe(wake)||pthread_create(&thread,NULL,blocked_worker,&wake[0]))return 114;
        close(fd);
        char pid[24],uid[24];snprintf(pid,sizeof(pid),"%d",getpid());snprintf(uid,sizeof(uid),"%u",(unsigned)getuid());
        char* args[]={argv[2],"--assert-bridge",pid,uid,NULL};execvp(argv[2],args);
        int saved=errno;char byte=0;write(wake[1],&byte,1);pthread_join(thread,NULL);close(wake[0]);close(wake[1]);
        printf("LINUX_BRIDGE_EXEC_ERROR=%d\n",saved);return 115;
    }
    if(argc==4&&!strcmp(argv[1],"--assert-bridge")) {
        if(getpid()!=atoi(argv[2])||getuid()!=(uid_t)strtoul(argv[3],NULL,10))return 116;
        DIR* tasks=opendir("/proc/self/task");if(!tasks)return 117;
        struct dirent* entry;int count=0;while((entry=readdir(tasks)))if(entry->d_name[0]>='0'&&entry->d_name[0]<='9')count++;closedir(tasks);
        if(count!=1)return 118;
        FILE* maps=fopen("/proc/self/maps","r");if(!maps)return 119;
        char line[1024];int old=0;while(fgets(line,sizeof(line),maps))if(strstr(line,"hish-old-image-contract"))old=1;fclose(maps);
        if(old)return 120;
        FILE* domain=fopen("/proc/self/attr/current","r");char security[256]={0};if(domain){if(fgets(security,sizeof(security),domain))printf("LINUX_BRIDGE_DOMAIN=%s\n",security);fclose(domain);}
        puts("LINUX_KERNEL_BRIDGE_PID_THREADS_MAPS_OK");return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--exec-contract")) {
        const int closed=open("/dev/null",O_RDONLY|O_CLOEXEC),retained=open("/dev/null",O_RDONLY);
        if(closed<0||retained<0)return 69;
        signal(SIGUSR1,received);signal(SIGUSR2,SIG_IGN);
        sigset_t block;sigemptyset(&block);sigaddset(&block,SIGALRM);sigprocmask(SIG_BLOCK,&block,NULL);
        setenv("HISH_EXEC_TOKEN","contract",1);
        char a[24],b[24],pid[24];snprintf(a,sizeof(a),"%d",closed);snprintf(b,sizeof(b),"%d",retained);snprintf(pid,sizeof(pid),"%d",getpid());
        char* args[]={argv[2],"--assert-contract",a,b,pid,NULL};execv(argv[2],args);return 70;
    }
    if(argc==3&&!strcmp(argv[1],"--exec-stdio")) {
        int flags=fcntl(0,F_GETFD);if(flags<0||fcntl(0,F_SETFD,flags|FD_CLOEXEC))return 121;
        char* args[]={argv[2],"--assert-closed-stdin",NULL};execv(argv[2],args);return 122;
    }
    if(argc==2&&!strcmp(argv[1],"--assert-closed-stdin")) {
        if(fcntl(0,F_GETFD)!=-1||errno!=EBADF)return 123;
        puts("LINUX_EXEC_STDIO_CLOEXEC_OK");return 0;
    }
    if(argc==5&&!strcmp(argv[1],"--assert-contract")) {
        struct sigaction a,b;sigset_t mask;sigaction(SIGUSR1,NULL,&a);sigaction(SIGUSR2,NULL,&b);sigprocmask(SIG_SETMASK,NULL,&mask);
        const char* token=getenv("HISH_EXEC_TOKEN");
        const char* executable=(const char*)getauxval(AT_EXECFN);
        if(!executable||strcmp(executable,argv[0])||!token||strcmp(token,"contract")||getpid()!=atoi(argv[4])||fcntl(atoi(argv[2]),F_GETFD)!=-1||errno!=EBADF||
            fcntl(atoi(argv[3]),F_GETFD)<0||a.sa_handler!=SIG_DFL||b.sa_handler!=SIG_IGN||!sigismember(&mask,SIGALRM))return 71;
        close(atoi(argv[3]));puts("LINUX_EXEC_CONTRACT_OK");return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--orphan")) {
        const pid_t child = fork();
        if (child < 0) return 65;
        if (!child) {
            if (setsid() < 0) _exit(66);
            printf("LINUX_DETACHED_PID=%d\n", getpid()); fflush(stdout);
            sleep(30); _exit(0);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--exec")) {
        execv(argv[2], argv + 2); perror("execv"); return 63;
    }
    if (argc >= 3 && !strcmp(argv[1], "--exec-search")) {
        execvp(argv[2], argv + 2); perror("execvp"); return 64;
    }
    if (argc == 3 && !strcmp(argv[1], "--echo")) { puts(argv[2]); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--exit")) return atoi(argv[2]);
    printf("LINUX_ABI_MAIN argc=%d tls=%d uid=%u\n", argc, local_value, (unsigned)getuid()); fflush(stdout);
    if (argc != 1 || local_value != 42) return 51;
    main_local = &local_value;
    char* memory = malloc(8192);
    if (!memory) return 52;
    memset(memory, 0x35, 8192);
    if (memory[8191] != 0x35) return 52;
    free(memory);
    errno = EINVAL;
    if (errno != EINVAL) return 53;
    puts("LINUX_ABI_MALLOC_OK"); fflush(stdout);
    pthread_t thread;
    puts("LINUX_ABI_THREAD_BEGIN"); fflush(stdout);
    const int thread_error = pthread_create(&thread, NULL, worker, NULL);
    if (thread_error) { printf("LINUX_ABI_THREAD_ERROR=%d\n", thread_error); return 57; }
    void* result = NULL;
    if (pthread_join(thread, &result) || result != (void*)99 || local_value != 42) return 58;
    puts("LINUX_ABI_REAL_TLS_OK"); fflush(stdout);
    struct sigaction action = {0}; action.sa_handler = received; sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) || raise(SIGUSR1) || signal_seen != SIGUSR1) return 59;
    puts("LINUX_ABI_SIGNAL_OK"); fflush(stdout);
    int pipefd[2];
    if (pipe(pipefd)) return 60;
    puts("LINUX_ABI_FORK_BEGIN"); fflush(stdout);
    const pid_t child = fork();
    if (child < 0) return 60;
    if (!child) { close(pipefd[0]); const char byte='k'; const int ok=write(pipefd[1], &byte, 1)==1; _exit(ok?17:18); }
    close(pipefd[1]); char byte=0; const int read_ok=read(pipefd[0], &byte, 1)==1 && byte=='k'; close(pipefd[0]);
    int status=0; while(waitpid(child,&status,0)<0) if(errno!=EINTR)return 60;
    if(!read_ok || !WIFEXITED(status) || WEXITSTATUS(status)!=17) return 60;
    puts("LINUX_ABI_FORK_PIPE_WAIT_OK"); fflush(stdout);
    struct timespec now; unsigned char random[16];
    if(clock_gettime(CLOCK_MONOTONIC,&now) || getrandom(random,sizeof(random),0)!=sizeof(random)) return 61;
    int pair[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return 62; close(pair[0]); close(pair[1]);
    puts("LINUX_ABI_CLOCK_RANDOM_SOCKET_OK"); fflush(stdout);
    const char* home=getenv("HOME"); if(!home)return 54;
    char name[512]; snprintf(name,sizeof(name),"%s/.hish-linux-%d",home,getpid());
    FILE* file=fopen(name,"w+"); if(!file)return 55;
    const char text[]="native Linux libc file IO";char back[sizeof(text)]={0};
    const int wrote=fwrite(text,1,sizeof(text),file)==sizeof(text),seeked=fseek(file,0,SEEK_SET)==0;
    const int readback=fread(back,1,sizeof(back),file)==sizeof(back);fclose(file);
    const int cleaned=unlink(name)==0;
    if(!wrote||!seeked||!readback||!cleaned||memcmp(back,text,sizeof(text)))return 56;
    puts("LINUX_ABI_STORAGE_OK\nLINUX_ABI_NATIVE_END");fflush(stdout);
    return 42;
}
