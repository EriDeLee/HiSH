// A real guest program for pkg doctor: rootfs reads, fork, exec and wait.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc,char **argv){
    char text[32]={0};FILE *f=fopen("/hish-marker","r");if(!f)return 10;if(!fgets(text,sizeof text,f)){fclose(f);return 11;}fclose(f);if(strcmp(text,"HISH_ROOTFS_42\n"))return 12;
    if(argc==2&&!strcmp(argv[1],"child")){puts("PROOT_GUEST_CHILD_PASS");return 0;}
    pid_t pid=fork();if(pid<0)return 13;if(!pid){execl("/bin/probe","probe","child",NULL);_exit(14);}
    int status=0;if(waitpid(pid,&status,0)!=pid||!WIFEXITED(status)||WEXITSTATUS(status))return 15;puts("PROOT_ROOTFS_FORK_EXEC_PASS");return 0;
}
