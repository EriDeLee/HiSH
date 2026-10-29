#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
static inline const char* HishImageName(const char* file,char* buffer,size_t capacity)
{
    ssize_t n=syscall(SYS_readlinkat,AT_FDCWD,file,buffer,capacity-1);
    if(n>0&&(size_t)n<capacity&&buffer[0]=='/'){buffer[n]=0;return buffer;}
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
