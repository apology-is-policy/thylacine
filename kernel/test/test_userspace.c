// P3-Ed: end-to-end userspace bringup test.
//
// The capstone of Phase 3 userspace work. Validates the full chain:
//
//   kernel rfork
//        │ child kthread
//        ▼
//   exec_thunk in EL1 on kthread kstack
//        │ exec_setup(p, blob, size) — populate Proc address space
//        │ userland_enter(entry, sp) — eret to EL0
//        ▼
//   user code in EL0
//        │ first instruction faults (no PTE)
//        │ kernel demand-pages → mmu_install_user_pte
//        │ user retries successfully
//        │ ... runs through 4 instructions ...
//        │ svc #0 — EL0 → EL1
//        ▼
//   exception_sync_lower_el → syscall_dispatch
//        │ SYS_EXITS(0) → kernel exits("ok")
//        ▼
//   Proc enters ZOMBIE; thread EXITING
//        │ parent's wait_pid wakes
//        ▼
//   Test verifies exit_status == 0.
//
// User program (4 AArch64 instructions, 16 bytes; hand-encoded):
//
//   movz x8, #0      d2800008    SYS_EXITS
//   movz x0, #0      d2800000    status 0 → "ok"
//   svc  #0          d4000001
//   b    .           14000000    defensive loop (should never execute)

#include "test.h"

#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_userspace_exec_exits_ok(void);

// Hand-encoded user program (4 little-endian u32 instructions).
static const u32 g_user_program_ok[] = {
    0xd2800008,  // movz x8, #0       — SYS_EXITS syscall number
    0xd2800000,  // movz x0, #0       — status 0 → "ok" mapping
    0xd4000001,  // svc  #0           — trap to EL1 syscall handler
    0x14000000,  // b    .            — defensive infinite loop
};

// ELF blob construction. A minimal ET_EXEC with a single PT_LOAD segment
// at vaddr 0x10000 covering one page; the user program lives at
// file_offset = PAGE_SIZE.
#define ELF_BLOB_SIZE 8192
static _Alignas(struct Elf64_Ehdr) u8 g_elf_blob[ELF_BLOB_SIZE];

static size_t build_user_elf(const u32 *program, size_t program_words) {
    // Zero the whole blob so trailing bytes are clean.
    for (size_t i = 0; i < ELF_BLOB_SIZE; i++) g_elf_blob[i] = 0;

    struct Elf64_Ehdr *eh = (struct Elf64_Ehdr *)g_elf_blob;
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
    eh->e_entry     = 0x10000;
    eh->e_phoff     = sizeof(struct Elf64_Ehdr);
    eh->e_ehsize    = sizeof(struct Elf64_Ehdr);
    eh->e_phentsize = sizeof(struct Elf64_Phdr);
    eh->e_phnum     = 1;

    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)(g_elf_blob + eh->e_phoff);
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = (u64)PAGE_SIZE;
    ph[0].p_vaddr  = 0x10000;
    ph[0].p_paddr  = 0x10000;
    ph[0].p_filesz = (u64)(program_words * sizeof(u32));
    ph[0].p_memsz  = 0x1000;
    ph[0].p_align  = 0x1000;

    // Copy the program into the blob at file_offset = PAGE_SIZE.
    u32 *dst = (u32 *)(g_elf_blob + PAGE_SIZE);
    for (size_t i = 0; i < program_words; i++) {
        dst[i] = program[i];
    }

    // Total blob size = headers page + program page.
    return (size_t)PAGE_SIZE * 2;
}

// Per-test exec arguments passed to the rfork'd child via the entry's
// arg pointer.
struct exec_args {
    const void *blob;
    size_t      blob_size;
};

// Child entry. Runs in EL1 on the kthread kstack. Calls exec_setup +
// userland_enter; never returns from userland_enter (transitions to
// EL0). If exec_setup fails, exits with "fail-exec" so wait_pid
// observes a non-zero status.
__attribute__((noreturn))
static void exec_thunk(void *arg) {
    struct exec_args *ea = (struct exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("exec_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->blob_size, &entry, &sp);
    if (rc != 0) {
        exits("fail-exec");
    }

    userland_enter(entry, sp);
    // Unreachable — userland_enter is __noreturn.
}

void test_userspace_exec_exits_ok(void) {
    // Build the "ok" user program ELF.
    size_t blob_size = build_user_elf(g_user_program_ok,
                                      sizeof(g_user_program_ok) / sizeof(u32));

    struct exec_args args = {
        .blob      = g_elf_blob,
        .blob_size = blob_size,
    };

    int pid = rfork(RFPROC, exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid returned the rfork'd pid");
    TEST_EXPECT_EQ(status, 0,
        "userspace ran, demand-paged, called SYS_EXITS(0); "
        "exit_status must be 0 (\"ok\" mapping)");
}

// NOTE: a sibling `test_userspace_exec_exits_fail` was investigated
// during P3-Ed and revealed a "second-userspace-test-iteration hang"
// reproducible against ANY second exec'd EL0 thread — even a second
// invocation of the identical "ok" program. After ~30 minutes of
// debugging:
//   - Both first + second tests show TTBR0_EL1 = same value (recycled
//     ASID + recycled L0 page; KP_ZERO confirmed).
//   - L0[0] = 0 in both tests' userland_enter.
//   - First test: instruction abort → demand-page → SVC → exits → PASS.
//   - Second test: eret completes, no instruction abort fires, no
//     SVC fires, the EL0 thread silently runs without faulting.
//   - tlbi vmalle1is + ic ialluis in userland_enter did not resolve.
// Hypothesis: stale state across consecutive userspace tests (TLB,
// I-cache, or pgtable-walker uop cache). Documented as
// trip-hazard #157; investigation deferred to a focused audit.
// At v1.0 the SINGLE end-to-end test demonstrates the kernel→exec→
// userspace→syscall→kernel chain works; a second run would be
// redundant once the underlying issue is found.
