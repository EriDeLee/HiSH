#ifndef HISH_ELF_CODE_MAPPING_H
#define HISH_ELF_CODE_MAPPING_H
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef HISH_ELF_DIAGNOSTICS_ENV
#define HISH_ELF_DIAGNOSTICS_ENV "HISH_ELF_DIAGNOSTICS"
#endif

// The caller owns/reserves the complete target range and validates PT_LOAD.
// Keep permitted file mappings fast. When file RX is denied, use the app's
// separately granted anonymous-code capability: RW -> copy -> RX, never W+X.
// snapshot is the already validated file-page bytes, or NULL for exact pread.
static inline void* HishMapELFCode(void* target, size_t length, int protection,
    int fd, off_t offset, size_t bytes, const void* snapshot, const char** stage)
{
    *stage = "map ELF code";
    if (!length || bytes > length || offset < 0 || !(protection & PROT_EXEC) || (protection & PROT_WRITE)) {
        errno = ENOEXEC; return MAP_FAILED;
    }
    void* memory = mmap(target, length, protection, MAP_PRIVATE | MAP_FIXED, fd, offset);
    if (memory != MAP_FAILED || (errno != EACCES && errno != EPERM)) return memory;
    *stage = "allocate anonymous ELF code";
    memory = mmap(target, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (memory == MAP_FAILED) return memory;
    *stage = "read anonymous ELF code";
    if (snapshot) memcpy(memory, snapshot, bytes);
    else for (size_t at = 0; at < bytes;) {
        const ssize_t count = pread(fd, (unsigned char*)memory + at, bytes - at, offset + at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = ENOEXEC; return MAP_FAILED; }
        at += (size_t)count;
    }
    __builtin___clear_cache((char*)memory, (char*)memory + length);
    *stage = "protect anonymous ELF code RX";
    if (mprotect(memory, length, protection)) return MAP_FAILED;
    if (getenv(HISH_ELF_DIAGNOSTICS_ENV))
        dprintf(2, "HISH_ELF_ANONYMOUS_RX address=%p size=%zu file_bytes=%zu\n", memory, length, bytes);
    return memory;
}
#endif
