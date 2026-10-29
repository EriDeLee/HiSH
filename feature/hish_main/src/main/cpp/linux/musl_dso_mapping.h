#ifndef HISH_MUSL_DSO_MAPPING_H
#define HISH_MUSL_DSO_MAPPING_H
// DSO mapping may occur after posix_spawn redirects stderr into application
// data. Opt in separately; generic exec diagnostics must not add DSO output.
#define HISH_ELF_DIAGNOSTICS_ENV "HISH_MUSL_DSO_DIAGNOSTICS"
#include "elf_code_mapping.h"
#include <elf.h>
#include <sys/stat.h>

// Included only by the source-built Linux musl ldso, after its early bootstrap.
// The upstream linker still owns symbols, relocations, TLS and constructors.
static int HishMuslDSOHeader(int fd, const Elf64_Ehdr* eh)
{
    struct stat info;
    if (fstat(fd, &info)) return -1;
    if (!S_ISREG(info.st_mode) || info.st_size < (off_t)sizeof(*eh) || info.st_size > 64 * 1024 * 1024 ||
        memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_ident[EI_VERSION] != EV_CURRENT ||
        eh->e_version != EV_CURRENT || eh->e_machine != EM_AARCH64 || eh->e_type != ET_DYN ||
        eh->e_phentsize != sizeof(Elf64_Phdr) || !eh->e_phnum || eh->e_phnum > 128 ||
        eh->e_phoff > (uint64_t)info.st_size ||
        (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > (uint64_t)info.st_size - eh->e_phoff) {
        errno = ENOEXEC; return -1;
    }
    return 0;
}
static int HishMuslDSORead(int fd, void* target, size_t bytes, off_t offset)
{
    for (size_t at = 0; at < bytes;) {
        ssize_t n = pread(fd, (char*)target + at, bytes - at, offset + at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ENOEXEC; return -1; }
        at += (size_t)n;
    }
    return 0;
}
static int HishMuslDSOContains(const Elf64_Phdr* ph, size_t count, uint64_t address, uint64_t bytes)
{
    for (size_t i = 0; i < count; i++) if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_R) &&
        address >= ph[i].p_vaddr && address - ph[i].p_vaddr <= ph[i].p_filesz &&
        bytes <= ph[i].p_filesz - (address - ph[i].p_vaddr)) return 1;
    return 0;
}
static int HishMuslDSODynamic(const unsigned char* base, const Elf64_Phdr* ph, size_t count)
{
    const Elf64_Phdr* dynamic = NULL;
    for (size_t i = 0; i < count; i++) if (ph[i].p_type == PT_DYNAMIC) {
        if (dynamic) { errno = ENOEXEC; return -1; }
        dynamic = ph + i;
    }
    if (!dynamic || dynamic->p_filesz < sizeof(Elf64_Dyn) || dynamic->p_filesz % sizeof(Elf64_Dyn) ||
        !HishMuslDSOContains(ph, count, dynamic->p_vaddr, dynamic->p_filesz)) { errno = ENOEXEC; return -1; }
    const Elf64_Dyn* entries = (const void*)(base + dynamic->p_vaddr);
    size_t length = dynamic->p_filesz / sizeof(*entries), end = 0;
    uint64_t strings = 0, stringSize = 0;
    for (; end < length && entries[end].d_tag; end++) {
        if (entries[end].d_tag == DT_TEXTREL ||
            (entries[end].d_tag == DT_FLAGS && (entries[end].d_un.d_val & DF_TEXTREL))) { errno = EACCES; return -1; }
        if (entries[end].d_tag == DT_STRTAB) strings = entries[end].d_un.d_ptr;
        if (entries[end].d_tag == DT_STRSZ) stringSize = entries[end].d_un.d_val;
    }
    if (end == length || !stringSize || !HishMuslDSOContains(ph, count, strings, stringSize)) { errno = ENOEXEC; return -1; }
    for (size_t i = 0; i < end; i++) if (entries[i].d_tag == DT_NEEDED) {
        const uint64_t offset = entries[i].d_un.d_val;
        if (offset >= stringSize) { errno = ENOEXEC; return -1; }
        const char* name = (const char*)base + strings + offset;
        if (!memchr(name, 0, stringSize - offset)) { errno = ENOEXEC; return -1; }
        // Detect explicit foreign libc dependencies before any relocations or
        // constructors. This is not a proof that every other DSO has a musl ABI.
        if (!strcmp(name, "libc.so.6") || !strcmp(name, "ld-linux-aarch64.so.1") ||
            !strcmp(name, "libc.so") || !strcmp(name, "libc++.so")) { errno = ELIBBAD; return -1; }
    }
    return 0;
}
static int HishMuslMapDSO(int fd, const Elf64_Ehdr* eh, const Elf64_Phdr* ph, size_t page,
    unsigned char** mapping, size_t* mapSize, unsigned char** loadBase)
{
    struct stat info;
    if (fstat(fd, &info)) return -1;
    if (page != 4096) { errno = ENOTSUP; return -1; }
    uint64_t low = UINT64_MAX, high = 0, alignment = page;
    for (size_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* p = ph + i;
        if (p->p_type == PT_GNU_STACK && (p->p_flags & PF_X)) { errno = EACCES; return -1; }
        if (p->p_type == PT_TLS && (!p->p_align || (p->p_align & (p->p_align - 1)) ||
            p->p_align > (1u << 24) || p->p_filesz > p->p_memsz || p->p_memsz > 64 * 1024 * 1024 ||
            !HishMuslDSOContains(ph, eh->e_phnum, p->p_vaddr, p->p_filesz))) { errno = ENOEXEC; return -1; }
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        if (p->p_filesz > p->p_memsz || p->p_offset > (uint64_t)info.st_size ||
            p->p_filesz > (uint64_t)info.st_size - p->p_offset || p->p_memsz > UINT64_MAX - page ||
            p->p_vaddr > UINT64_MAX - p->p_memsz - page || p->p_align > (1u << 24) ||
            (p->p_align && ((p->p_align & (p->p_align - 1)) || p->p_offset % p->p_align != p->p_vaddr % p->p_align)) ||
            p->p_offset % page != p->p_vaddr % page) { errno = ENOEXEC; return -1; }
        if ((p->p_flags & PF_X) && ((p->p_flags & PF_W) || p->p_filesz != p->p_memsz)) { errno = EACCES; return -1; }
        const uint64_t start = p->p_vaddr & -(uint64_t)page;
        const uint64_t end = (p->p_vaddr + p->p_memsz + page - 1) & -(uint64_t)page;
        for (size_t j = 0; j < i; j++) if (ph[j].p_type == PT_LOAD && ph[j].p_memsz) {
            const uint64_t other = ph[j].p_vaddr & -(uint64_t)page;
            const uint64_t otherEnd = (ph[j].p_vaddr + ph[j].p_memsz + page - 1) & -(uint64_t)page;
            if (start < otherEnd && end > other) { errno = ENOEXEC; return -1; }
        }
        if (start < low) low = start;
        if (end > high) high = end;
        if (p->p_align > alignment) alignment = p->p_align;
    }
    if (high <= low || high - low > 256 * 1024 * 1024) { errno = ENOEXEC; return -1; }
    const size_t length = high - low;
    void* reserved = mmap(NULL, length + alignment, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserved == MAP_FAILED) return -1;
    const uintptr_t start = (uintptr_t)reserved, aligned = start + ((low - start) & (alignment - 1));
    const size_t before = aligned - start, after = alignment - before;
    if (before) munmap(reserved, before);
    if (after) munmap((void*)(aligned + length), after);
    unsigned char* base = (unsigned char*)(aligned - low);
    const char* stage = "map DSO data";
    for (size_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* p = ph + i;
        if (p->p_type != PT_LOAD || !p->p_memsz) continue;
        const uint64_t begin = p->p_vaddr & -(uint64_t)page;
        const uint64_t end = (p->p_vaddr + p->p_memsz + page - 1) & -(uint64_t)page;
        const int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) | (p->p_flags & PF_X ? PROT_EXEC : 0);
        void* target = base + begin;
        if (p->p_flags & PF_X) {
            const uint64_t offset = p->p_offset & -(uint64_t)page;
            if (HishMapELFCode(target, end - begin, prot, fd, offset,
                p->p_offset - offset + p->p_filesz, NULL, &stage) == MAP_FAILED) goto fail;
        } else {
            stage = "map DSO data";
            if (mmap(target, end - begin, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
                HishMuslDSORead(fd, base + p->p_vaddr, p->p_filesz, p->p_offset) ||
                mprotect(target, end - begin, prot)) goto fail;
        }
    }
    stage = "validate DSO dynamic table/libc";
    if (HishMuslDSODynamic(base, ph, eh->e_phnum)) goto fail;
    *mapping = (unsigned char*)aligned; *mapSize = length; *loadBase = base;
    if (getenv("HISH_MUSL_DSO_DIAGNOSTICS")) dprintf(2, "HISH_MUSL_DSO_MAPPED address=%p size=%zu\n", (void*)aligned, length);
    return 0;
fail:;
    const int saved = errno;
    if (getenv("HISH_MUSL_DSO_DIAGNOSTICS")) dprintf(2, "HiSH DSO: %s errno=%d\n", stage, saved);
    munmap((void*)aligned, length); errno = saved; return -1;
}
#endif
