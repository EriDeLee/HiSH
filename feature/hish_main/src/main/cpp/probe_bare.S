/* Owned, freestanding AArch64 Linux-ABI test. No interpreter or libc. */
.text
.global _start
.type _start, %function
_start:
    ldr x3, [sp]
    cmp x3, #1
    b.ne .Lbad_stack
    mov x0, #1
    adr x1, .Lmessage
    mov x2, #(.Lend-.Lmessage)
    mov x8, #64
    svc #0
    mov x0, #42
    b .Lexit
.Lbad_stack:
    mov x0, #43
.Lexit:
    mov x8, #93
    svc #0
    brk #0
.size _start, .-_start
.global HiSHAuditConst42
.type HiSHAuditConst42, %function
HiSHAuditConst42:
    mov w0, #42
    ret
.size HiSHAuditConst42, .-HiSHAuditConst42
.Lmessage:
    .ascii "BARE_LINUX_ABI_STACK_OK\n"
.Lend:
.section .note.GNU-stack,"",%progbits
