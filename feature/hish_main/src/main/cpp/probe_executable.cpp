#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int main()
{
    printf("NATIVE_ELF_OK pid=%d uid=%d HOME=%s\n", getpid(), getuid(), getenv("HOME") ? getenv("HOME") : "");
    return 42;
}
