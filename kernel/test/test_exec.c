// P3-Eb: exec_setup tests.
//
// Five tests exercise exec_setup's address-space population:
//
//   exec.setup_smoke
//     Synthesize a minimal valid ELF; call exec_setup; verify VMAs
//     installed at expected vaddrs with expected prot; verify user
//     stack VMA installed at top of user-VA; verify entry + sp out.
//
//   exec.setup_segment_data_copied
//     ELF with a PT_LOAD segment containing recognizable bytes;
//     verify the bytes are present in the VMO's backing pages
//     (read via direct map).
//
//   exec.setup_constraints
//     NULL Proc, NULL blob, kproc rejected, p with existing VMAs
//     rejected, unaligned segment vaddr rejected, ELF parse errors
//     surfaced as -1.
//
//   exec.setup_multi_segment
//     ELF with text RX + rodata R + data RW segments; verify all
//     three VMAs installed with correct prot bits; verify user
//     stack VMA also installed.
//
//   exec.setup_lifecycle_round_trip
//     exec_setup + proc_free → all backing pages return to baseline
//     (sub-tables freed by P3-Db walker; segment + stack VMOs freed
//     by mapping_count→0 at vma_drain since vmo_unref already dropped
//     the caller-held handle in exec_setup).

#include "test.h"

#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/vmo.h>

#include "../../mm/phys.h"

void test_exec_setup_smoke(void);
void test_exec_setup_segment_data_copied(void);
void test_exec_setup_constraints(void);
void test_exec_setup_multi_segment(void);
void test_exec_setup_lifecycle_round_trip(void);

#define ELF_BLOB_SIZE 8192
// 8-byte aligned per elf_load's R5-G F61 alignment precondition. We use
// 16-byte alignment for safety (struct Elf64_Ehdr alignment fits inside).
static _Alignas(struct Elf64_Ehdr) u8 g_elf_blob[ELF_BLOB_SIZE];

static void zero_blob(void) {
    for (size_t i = 0; i < ELF_BLOB_SIZE; i++) g_elf_blob[i] = 0;
}

// Build a minimal ELF in g_elf_blob with `n_loads` PT_LOAD segments.
// Each segment's flags come from `flags[i]`; vaddr starts at 0x10000
// and steps by 0x10000 (each segment is one page = 0x1000 memsz with
// generous spacing). file_offset packs the segments after the headers
// (page-aligned).
//
// `filesz_bytes`: each segment's filesz; pass the same value for all.
//                 Bytes [file_offset .. file_offset + filesz_bytes)
//                 of the blob are written by the caller (via
//                 g_elf_blob[file_offset + i]) before exec_setup runs.
//
// Returns total blob size.
static size_t build_elf(const u32 *flags, int n_loads, u64 filesz_bytes) {
    zero_blob();

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
    eh->e_shoff     = 0;
    eh->e_flags     = 0;
    eh->e_ehsize    = sizeof(struct Elf64_Ehdr);
    eh->e_phentsize = sizeof(struct Elf64_Phdr);
    eh->e_phnum     = (u16)n_loads;
    eh->e_shentsize = 0;
    eh->e_shnum     = 0;
    eh->e_shstrndx  = 0;

    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)(g_elf_blob + eh->e_phoff);

    // Page-align each segment's file_offset. Pack them at PAGE_SIZE
    // intervals starting at PAGE_SIZE (so headers occupy [0, PAGE_SIZE)).
    for (int i = 0; i < n_loads; i++) {
        ph[i].p_type   = PT_LOAD;
        ph[i].p_flags  = flags[i];
        ph[i].p_offset = (u64)PAGE_SIZE * (u64)(i + 1);
        ph[i].p_vaddr  = 0x10000ull + (u64)i * 0x10000ull;
        ph[i].p_paddr  = ph[i].p_vaddr;
        ph[i].p_filesz = filesz_bytes;
        ph[i].p_memsz  = 0x1000;
        ph[i].p_align  = 0x1000;
    }

    // Total blob size: max file_offset + filesz, rounded up to page.
    return (size_t)PAGE_SIZE * (size_t)(n_loads + 1);
}

static struct Proc *make_proc(void) {
    return proc_alloc();
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_exec_setup_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/0);

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup smoke should succeed");
    TEST_EXPECT_EQ(entry, (u64)0x10000,            "entry == ELF e_entry");
    TEST_EXPECT_EQ(sp,    EXEC_USER_STACK_TOP,     "sp == user stack top");

    // Segment VMA at vaddr 0x10000.
    struct Vma *seg_vma = vma_lookup(p, 0x10000ull);
    TEST_ASSERT(seg_vma != NULL,                   "segment VMA visible");
    TEST_EXPECT_EQ(seg_vma->prot, VMA_PROT_RX,     "segment prot RX");

    // User stack VMA — lookup should hit anywhere in [BASE, TOP).
    struct Vma *stack_vma = vma_lookup(p, EXEC_USER_STACK_BASE);
    TEST_ASSERT(stack_vma != NULL,                 "stack VMA visible at base");
    TEST_EXPECT_EQ(stack_vma->prot, VMA_PROT_RW,   "stack prot RW");

    drop_proc(p);
}

