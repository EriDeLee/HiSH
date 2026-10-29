#pragma once
#include <string.h>

// Historical app-specific routes are retained only for explicit diagnostic
// builds. The daily shell must not own Git/curl/compiler package versions.
#define HISH_LEGACY_APPLICATION_TOOLS(X) \
    X("curl", "libhish_linux_curl.so", Musl) \
    X("tcc", "libhish_linux_tcc.so", Musl) \
    X("git", "libhish_linux_git.so", Musl) \
    X("git-upload-pack", "libhish_linux_git.so", Musl) \
    X("git-receive-pack", "libhish_linux_git.so", Musl) \
    X("git-upload-archive", "libhish_linux_git.so", Musl) \
    X("git-index-pack", "libhish_linux_git.so", Musl) \
    X("git-unpack-objects", "libhish_linux_git.so", Musl) \
    X("git-pack-objects", "libhish_linux_git.so", Musl) \
    X("git-remote-http", "libhish_linux_git_remote_http.so", Musl) \
    X("git-remote-https", "libhish_linux_git_remote_http.so", Musl) \
    X("git-http-fetch", "libhish_linux_git_http_fetch.so", Musl) \
    X("git-http-push", "libhish_linux_git_http_push.so", Musl) \
    X("gnu-printf", "libhish_linux_gnu_printf.so", Glibc) \
    X("gnu-uname", "libhish_linux_gnu_uname.so", Glibc)

#define HISH_NATIVE_TEST_TOOLS(X) \
    X("linux-musl-check", "libhish_linux_musl_dynamic.so", Musl) \
    X("linux-musl-static-check", "libhish_linux_musl_static.so", Static) \
    X("linux-glibc-check", "libhish_linux_glibc_dynamic.so", Glibc) \
    X("linux-glibc-static-check", "libhish_linux_glibc_static_compat.so", Static) \
    X("linux-tty-check", "libhish_linux_tty_check.so", Musl) \
    X("glibc-tty-check", "libhish_linux_glibc_tty_check.so", Glibc) \
    X("hish-bench", "libhish_linux_hishbench.so", Musl) \
    X("hish-bench-static", "libhish_linux_hishbench_static.so", Static) \
    X("hish-bench-ohos", "libhish_bench_ohos.so", Ohos) \
    X("ohos-check", "libhish_probe_dynamic_runtime.so", Ohos)
#ifdef HISH_ENABLE_DIAGNOSTICS
#define HISH_NATIVE_TOOLS(X) HISH_LEGACY_APPLICATION_TOOLS(X) HISH_NATIVE_TEST_TOOLS(X)
#else
#define HISH_NATIVE_TOOLS(X)
#endif

// Match only an old app-generated forwarding script, not a command name in
// general. Future independently installed programs/scripts keep normal exec.
static inline int HishRetiredApplicationWrapper(const char* name, const char* bytes, size_t size)
{
    static const char* names[] = {"curl", "tcc", "git", "git-upload-pack", "git-receive-pack",
        "git-upload-archive", "git-index-pack", "git-unpack-objects", "git-pack-objects",
        "git-remote-http", "git-remote-https", "git-http-fetch", "git-http-push", "gnu-printf", "gnu-uname"};
    int retired = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(name, names[i])) { retired = 1; break; }
    if (!retired) return 0;
    const char head[] = "#!/bin/sh\nexec ", tail[] = " \"$@\"\n";
    const size_t length = strlen(name), start = sizeof(head) - 1;
    return size == start + length + sizeof(tail) - 1 && !memcmp(bytes, head, start) &&
        !memcmp(bytes + start, name, length) && !memcmp(bytes + start + length, tail, sizeof(tail) - 1);
}

// Old app-created wrappers remain in PREFIX on an upgrade. Refuse their
// command names rather than recursively executing the same wrapper forever.
static inline int HishDisabledTool(const char* name)
{
    if (!strcmp(name,"busybox") || !strcmp(name,"ash") || !strcmp(name,"dash") ||
        !strcmp(name,"mrsh") || !strcmp(name,"mksh-trial")) return 1;
#ifndef HISH_ENABLE_DIAGNOSTICS
    if (!strcmp(name, "hish-run")) return 1;
#define DISABLED_TOOL(tool,file,runtime) if (!strcmp(name,tool)) return 1;
    HISH_NATIVE_TEST_TOOLS(DISABLED_TOOL)
#undef DISABLED_TOOL
#endif
    return 0;
}
