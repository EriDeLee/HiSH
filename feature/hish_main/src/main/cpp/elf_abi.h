#ifndef HISH_ELF_ABI_H
#define HISH_ELF_ABI_H
#include <string.h>

// One classification for every loader entry. Only the exact standard AArch64
// interpreter names select a libc; anything else is reported as unknown
// instead of being guessed from a substring.
enum HishElfAbi { HishAbiStatic, HishAbiMusl, HishAbiGlibc, HishAbiUnknown };
static inline enum HishElfAbi HishInterpreterAbi(const char* interpreter)
{
    if (!interpreter || !*interpreter) return HishAbiStatic;
    const char* base = strrchr(interpreter, '/');
    base = base ? base + 1 : interpreter;
    if (!strcmp(base, "ld-musl-aarch64.so.1")) return HishAbiMusl;
    if (!strcmp(base, "ld-linux-aarch64.so.1")) return HishAbiGlibc;
    return HishAbiUnknown;
}
#endif
