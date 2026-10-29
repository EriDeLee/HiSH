#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/types.h>
#include <unistd.h>

// Ordinary apps cannot change credentials. Some Linux tools redundantly drop
// to the identity they already have; implement only that no-op in userspace.
// This never grants a different UID/GID or changes the kernel's access checks.
int setuid(uid_t id) { if(id==getuid()&&id==geteuid())return 0;errno=EPERM;return -1; }
int setgid(gid_t id) { if(id==getgid()&&id==getegid())return 0;errno=EPERM;return -1; }
int setresuid(uid_t real,uid_t effective,uid_t saved)
{
    const uid_t id=getuid();if(id==geteuid()&&(real==(uid_t)-1||real==id)&&(effective==(uid_t)-1||effective==id)&&(saved==(uid_t)-1||saved==id))return 0;
    errno=EPERM;return -1;
}
int setresgid(gid_t real,gid_t effective,gid_t saved)
{
    const gid_t id=getgid();if(id==getegid()&&(real==(gid_t)-1||real==id)&&(effective==(gid_t)-1||effective==id)&&(saved==(gid_t)-1||saved==id))return 0;
    errno=EPERM;return -1;
}

// Linux returns the previous filesystem ID even when a change is refused.
// Read that actual ID, not an assumed effective ID. Never invoke the forbidden
// setfs* syscall, grant a new identity, or claim that a different ID was set.
static int filesystem_id(const char* field, unsigned* value)
{
    const int fd=open("/proc/self/status",O_RDONLY|O_CLOEXEC);
    if(fd<0)return -1;
    char bytes[8192];size_t used=0;
    while(used<sizeof(bytes)-1){
        const ssize_t n=read(fd,bytes+used,sizeof(bytes)-1-used);
        if(n<0&&errno==EINTR)continue;
        if(n<0){const int saved=errno;close(fd);errno=saved;return -1;}
        if(!n)break;used+=(size_t)n;
    }
    close(fd);bytes[used]=0;
    const size_t length=strlen(field);
    for(char* line=bytes;line&&*line;){
        char* end=strchr(line,'\n');if(end)*end=0;
        if(!strncmp(line,field,length)){
            const char* p=line+length;unsigned number=0;
            for(int i=0;i<4;i++){
                while(*p==' '||*p=='\t')p++;
                if(*p<'0'||*p>'9'){errno=EIO;return -1;}
                number=0;
                while(*p>='0'&&*p<='9'){
                    const unsigned digit=*p++-'0';
                    if(number>(UINT_MAX-digit)/10){errno=EOVERFLOW;return -1;}
                    number=number*10+digit;
                }
            }
            *value=number;return 0;
        }
        line=end?end+1:NULL;
    }
    errno=EIO;return -1;
}
int setfsuid(uid_t requested)
{
    unsigned current;const int saved=errno;
    if(filesystem_id("Uid:",&current))return -1;
    errno=(requested==(uid_t)-1||requested==current)?saved:EPERM;
    return (int)current;
}
int setfsgid(gid_t requested)
{
    unsigned current;const int saved=errno;
    if(filesystem_id("Gid:",&current))return -1;
    errno=(requested==(gid_t)-1||requested==current)?saved:EPERM;
    return (int)current;
}
