#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
static _Thread_local int audit_tls = 41;
__attribute__((constructor)) static void initialize(void) { audit_tls++; }
static void* worker(void* ignored)
{
    (void)ignored;
    if (audit_tls != 41) return (void*)1;
    audit_tls = 99;
    return (void*)99;
}
int main(int argc, char** argv)
{
    (void)argv;
    printf("STATIC_C_MAIN argc=%d tls=%d\n", argc, audit_tls); fflush(stdout);
    if (argc != 1 || audit_tls != 42) return 51;
    char* memory = malloc(8192);
    if (!memory) return 52;
    memset(memory, 0x35, 8192);
    if (memory[8191] != 0x35) return 52;
    free(memory);
    errno = EINVAL;
    if (errno != EINVAL) return 53;
    printf("STATIC_C_MALLOC_TLS_INIT_OK\n"); fflush(stdout);
    pthread_t thread;
    printf("STATIC_C_THREAD_BEGIN\n"); fflush(stdout);
    if (pthread_create(&thread, NULL, worker, NULL) != 0) return 57;
    void* result = NULL;
    if (pthread_join(thread, &result) != 0 || result != (void*)99 || audit_tls != 42) return 58;
    printf("STATIC_C_THREAD_TLS_OK\n"); fflush(stdout);
    const char* home = getenv("HOME");
    if (!home) return 54;
    char name[512]; snprintf(name, sizeof(name), "%s/.hish-static-%d", home, getpid());
    FILE* file = fopen(name, "w+");
    if (!file) return 55;
    const char text[] = "static native file IO";
    const int wrote = fwrite(text, 1, sizeof(text), file) == sizeof(text);
    const int seeked = fseek(file, 0, SEEK_SET) == 0;
    char back[sizeof(text)] = {0};
    const int readback = fread(back, 1, sizeof(back), file) == sizeof(back);
    fclose(file);
    const int cleaned = unlink(name) == 0;
    if (!wrote || !seeked || !readback || !cleaned || memcmp(back, text, sizeof(text))) return 56;
    printf("STATIC_C_STORAGE_OK\nSTATIC_C_NATIVE_END\n"); fflush(stdout);
    return 42;
}
