// Audit R7 probe: reproducer for trip-hazard #157 (second-userspace-iteration hang).
//
// Two consecutive userspace exec invocations. If the first passes and the
// second hangs, we've reproduced the issue.

#include "test.h"

#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_userspace_first_iteration(void);
void test_userspace_second_iteration(void);

// Hand-encoded AArch64 program: SYS_EXITS(0).
//   0xd2800000 movz x0, #0          ; status 0
//   0xd2800008 movz x8, #0          ; SYS_EXITS
//   0xd4000001 svc  #0
//   0x14000000 b    .               ; defensive
static const u32 g_program[] = {
    0xd2800000,
    0xd2800008,
    0xd4000001,
    0x14000000,
};

#define ELF_BLOB_SIZE 8192
static _Alignas(struct Elf64_Ehdr) u8 g_blob[ELF_BLOB_SIZE];
#define SEG_VADDR 0x10000ull

static size_t build_blob(void) {
    for (size_t i = 0; i < ELF_BLOB_SIZE; i++) g_blob[i] = 0;

    struct Elf64_Ehdr *eh = (struct Elf64_Ehdr *)g_blob;
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
    eh->e_entry     = SEG_VADDR;
    eh->e_phoff     = sizeof(struct Elf64_Ehdr);
    eh->e_ehsize    = sizeof(struct Elf64_Ehdr);
    eh->e_phentsize = sizeof(struct Elf64_Phdr);
    eh->e_phnum     = 1;

    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)(g_blob + eh->e_phoff);
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = (u64)PAGE_SIZE;
    ph[0].p_vaddr  = SEG_VADDR;
    ph[0].p_paddr  = SEG_VADDR;
    ph[0].p_filesz = sizeof(g_program);
    ph[0].p_memsz  = (u64)PAGE_SIZE;
    ph[0].p_align  = (u64)PAGE_SIZE;

    u32 *code = (u32 *)(g_blob + PAGE_SIZE);
    for (size_t i = 0; i < sizeof(g_program) / sizeof(u32); i++) {
        code[i] = g_program[i];
    }
    return (size_t)PAGE_SIZE * 2;
}

struct exec_args {
    const void *blob;
    size_t      size;
    int         iteration;
};

__attribute__((noreturn))
static void exec_thunk(void *arg) {
    struct exec_args *ea = (struct exec_args *)arg;
    struct Thread *t = current_thread();
    struct Proc *p = t->proc;

    uart_puts("    [iteration ");
    uart_putdec((u64)ea->iteration);
    uart_puts("] exec_thunk: pid=");
    uart_putdec((u64)p->pid);
    uart_puts(" asid=");
    uart_putdec((u64)p->asid);
    uart_puts(" pgtable_root=");
    uart_puthex64((u64)p->pgtable_root);
    uart_puts("\n");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) exits("fail-exec");

    uart_puts("    [iteration ");
    uart_putdec((u64)ea->iteration);
    uart_puts("] about to userland_enter entry=");
    uart_puthex64(entry);
    uart_puts(" sp=");
    uart_puthex64(sp);
    uart_puts("\n");

    // Read TTBR0_EL1 and L0[0] for forensic diagnosis.
    u64 ttbr0_live;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0_live));
    uart_puts("    [iteration ");
    uart_putdec((u64)ea->iteration);
    uart_puts("] TTBR0_EL1 live=");
    uart_puthex64(ttbr0_live);
    uart_puts("\n");

    // Read L0[0] via the direct map (pgtable_root PA → KVA).
    extern void *pa_to_kva(u64);
    u64 *l0 = (u64 *)pa_to_kva(p->pgtable_root);
    uart_puts("    [iteration ");
    uart_putdec((u64)ea->iteration);
    uart_puts("] L0[0]=");
    uart_puthex64(l0[0]);
    uart_puts("\n");

    uart_puts("    [iteration ");
    uart_putdec((u64)ea->iteration);
    uart_puts("] about to call userland_enter\n");

    userland_enter(entry, sp);
}

static void run_one_iteration(int iteration) {
    size_t size = build_blob();
    struct exec_args args = {
        .blob = g_blob,
        .size = size,
        .iteration = iteration,
    };

    int pid = rfork(RFPROC, exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "exit status 0");

    uart_puts("    [iteration ");
    uart_putdec((u64)iteration);
    uart_puts("] reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts("\n");
}

void test_userspace_first_iteration(void) {
    run_one_iteration(1);
}

void test_userspace_second_iteration(void) {
    run_one_iteration(2);
}
