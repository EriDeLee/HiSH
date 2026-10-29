#include <string.h>
// Test-only: force expansion so the compiler evaluates literal input cases.
#define inline inline __attribute__((always_inline))
#include "native_tools.h"
#undef inline

#define TOOL(name, file, runtime) name,
static const char *applications[] = { HISH_NATIVE_TOOLS(TOOL) NULL };
#undef TOOL

// Compiled with test-only inlining/unrolling to LLVM IR: all cases must fold
// to return 0. This exercises
// the actual shared C helper without executing a foreign-ABI program on Windows.
int base_tool_contract(void)
{
    if (applications[0]) return 1;
    const char git[] = "#!/bin/sh\nexec git \"$@\"\n";
    const char tcc[] = "#!/bin/sh\nexec tcc \"$@\"\n";
    const char custom[] = "#!/bin/sh\nprintf user-script\n";
    if (!HishRetiredApplicationWrapper("git", git, sizeof(git)-1)) return 2;
    if (!HishRetiredApplicationWrapper("tcc", tcc, sizeof(tcc)-1)) return 3;
    if (HishRetiredApplicationWrapper("git", custom, sizeof(custom)-1)) return 4;
    if (HishRetiredApplicationWrapper("new-package", git, sizeof(git)-1)) return 5;
    if (HishRetiredApplicationWrapper("git", git, sizeof(git)-2)) return 6;
    if (HishRetiredApplicationWrapper("git", "\177ELF", 4)) return 7;
    if (HishDisabledTool("git") || HishDisabledTool("curl") || HishDisabledTool("tcc")) return 8;
    return 0;
}
