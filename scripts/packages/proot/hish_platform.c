/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <talloc.h>
#include "hish_platform.h"
#include "execve/aoxp.h"
#include "execve/execve.h"
#include "tracee/mem.h"
#include "path/binding.h"
static const char *native_dir(void){const char *p=getenv("HISH_NATIVE_LIBDIR");return p&&p[0]=='/'?p:NULL;}
int hish_platform_path(const char *path,char *result){if(strcmp(path,"/etc/ld-musl-namespace-aarch64.ini"))return 0;strcpy(result,path);return 1;}
int hish_platform_bindings(Tracee *t){
    const char *p=native_dir();if(!p)return -ENOTSUP;
    if(!new_binding(t,"/system","/system",true)||!new_binding(t,p,p,true))return -EINVAL;
    return 0;
}
int hish_prepare_exec(Tracee *t){
    const char *lib=native_dir();if(!lib)return -ENOTSUP;
    ArrayOfXPointers *env;int status=fetch_array_of_xpointers(t,&env,SYSARG_3,0);if(status<0)return status;
    char **values=talloc_zero_array(t->ctx,char*,env->length+8);if(!values)return -ENOMEM;size_t count=0;
    for(size_t i=0;i+1<env->length;i++){
        char *v;status=read_xpointee_as_string(env,i,&v);if(status<0)return status;
        if(!strncmp(v,"LD_",3)||!strncmp(v,"HISH_PROOT_",11)||!strncmp(v,"HISH_EXEC_BRIDGE_FD=",20))continue;
        values[count++]=v;
    }
    values[count++]=talloc_asprintf(t->ctx,"LD_PRELOAD=%s/libhish_exec_bridge.so",lib);
    values[count++]=talloc_asprintf(t->ctx,"LD_LIBRARY_PATH=%s:/system/lib64/ndk",lib);
    values[count++]=talloc_strdup(t->ctx,"HISH_PROOT_ACTIVE=1");
    values[count++]=talloc_asprintf(t->ctx,"HISH_PROOT_LIBC=%s",t->load_info->interp&&strstr(t->load_info->interp->host_path,"glibc")?"glibc":"musl");
    char *space=talloc_array(t->ctx,char,65536);if(!space)return -ENOMEM;memset(space,'x',65535);space[65535]=0;memcpy(space,"HISH_PROOT_SCRIPT=",18);values[count++]=space;
    status=resize_array_of_xpointers(env,env->length-1,(ssize_t)(count+1-env->length));if(status<0)return status;
    for(size_t i=0;i<count;i++){if(!values[i])return -ENOMEM;status=write_xpointee(env,i,values[i]);if(status<0)return status;}
    return push_array_of_xpointers(env,SYSARG_3);
}
static word_t slot_address(Tracee *t,word_t sp){
    word_t argc=peek_word(t,sp);if(argc>1024)return 0;word_t env=sp+(argc+2)*sizeof(word_t);
    for(size_t i=0;i<4096;i++){word_t p=peek_word(t,env+i*sizeof(word_t));if(!p)return 0;char name[18];if(read_data(t,name,p,sizeof name)<0)return 0;if(!memcmp(name,"HISH_PROOT_SCRIPT=",18))return p;}
    return 0;
}
word_t hish_script_end(Tracee *t,word_t sp){word_t p=slot_address(t,sp);return p?(p+65536)&~(word_t)15:0;}
int hish_publish_script(Tracee *t,word_t sp,word_t address,const void *data,size_t size){
    word_t p=slot_address(t,sp);if(!p||size>65536-64||address<p+64||address>p+65536-size)return -E2BIG;
    int status=write_data(t,address,data,size);if(status<0)return status;char header[64];snprintf(header,sizeof header,"HISH_PROOT_SCRIPT=%016lx:%08zx",address,size);return write_data(t,p,header,strlen(header)+1);
}
