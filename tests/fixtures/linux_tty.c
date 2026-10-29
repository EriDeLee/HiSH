#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
static volatile sig_atomic_t resized;
static volatile sig_atomic_t interrupted,quit;
static void winch(int signal){(void)signal;resized=1;}
static void caught(int signal){if(signal==SIGINT)interrupted=1;if(signal==SIGQUIT)quit=1;}
int main(void)
{
    if(!isatty(0)||!isatty(1)||!isatty(2)){puts("TTY_ASSERT_ISATTY_FAIL");return 71;}
    int tty=open("/dev/tty",O_RDWR|O_CLOEXEC),pipefd[2];
    if(tty<0||!isatty(tty)||!(fcntl(tty,F_GETFD)&FD_CLOEXEC)||pipe(pipefd)){puts("TTY_ASSERT_DESCRIPTOR_FAIL");return 78;}
    if(isatty(pipefd[0])||isatty(pipefd[1])){puts("TTY_ASSERT_PIPE_FAIL");return 79;}
    close(tty);close(pipefd[0]);close(pipefd[1]);
    const pid_t shell_foreground=tcgetpgrp(0);
    if(setpgid(0,0)||tcsetpgrp(0,getpgrp())){puts("TTY_ASSERT_FOREGROUND_FAIL");return 89;}
    struct termios saved,raw;struct winsize size;
    if(tcgetattr(0,&saved)||ioctl(0,TIOCGWINSZ,&size)||size.ws_col!=80||size.ws_row!=24){puts("TTY_ASSERT_INITIAL_FAIL");return 72;}
    struct sigaction action={0};action.sa_handler=winch;sigemptyset(&action.sa_mask);sigaction(SIGWINCH,&action,NULL);
    raw=saved;cfmakeraw(&raw);if(tcsetattr(0,TCSANOW,&raw)){puts("TTY_ASSERT_RAW_FAIL");return 73;}
    puts("TTY_RAW_READY");fflush(stdout);
    const unsigned char expected[]={'a',27,'[','A',3};unsigned char input[sizeof(expected)];size_t used=0;
    while(used<sizeof(input)){ssize_t n=read(0,input+used,sizeof(input)-used);if(n<0&&errno==EINTR)continue;if(n<=0)return 74;used+=n;}
    if(memcmp(input,expected,sizeof(input))){puts("TTY_ASSERT_BYTES_FAIL");return 75;}
    if(ioctl(0,TIOCGWINSZ,&size)||size.ws_col!=132||size.ws_row!=43||!resized){puts("TTY_ASSERT_RESIZE_FAIL");return 76;}
    puts("TTY_RAW_BYTES_RESIZE_OK");
    pid_t session=-1;
    if(tcgetsid(0)!=getsid(0)||ioctl(0,TIOCGSID,&session)||session!=getsid(0)){puts("TTY_ASSERT_SESSION_FAIL");return 80;}
    resized=0;size.ws_col=100;size.ws_row=30;
    if(ioctl(0,TIOCSWINSZ,&size)){puts("TTY_ASSERT_IOCTL_RESIZE_FAIL");return 81;}
    for(int i=0;i<100&&!resized;i++)usleep(1000);
    if(!resized){puts("TTY_ASSERT_IOCTL_WINCH_FAIL");return 82;}
    raw.c_lflag|=ISIG;raw.c_cc[VINTR]=2;
    action.sa_handler=caught;sigaction(SIGINT,&action,NULL);sigaction(SIGQUIT,&action,NULL);
    if(tcsetattr(0,TCSANOW,&raw))return 83;
    puts("TTY_SIGNAL_READY");fflush(stdout);
    for(int i=0;i<500&&(!interrupted||!quit);i++)usleep(10000);
    if(!interrupted||!quit){puts("TTY_ASSERT_SIGNAL_FAIL");return 84;}
    puts("TTY_CUSTOM_SIGNALS_OK");fflush(stdout);
    const pid_t foreground=tcgetpgrp(0);pid_t child=fork();if(child<0)return 85;
    if(!child){
        signal(SIGINT,SIG_DFL);signal(SIGQUIT,SIG_DFL);signal(SIGTSTP,SIG_DFL);
        if(setpgid(0,0)||tcsetpgrp(0,getpgrp()))_exit(86);
        puts("TTY_JOB_READY");fflush(stdout);
        // SIGTSTP stops the actual kernel process, SIGCONT resumes it.
        pause();_exit(87);
    }
    int status=0;pid_t waited;do{waited=waitpid(child,&status,WUNTRACED);}while(waited<0&&errno==EINTR);
    int stopped=waited==child&&WIFSTOPPED(status)&&(WSTOPSIG(status)==SIGTSTP||WSTOPSIG(status)==SIGSTOP);
    printf("TTY_JOB_WAIT waited=%d child=%d status=%d stopped=%d errno=%d\n",waited,child,status,stopped,errno);fflush(stdout);
    if(stopped&&WSTOPSIG(status)!=SIGTSTP){puts("TTY_STOP_REASON_SIGSTOP_OBSERVED");fflush(stdout);}
    if(stopped){kill(child,SIGCONT);puts("TTY_JOB_CONTINUED");fflush(stdout);}
    else kill(child,SIGKILL);
    do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    if(tcsetpgrp(0,foreground)||!stopped||waited!=child||!WIFSIGNALED(status)||WTERMSIG(status)!=SIGINT){puts("TTY_ASSERT_JOB_CONTROL_FAIL");return 88;}
    puts("TTY_FOREGROUND_STOP_CONTINUE_INTERRUPT_OK");
    if(tcsetattr(0,TCSANOW,&saved))return 77;
    if(tcsetpgrp(0,shell_foreground)){puts("TTY_ASSERT_SHELL_FOREGROUND_FAIL");return 90;}
    puts("TTY_RESTORED");fflush(stdout);return 0;
}
