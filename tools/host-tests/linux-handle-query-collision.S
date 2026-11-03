/* Build as an unmarked Linux AArch64 ELF, never through pouch-ld.
 * Execute through an MPHENO_LINUX mount; ELF branding does not select ABI.
 * Linux restart_syscall(128) must remain ENOSYS, even with live fd 1 in x0. */
.global _start
.text
_start:
    mov x0, #1
    mov x8, #128
    svc #0
    cmn x0, #38
    b.ne wrong_query
    mov x0, #1
    adr x1, message
    mov x2, #message_end-message
    mov x8, #64
    svc #0
    cmp x0, #message_end-message
    b.ne failed
    mov x0, #0
    b done
failed:
    mov x0, #11
    b done
wrong_query:
    mov x0, #10
done:
    mov x8, #93
    svc #0
    b .
message: .ascii "PASS LINUX restart128-enosys\n"
message_end:
