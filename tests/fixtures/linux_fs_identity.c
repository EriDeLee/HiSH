#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/fsuid.h>
#include <unistd.h>

static int observed(unsigned ids[8])
{
    FILE* file=fopen("/proc/self/status","r");if(!file)return -1;
    char line[512];int found=0;
    while(fgets(line,sizeof(line),file)){
        if(sscanf(line,"Uid: %u %u %u %u",ids,ids+1,ids+2,ids+3)==4)found|=1;
        if(sscanf(line,"Gid: %u %u %u %u",ids+4,ids+5,ids+6,ids+7)==4)found|=2;
    }
    fclose(file);return found==3?0:-1;
}
int main(void)
{
    unsigned before[8],after[8];if(observed(before))return 1;
    const uid_t uid=before[3];const gid_t gid=before[7];
    if((unsigned)setfsuid((uid_t)-1)!=uid||(unsigned)setfsgid((gid_t)-1)!=gid)return 2;
    if((unsigned)setfsuid(uid)!=uid||(unsigned)setfsgid(gid)!=gid)return 3;
    if(observed(after)||memcmp(before,after,sizeof(before)))return 4;
    puts("FILESYSTEM_ID_QUERY_SAME_OK");
    if((unsigned)setfsuid(uid?0:1)!=uid||(unsigned)setfsgid(gid?0:1)!=gid)return 5;
    if(observed(after)||memcmp(before,after,sizeof(before)))return 6;
    if((unsigned)setfsuid(uid+1)!=uid||(unsigned)setfsgid(gid+1)!=gid)return 7;
    if(observed(after)||memcmp(before,after,sizeof(before)))return 8;
    printf("FILESYSTEM_ID_FOREIGN_REFUSED_OK uid=%u gid=%u\n",uid,gid);
    return 0;
}
