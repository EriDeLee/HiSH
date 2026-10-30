#ifndef HISH_LINUX_LIBRARY_PATH_H
#define HISH_LINUX_LIBRARY_PATH_H
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Lookup policy, not a sandbox for arbitrary code already running as this UID.
// Keep the existing native layout and never send this search list to OHOS.
#ifndef HISH_PRIVATE_NATIVE_ROOT
#define HISH_PRIVATE_NATIVE_ROOT "/data/storage/el2/base/files/native"
#endif
#define HISH_LIBRARY_PATH_LIMIT 32768

static inline const char* HishEnvironmentValue(char* const environment[], const char* name)
{
    const size_t size = strlen(name);
    for (size_t i = 0; environment && environment[i] && i < 4096; i++)
        if (!strncmp(environment[i], name, size) && environment[i][size] == '=') return environment[i] + size + 1;
    return NULL;
}
static inline int HishPathWithin(const char* path, const char* root)
{
    const size_t size = strlen(root);
    return !strncmp(path, root, size) && (!path[size] || path[size] == '/');
}
static inline int HishAppendLibraryPath(char* output, size_t capacity, const char* path)
{
    const size_t size = strlen(path), used = strlen(output);
    for (const char* at = output; *at;) {
        const char* end = strchr(at, ':');
        const size_t length = end ? (size_t)(end - at) : strlen(at);
        if (length == size && !memcmp(at, path, size)) return 0;
        if (!end) break;
        at = end + 1;
    }
    if (used >= capacity || size + (used != 0) >= capacity - used) { errno = E2BIG; return -1; }
    if (used) output[used] = ':';
    memcpy(output + used + (used != 0), path, size + 1);
    return 0;
}
static inline int HishPrivateLibraryDirectory(char* output, size_t capacity,
    const char* path, const char* nativeRoot, const char* installed)
{
    char canonical[PATH_MAX];
    if (*path != '/') { errno = EINVAL; return -1; }
    if (!realpath(path, canonical)) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    struct stat info;
    if (stat(canonical, &info)) return -1;
    if (!S_ISDIR(info.st_mode)) { errno = ENOTDIR; return -1; }
    if (strcmp(canonical, installed) && !HishPathWithin(canonical, nativeRoot)) { errno = EACCES; return -1; }
    return HishAppendLibraryPath(output, capacity, canonical);
}
// HAP runtime first; explicit app-private paths next; musl's own default last.
// glibc and OHOS deliberately retain their existing search policy this round.
static inline int HishMuslLibraryPath(char* output, size_t capacity,
    const char* installed, char* const environment[])
{
    char nativeRoot[PATH_MAX], installedRoot[PATH_MAX], directory[PATH_MAX];
    if (!capacity) { errno = E2BIG; return -1; }
    output[0] = 0;
    if (!realpath(installed, installedRoot) || !realpath(HISH_PRIVATE_NATIVE_ROOT, nativeRoot)) return -1;
    if (HishAppendLibraryPath(output, capacity, installedRoot)) return -1;
    const char* search = HishEnvironmentValue(environment, "LD_LIBRARY_PATH");
    if (search && strnlen(search, HISH_LIBRARY_PATH_LIMIT) >= HISH_LIBRARY_PATH_LIMIT) { errno = E2BIG; return -1; }
    for (const char* at = search; at && *at;) {
        const char* end = strchr(at, ':');
        const size_t size = end ? (size_t)(end - at) : strlen(at);
        if (!size || size >= sizeof(directory)) { errno = EINVAL; return -1; }
        memcpy(directory, at, size); directory[size] = 0;
        if (HishPrivateLibraryDirectory(output, capacity, directory, nativeRoot, installedRoot)) return -1;
        if (!end) break;
        at = end + 1;
        if (!*at) { errno = EINVAL; return -1; }
    }
    // Ordinary downloaded packages retain upstream SONAMEs and their own
    // standard layout. The HAP's ABI libraries still take precedence.
    const char* packageSuffixes[] = {"/packages/root/usr/lib", "/packages/root/lib"};
    for(size_t i=0;i<sizeof(packageSuffixes)/sizeof(packageSuffixes[0]);i++){
        if(strlen(nativeRoot)+strlen(packageSuffixes[i])>=sizeof(directory)){errno=ENAMETOOLONG;return -1;}
        strcpy(directory,nativeRoot);strcat(directory,packageSuffixes[i]);
        if(HishPrivateLibraryDirectory(output,capacity,directory,nativeRoot,installedRoot))return -1;
    }
    const char* suffix = "/usr/lib/musl";
    if (strlen(nativeRoot) + strlen(suffix) >= sizeof(directory)) { errno = ENAMETOOLONG; return -1; }
    strcpy(directory, nativeRoot); strcat(directory, suffix);
    return HishPrivateLibraryDirectory(output, capacity, directory, nativeRoot, installedRoot);
}
#endif
