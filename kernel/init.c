// /init — first userspace process (P3-F).
//
// At v1.0 P3-F, /init is a tiny embedded AArch64 program: prints
// "hello\n" via SYS_PUTS, exits via SYS_EXITS(0). Validates the full
// kernel→exec→userspace→syscall→kernel chain WITHOUT the test-harness
// scaffolding that the prior `userspace.exec_exits_ok` test required.
//
// Embedded user program (9 instructions; 36 bytes):
//
//   0x10000:  movz x0, #0x40        d2800800   x0[15:0] = 0x40
//   0x10004:  movk x0, #1, lsl #16  f2a00020   x0 = 0x10040 (msg addr)
//   0x10008:  movz x1, #6           d28000c1   x1 = 6 (msg length)
//   0x1000c:  movz x8, #1           d2800028   x8 = SYS_PUTS
//   0x10010:  svc  #0               d4000001   trap → SYS_PUTS dispatch
//   0x10014:  movz x0, #0           d2800000   status 0 ("ok")
//   0x10018:  movz x8, #0           d2800008   x8 = SYS_EXITS
//   0x1001c:  svc  #0               d4000001   trap → SYS_EXITS dispatch
//   0x10020:  b    .                14000000   defensive (unreachable)
//   0x10024..0x1003F:  zeros        (padding)
//   0x10040..0x10045:  "hello\n"    msg
//
// Synthetic ELF wrapper: ET_EXEC, EM_AARCH64, single PT_LOAD segment
// at vaddr 0x10000 covering one page (filesz = 0x46 = 70 bytes; memsz
// = PAGE_SIZE = 4096). Headers occupy file_offset 0..end-of-headers;
// segment data starts at file_offset = PAGE_SIZE.
//
// Why hand-encoded + synthetic ELF rather than a cross-compiled C
// binary: at v1.0 P3-F there's no userspace toolchain integration in
// the build (sysroot is Phase 5; userspace tooling is Phase 3+ Rust);
// the in-tree blob is the smallest viable demonstrator. P3-F's
// successor (Phase 4 ramfs / Phase 5 toolchain) will replace this
// with a real binary build pipeline.
//
// Boot flow:
//   boot_main() … all bring-up …
//     test_run_all()                    in-kernel tests (kproc context)
//     fault_test_run()                  hardening proof (production no-op)
//     init_run()                        ← P3-F
//       build_init_elf()                construct synthetic ELF blob
//       rfork(RFPROC, init_thunk, args) child Proc on kthread kstack
//         init_thunk:
//           exec_setup(p, blob, size)   populate child's address space
//           userland_enter(entry, sp)   eret to EL0 (never returns)
//         child user code runs:
//           SVC SYS_PUTS("hello\n", 6)  prints to UART
//           SVC SYS_EXITS(0)            kernel exits("ok")
//       wait_pid(&status)               block; reap child
//     "Thylacine boot OK"               TOOLING.md §10 ABI
//
// Spec posture: no new TLA+ at P3-F. /init is impl-orchestration over
// already-spec'd primitives (rfork from proc model; exec_setup from
// vmo.tla mapping lifecycle; userland_enter is a single-instruction
// EL transition). Phase 5+ /init-as-server (long-running supervisor)
// will warrant a spec extension covering the supervisor/child
// reaping protocol.

#include <thylacine/init.h>

#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// User program — 9 hand-encoded AArch64 instructions, little-endian u32.
// Verified against `clang --target=aarch64-none-elf -c` disassembly.
static const u32 g_init_program[] = {
    0xd2800800,  // movz x0, #0x40        ; x0[15:0] = 0x40
    0xf2a00020,  // movk x0, #1, lsl #16  ; x0 = 0x10040 (msg addr)
    0xd28000c1,  // movz x1, #6           ; x1 = 6 (msg length)
    0xd2800028,  // movz x8, #1           ; x8 = SYS_PUTS
    0xd4000001,  // svc  #0
    0xd2800000,  // movz x0, #0           ; status 0 → "ok"
    0xd2800008,  // movz x8, #0           ; x8 = SYS_EXITS
    0xd4000001,  // svc  #0               ; never returns
    0x14000000,  // b    .                ; defensive (unreachable)
};

// User program embedded in the same RX page as the message. Code
// lives at byte offset 0..0x23; message at byte offset 0x40..0x45.
static const char g_init_msg[] = "hello\n";

// In-segment offset of the message. Code occupies the first 36 bytes;
// rounded to 0x40 to give the program a clean address to load.
#define INIT_MSG_SEGMENT_OFF   0x40

// Single PT_LOAD segment vaddr (must match g_init_program's adrp/movz
// computation: x0 = 0x10040 = INIT_SEGMENT_VADDR + INIT_MSG_SEGMENT_OFF).
#define INIT_SEGMENT_VADDR     0x10000ull

// ELF blob: header page + segment page = 8 KiB. Sized to host one ELF
// header + one PT_LOAD program header + segment data.
#define INIT_ELF_BLOB_SIZE     8192
static _Alignas(struct Elf64_Ehdr) u8 g_init_elf_blob[INIT_ELF_BLOB_SIZE];

