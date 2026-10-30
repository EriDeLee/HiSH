#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <errno.h>
static inline const char* HishImageName(const char* file,char* buffer,size_t capacity)
{
    // Resolve regular relative paths as well as symlinks. Cooperative procfs
    // readers require an absolute identity, even when invoked as ./program.
    const int saved=errno;
    const int fd=syscall(SYS_openat,AT_FDCWD,file,O_PATH|O_CLOEXEC,0);
    if(fd>=0){
        char descriptor[64];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
        ssize_t n=syscall(SYS_readlinkat,AT_FDCWD,descriptor,buffer,capacity-1);
        syscall(SYS_close,fd);errno=saved;
        if(n>0&&(size_t)n<capacity-1&&buffer[0]=='/'){buffer[n]=0;return buffer;}
    }
    errno=saved;
    return file;
}
// Logical image identity for cooperative libc calls. Raw procfs/syscalls still
// identify the kernel bootstrap; no kernel metadata or permission is changed.
static inline const char* HishExecutablePath(const char* path)
{
    if(strncmp(path,"/proc/",6))return path;
    char own[64];snprintf(own,sizeof(own),"/proc/%d/exe",getpid());
    if(strcmp(path,"/proc/self/exe")&&strcmp(path,"/proc/thread-self/exe")&&strcmp(path,own))return path;
    const char* image=getenv("HISH_EXECUTABLE");return image&&image[0]=='/'?image:path;
}
