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
//     verify the bytes are present in the BURROW's backing pages
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
//     by mapping_count→0 at vma_drain since burrow_unref already dropped
//     the caller-held handle in exec_setup).

#include "test.h"

#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/dev.h>       // #45: the blob-serving stub Dev
#include <thylacine/spoor.h>     // #45: spoor_alloc for the from_spoor path
#include <thylacine/image.h>     // #45: Image-cache counters (dispatch proof)

#include "../../mm/phys.h"
#include "../../arch/arm64/hwfeat.h"   // g_hw_features.linux_hwcap (AT_HWCAP)

void test_exec_setup_smoke(void);
void test_exec_setup_segment_data_copied(void);
void test_exec_setup_constraints(void);
void test_exec_setup_multi_segment(void);
void test_exec_setup_lifecycle_round_trip(void);
void test_exec_user_stack_guard(void);
void test_exec_setup_auxv(void);
void test_exec_setup_auxv_no_phdr_segment(void);
void test_exec_from_spoor_rodata_dispatch(void);

#define ELF_BLOB_SIZE 16384   // 4 pages: headers + 3 one-page segments (#45)
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

// Build a one-segment ELF whose PT_LOAD spans file offset 0 — so it
// COVERS the ELF header + program-header table. This is the real-binary
// shape (the first PT_LOAD always includes the headers); it lets
// exec_build_init_stack resolve a non-zero AT_PHDR. Returns blob size.
static size_t build_elf_phdrs_loaded(void) {
    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/0);
    // Repoint segment 0 to file offset 0 with a filesz that spans the
    // Ehdr (64) + one Phdr (56) = 120 bytes. (build_elf packs segment 0
    // at file_offset PAGE_SIZE, which does NOT cover the phdrs.)
    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)
        (g_elf_blob + sizeof(struct Elf64_Ehdr));
    ph[0].p_offset = 0;
    ph[0].p_filesz = 512;
    return size;
}


// =============================================================================
// #45 / REVENANT 4.6: the from_spoor PT_LOAD dispatch.
//
// A stub Dev serving file bytes straight from g_elf_blob, so
// exec_setup_from_spoor runs against a synthetic "file" with no FS. Proves the
// generalized gate: NON-WRITABLE segments (R+X text AND R-only rodata) route
// file-backed through the Image cache; writable data stays eager anon
// (I-36 condition 4). Fails on the pre-#45 gate by construction (rodata would
// come back BURROW_TYPE_ANON and only ONE Image entry would be created).
// =============================================================================

static size_t g_blob_dev_size;

static long blob_dev_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c;
    if (n <= 0 || off < 0 || (u64)off >= (u64)g_blob_dev_size) return 0;
    size_t avail = g_blob_dev_size - (size_t)off;
    size_t want  = (size_t)n < avail ? (size_t)n : avail;
    u8 *b = (u8 *)buf;
    for (size_t i = 0; i < want; i++) b[i] = g_elf_blob[(size_t)off + i];
    return (long)want;
}

static struct Dev g_blob_dev = {
    .dc   = '?',
    .name = "execblob",
    .read = blob_dev_read,
};