// Build the synthetic ELF in g_init_elf_blob. Returns the populated
// blob size (bytes from offset 0 to one past the last meaningful byte).
static size_t build_init_elf(void) {
    // Zero everything so trailing bytes (including post-message padding)
    // are clean, deterministic, KASLR-stable.
    for (size_t i = 0; i < INIT_ELF_BLOB_SIZE; i++) {
        g_init_elf_blob[i] = 0;
    }

    struct Elf64_Ehdr *eh = (struct Elf64_Ehdr *)g_init_elf_blob;
    eh->e_ident[EI_MAG0]    = ELFMAG0;
    eh->e_ident[EI_MAG1]    = ELFMAG1;
    eh->e_ident[EI_MAG2]    = ELFMAG2;
    eh->e_ident[EI_MAG3]    = ELFMAG3;
    eh->e_ident[EI_CLASS]   = ELFCLASS64;
    eh->e_ident[EI_DATA]    = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_ident[EI_OSABI]   = ELFOSABI_NONE;
    eh->e_type      = ET_EXEC;
    eh->e_machine   = EM_AARCH64;
    eh->e_version   = EV_CURRENT;
    eh->e_entry     = INIT_SEGMENT_VADDR;
    eh->e_phoff     = sizeof(struct Elf64_Ehdr);
    eh->e_ehsize    = sizeof(struct Elf64_Ehdr);
    eh->e_phentsize = sizeof(struct Elf64_Phdr);
    eh->e_phnum     = 1;

    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)(g_init_elf_blob + eh->e_phoff);
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = (u64)PAGE_SIZE;            // segment data at second page
    ph[0].p_vaddr  = INIT_SEGMENT_VADDR;
    ph[0].p_paddr  = INIT_SEGMENT_VADDR;
    // filesz spans code + padding + message: INIT_MSG_SEGMENT_OFF + 6 bytes.
    // sizeof(g_init_msg) is 7 (includes the implicit trailing NUL); we
    // copy 6 to match the SYS_PUTS length arg (movz x1, #6) so the NUL
    // doesn't reach UART.
    ph[0].p_filesz = (u64)(INIT_MSG_SEGMENT_OFF + sizeof(g_init_msg) - 1);
    ph[0].p_memsz  = (u64)PAGE_SIZE;
    ph[0].p_align  = (u64)PAGE_SIZE;

    // Copy the program code at file_offset = PAGE_SIZE (segment vaddr 0x10000).
    u32 *code_dst = (u32 *)(g_init_elf_blob + PAGE_SIZE);
    for (size_t i = 0; i < sizeof(g_init_program) / sizeof(u32); i++) {
        code_dst[i] = g_init_program[i];
    }

    // Copy the message at file_offset = PAGE_SIZE + INIT_MSG_SEGMENT_OFF
    // (segment vaddr 0x10040). The 6 bytes match the SYS_PUTS length arg.
    u8 *msg_dst = g_init_elf_blob + PAGE_SIZE + INIT_MSG_SEGMENT_OFF;
    for (size_t i = 0; i < sizeof(g_init_msg) - 1; i++) {
        msg_dst[i] = (u8)g_init_msg[i];
    }

    // Total blob size = header page + segment page (one full PAGE_SIZE
    // each, even though the meaningful data ends earlier — exec_setup
    // reads only [p_offset, p_offset+p_filesz) from the segment).
    return (size_t)PAGE_SIZE * 2;
}

// Arguments passed via rfork's `arg` to the child entry. Lives on the
// caller (boot CPU) stack for the duration of init_run(); the child
// reads it once before transitioning to EL0, after which the parent
// blocks in wait_pid().
struct init_args {
    const void *blob;
    size_t      blob_size;
};

// Child entry. Runs in EL1 on the rfork'd kthread's kstack, in the
// child Proc's context (current_thread()->proc is the new Proc).
// Calls exec_setup + userland_enter; never returns from userland_enter
// (transitions to EL0). On exec_setup failure, exits("fail-exec") so
// the parent's wait_pid observes a non-zero exit_status.
__attribute__((noreturn))
static void init_thunk(void *arg) {
    struct init_args *ia = (struct init_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("init_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("init_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ia->blob, ia->blob_size, &entry, &sp);
    if (rc != 0) {
        exits("fail-exec");
    }

    userland_enter(entry, sp);
    // userland_enter is __noreturn; control transfers to EL0 atomically.
}

void init_run(void) {
    uart_puts("  init: rforking child for /init (9-instr hello blob)\n");

    size_t blob_size = build_init_elf();

    struct init_args args = {
        .blob      = g_init_elf_blob,
        .blob_size = blob_size,
    };

    int pid = rfork(RFPROC, init_thunk, &args);
    if (pid < 0) {
        extinction("init: rfork(RFPROC, init_thunk) failed");
    }

    int status = -42;
    int reaped = wait_pid(&status);
    if (reaped != pid) {
        extinction_with_addr("init: wait_pid returned wrong pid", (u64)reaped);
    }
    if (status != 0) {
        extinction_with_addr("init: /init exited non-zero", (u64)status);
    }

    uart_puts("  init: /init pid=");
    uart_putdec((u64)pid);
    uart_puts(" exited cleanly (status=0)\n");
}