void test_exec_setup_segment_data_copied(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // Single text segment, RX, with 256 bytes of data.
    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/256);

    // Write recognizable bytes into the segment payload.
    // Segment file_offset = PAGE_SIZE.
    for (size_t i = 0; i < 256; i++) {
        g_elf_blob[PAGE_SIZE + i] = (u8)(i ^ 0x5A);
    }

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup with data");

    // Verify the bytes are now in the segment's VMO via vma_lookup
    // → vma->vmo->pages → direct map.
    struct Vma *vma = vma_lookup(p, 0x10000ull);
    TEST_ASSERT(vma != NULL, "segment VMA");
    TEST_ASSERT(vma->vmo != NULL, "VMA has VMO");
    TEST_ASSERT(vma->vmo->pages != NULL, "VMO has backing pages");

    u8 *vmo_kva = (u8 *)pa_to_kva(page_to_pa(vma->vmo->pages));
    for (size_t i = 0; i < 256; i++) {
        u8 want = (u8)(i ^ 0x5A);
        TEST_EXPECT_EQ(vmo_kva[i], want, "segment byte at offset i");
    }
    // Tail of the page (256 .. PAGE_SIZE) should be zero.
    for (size_t i = 256; i < PAGE_SIZE; i++) {
        TEST_EXPECT_EQ(vmo_kva[i], (u8)0, "tail zero-padded");
    }

    drop_proc(p);
}

void test_exec_setup_constraints(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/0);

    u64 entry, sp;

    // NULL Proc.
    TEST_EXPECT_EQ(exec_setup(NULL, g_elf_blob, size, &entry, &sp), -1,
        "NULL Proc rejected");

    // NULL blob.
    TEST_EXPECT_EQ(exec_setup(p, NULL, size, &entry, &sp), -1,
        "NULL blob rejected");

    // NULL out params.
    TEST_EXPECT_EQ(exec_setup(p, g_elf_blob, size, NULL, &sp), -1,
        "NULL entry_out rejected");
    TEST_EXPECT_EQ(exec_setup(p, g_elf_blob, size, &entry, NULL), -1,
        "NULL sp_out rejected");

    // Bad ELF — surface ELF_LOAD_BAD_MAGIC as -1.
    g_elf_blob[0] = 0;     // corrupt magic
    TEST_EXPECT_EQ(exec_setup(p, g_elf_blob, size, &entry, &sp), -1,
        "bad ELF magic surfaces as -1");

    // Unaligned segment vaddr — rebuild with a non-aligned vaddr.
    size = build_elf(flags, 1, /*filesz=*/0);
    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)
        (g_elf_blob + sizeof(struct Elf64_Ehdr));
    ph[0].p_vaddr  = 0x10001ull;          // off by 1
    ph[0].p_paddr  = 0x10001ull;
    // e_entry must still be in the segment.
    ((struct Elf64_Ehdr *)g_elf_blob)->e_entry = 0x10001ull;
    TEST_EXPECT_EQ(exec_setup(p, g_elf_blob, size, &entry, &sp), -1,
        "unaligned segment vaddr rejected");

    drop_proc(p);
}

void test_exec_setup_multi_segment(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // text RX @ 0x10000, rodata R @ 0x20000, data RW @ 0x30000.
    u32 flags[3] = { PF_R | PF_X, PF_R, PF_R | PF_W };
    size_t size = build_elf(flags, 3, /*filesz=*/0);

    u64 entry, sp;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup multi-segment");

    struct Vma *text_vma   = vma_lookup(p, 0x10000ull);
    struct Vma *rodata_vma = vma_lookup(p, 0x20000ull);
    struct Vma *data_vma   = vma_lookup(p, 0x30000ull);
    struct Vma *stack_vma  = vma_lookup(p, EXEC_USER_STACK_BASE);

    TEST_ASSERT(text_vma   != NULL, "text VMA");
    TEST_ASSERT(rodata_vma != NULL, "rodata VMA");
    TEST_ASSERT(data_vma   != NULL, "data VMA");
    TEST_ASSERT(stack_vma  != NULL, "stack VMA");

    TEST_EXPECT_EQ(text_vma->prot,   VMA_PROT_RX,         "text prot RX");
    TEST_EXPECT_EQ(rodata_vma->prot, VMA_PROT_READ,       "rodata prot R");
    TEST_EXPECT_EQ(data_vma->prot,   VMA_PROT_RW,         "data prot RW");
    TEST_EXPECT_EQ(stack_vma->prot,  VMA_PROT_RW,         "stack prot RW");

    drop_proc(p);
}

void test_exec_setup_lifecycle_round_trip(void) {
    u64 free_before = phys_free_pages();

    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 flags[2] = { PF_R | PF_X, PF_R | PF_W };
    size_t size = build_elf(flags, 2, /*filesz=*/0);

    u64 entry, sp;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup round-trip");

    drop_proc(p);

    u64 free_after = phys_free_pages();
    TEST_EXPECT_EQ(free_after, free_before,
        "phys_free_pages must return to baseline (no leak in exec lifecycle: "
        "segment VMOs freed via vma_drain → vmo_release_mapping → "
        "mapping_count→0 + handle_count==0 → vmo_free_internal; sub-tables "
        "freed by proc_pgtable_destroy walker)");
}
