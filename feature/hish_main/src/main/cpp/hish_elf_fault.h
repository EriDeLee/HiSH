#pragma once
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

#if defined(__aarch64__)
static inline long HishFaultSyscall(long number, long a, long b, long c)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static void HishFaultText(char* buffer, size_t* size, const char* text)
{
    while (*text && *size < 240) buffer[(*size)++] = *text++;
}
static void HishFaultNumber(char* buffer, size_t* size, uintptr_t value, unsigned base)
{
    char digits[32]; size_t count = 0;
    do { digits[count++] = "0123456789abcdef"[value % base]; value /= base; } while (value && count < sizeof(digits));
    while (count && *size < 240) buffer[(*size)++] = digits[--count];
}
// Diagnostic mode exits on a fault. It does not emulate a denied syscall or
// return from SIGSYS. Raw calls avoid crossing libc/TLS after an ELF transition.
__attribute__((no_stack_protector)) static void HishELFFault(int signal, siginfo_t* info, void* context)
{
    char buffer[256]; size_t size = 0;
    HishFaultText(buffer, &size, "ELF_FAULT signal="); HishFaultNumber(buffer, &size, signal, 10);
    if (signal == SIGSYS) { HishFaultText(buffer, &size, " syscall="); HishFaultNumber(buffer, &size, info->si_syscall, 10); }
    HishFaultText(buffer, &size, " pc=0x"); HishFaultNumber(buffer, &size, ((ucontext_t*)context)->uc_mcontext.pc, 16);
    HishFaultText(buffer, &size, " address=0x"); HishFaultNumber(buffer, &size, (uintptr_t)info->si_addr, 16);
    HishFaultText(buffer, &size, "\n");
    HishFaultSyscall(64, 2, (long)buffer, size);
    HishFaultSyscall(94, 128 + signal, 0, 0);
    for (;;) {}
}
#endif
static void HishInstallELFDiagnostics(void)
{
#if defined(__aarch64__)
    const char* enabled = getenv("HISH_ELF_DIAGNOSTICS");
    if (!enabled || enabled[0] != '1') return;
    struct sigaction action = {0};
    action.sa_sigaction = HishELFFault; action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSYS, &action, NULL); sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGILL, &action, NULL); sigaction(SIGBUS, &action, NULL); sigaction(SIGABRT, &action, NULL);
#endif
}
