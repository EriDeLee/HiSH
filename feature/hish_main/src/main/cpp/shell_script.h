#pragma once
#include <errno.h>
#include <stddef.h>
#include <string.h>
// Mutates the bounded first-line buffer. Linux shebangs carry one optional
// argument; preserve it intact instead of discarding flags such as sh -e.
static inline int HishShellScript(char* header,size_t count,const char** shell,char** option)
{
    *shell="sh";*option=NULL;
    if(count<2||header[0]!='#'||header[1]!='!')return 0;
    char* end=(char*)memchr(header,'\n',count);if(!end&&count==511){errno=ENOEXEC;return -1;}
    if(!end)end=header+count;
    while(end>header+2&&(end[-1]=='\r'||end[-1]==' '||end[-1]=='\t'))end--;*end=0;
    char* start=header+2;while(*start==' '||*start=='\t')start++;
    char* next=start;while(*next&&*next!=' '&&*next!='\t')next++;
    if(*next){*next++=0;while(*next==' '||*next=='\t')next++;if(*next)*option=next;}
    char* name=strrchr(start,'/');name=name?name+1:start;
    if(!strcmp(name,"env")){
        // Plain env sh is supported. env -S needs env's own option/splitting
        // contract and must not silently become an invocation of plain sh.
        if(!*option){errno=ENOEXEC;return -1;}name=*option;*option=NULL;
    }
    if(strcmp(name,"sh")&&strcmp(name,"mksh")){errno=ENOEXEC;return -1;}
    *shell=name;return 1;
}