static struct Proc *make_proc(void) {
    return proc_alloc();
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_exec_from_spoor_rodata_dispatch(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // text RX @ 0x10000 (file 0x1000), rodata R @ 0x20000 (file 0x2000),
    // data RW @ 0x30000 (file 0x3000); each filesz == memsz == one page.
    u32 flags[3] = { PF_R | PF_X, PF_R, PF_R | PF_W };
    size_t size = build_elf(flags, 3, /*filesz=*/0x1000);
    // Recognizable bytes in the RW data segment (proves the eager copy still
    // reads through the Dev). The FILE segments are not faulted here -- their
    // content path is demand_page.file_rodata_prot / file_smoke.
    for (size_t i = 0; i < 0x20; i++) g_elf_blob[0x3000 + i] = (u8)(0xE0 + i);
    g_blob_dev_size = size;

    u64 creates0 = image_cache_creates_for_test();

    struct Spoor *exe = spoor_alloc(&g_blob_dev);
    TEST_ASSERT(exe != NULL, "spoor_alloc");
    exe->qid.path = 0x45C0DEull;      // distinct Image key vs any other test
    exe->qid.vers = 7;

    u64 entry = 0, sp = 0;
    int rc = exec_setup_from_spoor(p, exe, size, NULL, 0, 0, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup_from_spoor");
    TEST_EXPECT_EQ(entry, (u64)0x10000, "entry == e_entry");

    struct Vma *text = vma_lookup(p, 0x10000ull);
    struct Vma *ro   = vma_lookup(p, 0x20000ull);
    struct Vma *rw   = vma_lookup(p, 0x30000ull);
    TEST_ASSERT(text != NULL && ro != NULL && rw != NULL, "three VMAs");
    TEST_EXPECT_EQ(text->prot, VMA_PROT_RX,   "text prot RX");
    TEST_EXPECT_EQ(ro->prot,   VMA_PROT_READ, "rodata prot R-only");
    TEST_EXPECT_EQ(rw->prot,   VMA_PROT_RW,   "data prot RW");
    TEST_EXPECT_EQ((int)text->burrow->type, (int)BURROW_TYPE_FILE,
        "text FILE-backed");
    TEST_EXPECT_EQ((int)ro->burrow->type, (int)BURROW_TYPE_FILE,
        "rodata FILE-backed (the #45 gate)");
    TEST_EXPECT_EQ((int)rw->burrow->type, (int)BURROW_TYPE_ANON,
        "data eager ANON (I-36 condition 4: writable never file-backed)");
    TEST_EXPECT_EQ(image_cache_creates_for_test() - creates0, 2,
        "two Image entries created (text + rodata)");

    // The eager RW copy carried the file bytes.
    u8 *rwb = (u8 *)pa_to_kva(page_to_pa(rw->burrow->pages));
    TEST_EXPECT_EQ((u64)rwb[0],    (u64)0xE0, "data byte 0 eager-copied");
    TEST_EXPECT_EQ((u64)rwb[0x1f], (u64)0xFF, "data byte 0x1f eager-copied");

    // Teardown: unmap (drop_proc) -> both Image entries go idle -> evict frees
    // the FILE Burrows (each clunks its adopted spoor ref) -> our own ref last.
    drop_proc(p);
    image_cache_evict_idle_for_test();
    spoor_clunk(exe);
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
    TEST_EXPECT_EQ(sp,    EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE,
        "sp == stack top minus the System V startup frame");

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

    // Verify the bytes are now in the segment's BURROW via vma_lookup
    // → vma->burrow->pages → direct map.
    struct Vma *vma = vma_lookup(p, 0x10000ull);
    TEST_ASSERT(vma != NULL, "segment VMA");
    TEST_ASSERT(vma->burrow != NULL, "VMA has BURROW");
    TEST_ASSERT(vma->burrow->pages != NULL, "BURROW has backing pages");

    u8 *burrow_kva = (u8 *)pa_to_kva(page_to_pa(vma->burrow->pages));
    for (size_t i = 0; i < 256; i++) {
        u8 want = (u8)(i ^ 0x5A);
        TEST_EXPECT_EQ(burrow_kva[i], want, "segment byte at offset i");
    }
    // Tail of the page (256 .. PAGE_SIZE) should be zero.
    for (size_t i = 256; i < PAGE_SIZE; i++) {
        TEST_EXPECT_EQ(burrow_kva[i], (u8)0, "tail zero-padded");
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
        "segment VMOs freed via vma_drain → burrow_release_mapping → "
        "mapping_count→0 + handle_count==0 → burrow_free_internal; sub-tables "
        "freed by proc_pgtable_destroy walker)");
}

// P5-secondary-stack-guard / corvus-bringup-d audit F7: exec_map_user_
// stack installs a one-page guard VMA directly below the user stack —
// a prot==0, no-BURROW reserved range. Verifies the guard is present,
// correctly shaped, distinct from the stack VMA, and reserves the
// address range against a future mapping (vma_insert overlap rejection).
void test_exec_user_stack_guard(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/0);

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup should succeed");

    // The guard VMA sits at [GUARD_BASE, STACK_BASE).
    struct Vma *guard = vma_lookup(p, EXEC_USER_STACK_GUARD_BASE);
    TEST_ASSERT(guard != NULL, "guard VMA visible at EXEC_USER_STACK_GUARD_BASE");
    TEST_EXPECT_EQ(guard->vaddr_start, EXEC_USER_STACK_GUARD_BASE,
        "guard VMA starts at EXEC_USER_STACK_GUARD_BASE");
    TEST_EXPECT_EQ(guard->vaddr_end, EXEC_USER_STACK_BASE,
        "guard VMA ends flush against the user stack base");
    TEST_EXPECT_EQ((u64)guard->prot, (u64)0,
        "guard VMA has prot==0 (userland_demand_page rejects every fault)");
    TEST_ASSERT(guard->burrow == NULL,
        "guard VMA has no backing BURROW");

    // The guard covers the whole page up to the stack base; nothing is
    // mapped one byte below the guard base.
    TEST_ASSERT(vma_lookup(p, EXEC_USER_STACK_BASE - 1) == guard,
        "guard VMA covers up to (but not including) the stack base");
    TEST_ASSERT(vma_lookup(p, EXEC_USER_STACK_GUARD_BASE - 1) == NULL,
        "nothing is mapped immediately below the guard");

    // The guard is a distinct VMA from the stack itself.
    struct Vma *stack = vma_lookup(p, EXEC_USER_STACK_BASE);
    TEST_ASSERT(stack != NULL && stack != guard,
        "the user stack VMA is distinct from, and above, the guard");

    // Reservation: a VMA overlapping the guard is rejected by
    // vma_insert — a future mapping allocator cannot fill the guard.
    struct Burrow *b = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(b != NULL, "burrow_create_anon for the overlap probe");
    struct Vma *intruder = vma_alloc(EXEC_USER_STACK_GUARD_BASE,
                                     EXEC_USER_STACK_GUARD_BASE + PAGE_SIZE,
                                     VMA_PROT_RW, b, 0);
    TEST_ASSERT(intruder != NULL, "vma_alloc for the overlap probe");
    TEST_EXPECT_EQ(vma_insert(p, intruder), -1,
        "a VMA overlapping the guard is rejected — the guard reserves the range");
    vma_free(intruder);          // rejected → never linked → safe to free
    burrow_unref(b);

    drop_proc(p);                // proc_free → vma_drain frees the NULL-burrow guard
}

// P6-pouch-kernel-auxv: exec_setup builds a System V process-startup
// frame (argc / argv / envp / auxv) at the top of the user stack.
// Verifies the exact byte layout against an ELF whose first PT_LOAD
// covers the program headers, so AT_PHDR resolves to a real user VA.
void test_exec_setup_auxv(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    size_t size = build_elf_phdrs_loaded();
    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup (phdrs-loaded ELF) should succeed");

    // sp sits EXEC_INIT_STACK_SIZE below the stack top and is 16-aligned.
    TEST_EXPECT_EQ(sp, EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE,
        "sp == EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE");
    TEST_EXPECT_EQ(sp & 15ull, 0ull, "sp is 16-byte aligned (AArch64 ABI)");

    // Read the frame back from the stack BURROW via the direct map.
    struct Vma *sv = vma_lookup(p, EXEC_USER_STACK_BASE);
    TEST_ASSERT(sv != NULL && sv->burrow != NULL, "stack VMA + BURROW present");
    u8 *stack_kva = (u8 *)pa_to_kva(page_to_pa(sv->burrow->pages));
    u64 *w = (u64 *)(stack_kva + EXEC_USER_STACK_SIZE - EXEC_INIT_STACK_SIZE);

    // argc / argv / envp — all empty at v1.0.
    TEST_EXPECT_EQ(w[0], 0ull, "argc == 0");
    TEST_EXPECT_EQ(w[1], 0ull, "argv[] terminator is NULL");
    TEST_EXPECT_EQ(w[2], 0ull, "envp[] terminator is NULL");

    // auxv — eight (a_type, a_val) pairs: AT_PHDR/PHENT/PHNUM/PAGESZ,
    // AT_HWCAP, AT_RANDOM, AT_VDSO_CLOCK (the vDSO page maps at boot --
    // vdso_init ran), AT_NULL last.
    TEST_EXPECT_EQ(w[3],  (u64)AT_PHDR,   "auxv[0].a_type == AT_PHDR");
    TEST_EXPECT_EQ(w[4],  0x10040ull,     "AT_PHDR == seg0 vaddr + e_phoff");
    TEST_EXPECT_EQ(w[5],  (u64)AT_PHENT,  "auxv[1].a_type == AT_PHENT");
    TEST_EXPECT_EQ(w[6],  (u64)sizeof(struct Elf64_Phdr),
        "AT_PHENT == sizeof(Elf64_Phdr) == 56");
    TEST_EXPECT_EQ(w[7],  (u64)AT_PHNUM,  "auxv[2].a_type == AT_PHNUM");
    TEST_EXPECT_EQ(w[8],  1ull,           "AT_PHNUM == e_phnum");
    TEST_EXPECT_EQ(w[9],  (u64)AT_PAGESZ, "auxv[3].a_type == AT_PAGESZ");
    TEST_EXPECT_EQ(w[10], (u64)PAGE_SIZE, "AT_PAGESZ == PAGE_SIZE");
    TEST_EXPECT_EQ(w[11], (u64)AT_HWCAP,  "auxv[4].a_type == AT_HWCAP");
    TEST_EXPECT_EQ(w[12], g_hw_features.linux_hwcap,
        "AT_HWCAP == g_hw_features.linux_hwcap");
    // FP + AdvSIMD are architecturally present on every ARMv8-A target
    // Thylacine boots on (QEMU-virt TCG/HVF; the Lazarus boards) — a
    // zero word would mean the PFR0 inverted-sentinel decode regressed.
    TEST_ASSERT((w[12] & 0x3ull) == 0x3ull,
        "AT_HWCAP carries FP|ASIMD (the PFR0 decode)");
    TEST_EXPECT_EQ(w[13], (u64)AT_RANDOM, "auxv[5].a_type == AT_RANDOM");
    TEST_EXPECT_EQ(w[15], (u64)AT_VDSO_CLOCK, "auxv[6].a_type == AT_VDSO_CLOCK");
    TEST_EXPECT_EQ(w[16], EXEC_USER_VDSO_BASE, "AT_VDSO_CLOCK == EXEC_USER_VDSO_BASE");
    TEST_EXPECT_EQ(w[17], (u64)AT_NULL,   "auxv[7].a_type == AT_NULL");
    TEST_EXPECT_EQ(w[18], 0ull,           "AT_NULL.a_val == 0");

    // AT_RANDOM points at the 16-byte entropy block, which must lie
    // within the user stack region.
    u64 rand_va = w[14];
    TEST_EXPECT_EQ(rand_va, sp + EXEC_INIT_RANDOM_OFFSET,
        "AT_RANDOM a_val == sp + EXEC_INIT_RANDOM_OFFSET");
    TEST_ASSERT(rand_va >= EXEC_USER_STACK_BASE &&
                rand_va + 16 <= EXEC_USER_STACK_TOP,
        "the AT_RANDOM block lies within the user stack");

    // The 16 entropy bytes are CSPRNG-populated — not all zero (a
    // genuine all-zero 16-byte draw is a 2^-128 event).
    u8 *rand_bytes = stack_kva + EXEC_USER_STACK_SIZE - 16;
    u8 rand_or = 0;
    for (int i = 0; i < 16; i++) rand_or |= rand_bytes[i];
    TEST_ASSERT(rand_or != 0, "AT_RANDOM block is CSPRNG-populated (non-zero)");

    drop_proc(p);
}

// P6-pouch-kernel-auxv: when no loaded segment covers the program-header
// table, exec_build_init_stack reports AT_PHDR == 0 / AT_PHNUM == 0 (a C
// runtime then skips the phdr walk — safe for a no-TLS program). build_elf
// packs segment 0 at file_offset PAGE_SIZE, so the phdrs at file offset 64
// are never within a loaded segment.
void test_exec_setup_auxv_no_phdr_segment(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1, /*filesz=*/0);
    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, g_elf_blob, size, &entry, &sp);
    TEST_EXPECT_EQ(rc, 0, "exec_setup should succeed");

    struct Vma *sv = vma_lookup(p, EXEC_USER_STACK_BASE);
    TEST_ASSERT(sv != NULL && sv->burrow != NULL, "stack VMA + BURROW present");
    u8 *stack_kva = (u8 *)pa_to_kva(page_to_pa(sv->burrow->pages));
    u64 *w = (u64 *)(stack_kva + EXEC_USER_STACK_SIZE - EXEC_INIT_STACK_SIZE);

    TEST_EXPECT_EQ(w[3], (u64)AT_PHDR,  "auxv still carries an AT_PHDR slot");
    TEST_EXPECT_EQ(w[4], 0ull,          "AT_PHDR == 0 (no segment covers the phdrs)");
    TEST_EXPECT_EQ(w[7], (u64)AT_PHNUM, "auxv still carries an AT_PHNUM slot");
    TEST_EXPECT_EQ(w[8], 0ull,          "AT_PHNUM == 0 when AT_PHDR is unresolved");
    // The whole phdr triple is zeroed when no segment covers the table —
    // a coherent "no phdrs" auxv (audit F1).
    TEST_EXPECT_EQ(w[6], 0ull, "AT_PHENT == 0 when AT_PHDR is unresolved");
    // The startup frame is otherwise well-formed. With the vDSO page mapped,
    // AT_VDSO_CLOCK occupies the slot at w[15] and AT_NULL terminates at
    // w[17] (AT_HWCAP shifted the tail by one entry).
    TEST_EXPECT_EQ(w[0],  0ull,          "argc == 0");
    TEST_EXPECT_EQ(w[17], (u64)AT_NULL,  "auxv terminated by AT_NULL");

    drop_proc(p);
}
