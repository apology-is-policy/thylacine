// ELF64 ARM64 loader tests (P2-Ga).
//
// Six tests covering parse-success + per-rejection-class verification.
// Each test constructs a synthetic ELF blob in-place (no on-disk
// dependency), passes it to elf_load, and verifies the result code +
// (for success cases) the parsed image structure.
//
// Tests:
//   elf.parse_minimal_ok        — single PT_LOAD R+X parses.
//   elf.parse_multi_segment_ok  — text RX + rodata R + data RW parses.
//   elf.header_rejection        — magic / class / data / version /
//                                 osabi / type / machine / file-version
//                                 / phentsize errors each produce the
//                                 expected error code.
//   elf.rwx_rejected            — PF_W | PF_X (with or without PF_R)
//                                 returns ELF_LOAD_RWX_REJECTED. ARCH
//                                 §28 I-12 enforcement at the ELF
//                                 loader layer.
//   elf.bounds_rejection        — too small / NULL inputs / phtab OOB /
//                                 segment OOB / filesz > memsz / too
//                                 many phdrs.
//   elf.policy_rejection        — PT_INTERP rejected (static binaries
//                                 only at v1.0); PT_GNU_STACK with
//                                 PF_X rejected (NX-stack policy);
//                                 e_entry == 0 rejected; entry outside
//                                 any LOAD segment rejected.
//
// Each test is self-contained: rebuilds a known-good blob via
// `build_elf` then mutates one field for the negative cases.

#include "test.h"

#include <thylacine/elf.h>
#include <thylacine/types.h>

void test_elf_parse_minimal_ok(void);
void test_elf_parse_multi_segment_ok(void);
void test_elf_header_rejection(void);
void test_elf_rwx_rejected(void);
void test_elf_bounds_rejection(void);
void test_elf_policy_rejection(void);

#define TEST_ELF_BLOB_SIZE 4096
// R5-G F71 close: explicit alignment so the cast `(struct Elf64_Ehdr *)
// g_test_elf_blob` is well-defined regardless of compiler heuristics
// for BSS array placement. Matches the new alignment precondition in
// elf_load (R5-G F61).
static _Alignas(struct Elf64_Ehdr) u8 g_test_elf_blob[TEST_ELF_BLOB_SIZE];

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static void zero_blob(void) {
    for (size_t i = 0; i < TEST_ELF_BLOB_SIZE; i++) g_test_elf_blob[i] = 0;
}

// Build a minimal valid ELF in g_test_elf_blob with `n_loads` PT_LOAD
// segments. Each segment's flags come from `flags[i]`; vaddr is 0x10000
// + i * 0x10000. p_filesz = 0, p_memsz = 0x1000 (so file_offset bounds
// check trivially passes). Entry point is 0x10000 (first segment).
//
// Returns total blob size.
static size_t build_elf(const u32 *flags, int n_loads) {
    zero_blob();

    struct Elf64_Ehdr *eh = (struct Elf64_Ehdr *)g_test_elf_blob;

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

    struct Elf64_Phdr *ph = (struct Elf64_Phdr *)(g_test_elf_blob + eh->e_phoff);

    for (int i = 0; i < n_loads; i++) {
        ph[i].p_type   = PT_LOAD;
        ph[i].p_flags  = flags[i];
        ph[i].p_offset = 0;
        ph[i].p_vaddr  = 0x10000ull + (u64)i * 0x10000ull;
        ph[i].p_paddr  = ph[i].p_vaddr;
        ph[i].p_filesz = 0;
        ph[i].p_memsz  = 0x1000;
        ph[i].p_align  = 0x1000;
    }

    return sizeof(struct Elf64_Ehdr) + (size_t)n_loads * sizeof(struct Elf64_Phdr);
}

static struct Elf64_Ehdr *blob_ehdr(void) {
    return (struct Elf64_Ehdr *)g_test_elf_blob;
}

static struct Elf64_Phdr *blob_phdrs(void) {
    return (struct Elf64_Phdr *)(g_test_elf_blob + blob_ehdr()->e_phoff);
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_elf_parse_minimal_ok(void) {
    u32 flags[1] = { PF_R | PF_X };
    size_t size = build_elf(flags, 1);

    struct elf_image img;
    int r = elf_load(g_test_elf_blob, size, &img);
    TEST_EXPECT_EQ(r, ELF_LOAD_OK, "minimal valid ELF must parse");
    TEST_EXPECT_EQ(img.n_segments, 1, "1 segment expected");
    TEST_EXPECT_EQ(img.entry, (u64)0x10000, "entry == segment vaddr");
    TEST_EXPECT_EQ(img.segments[0].vaddr, (u64)0x10000, "segment vaddr");
    TEST_EXPECT_EQ(img.segments[0].flags, (u32)(PF_R | PF_X),
        "RX flags preserved");
    TEST_EXPECT_EQ(img.segments[0].memsz, (u64)0x1000, "memsz preserved");
}

void test_elf_parse_multi_segment_ok(void) {
    // Layout: text RX @ 0x10000; rodata R @ 0x20000; data RW @ 0x30000.
    u32 flags[3] = { PF_R | PF_X, PF_R, PF_R | PF_W };
    size_t size = build_elf(flags, 3);

    struct elf_image img;
    int r = elf_load(g_test_elf_blob, size, &img);
    TEST_EXPECT_EQ(r, ELF_LOAD_OK, "3-segment ELF must parse");
    TEST_EXPECT_EQ(img.n_segments, 3, "3 segments expected");
    TEST_EXPECT_EQ(img.segments[0].flags, (u32)(PF_R | PF_X), "text RX");
    TEST_EXPECT_EQ(img.segments[1].flags, (u32)PF_R,           "rodata R");
    TEST_EXPECT_EQ(img.segments[2].flags, (u32)(PF_R | PF_W),  "data RW");
}

void test_elf_header_rejection(void) {
    u32 flags[1] = { PF_R | PF_X };
    size_t size;
    struct elf_image img;

    // Bad magic.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_MAG0] = 0;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_MAGIC, "bad magic rejected");

    // Bad class.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_CLASS] = ELFCLASS32;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_CLASS, "ELFCLASS32 rejected");

    // Bad data (big-endian).
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_DATA] = ELFDATA2MSB;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_DATA, "ELFDATA2MSB rejected");

    // Bad ident version.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_VERSION] = EV_NONE;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_VERSION, "ident EV_NONE rejected");

    // Bad OSABI.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_OSABI] = 99;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_OSABI, "non-zero OSABI rejected");

    // Bad e_type (REL not EXEC).
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_REL;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_TYPE, "ET_REL rejected");

    // ET_DYN (PIE) rejected at v1.0.
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_DYN;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_TYPE, "ET_DYN rejected at v1.0 (PIE deferred)");

    // Bad machine.
    size = build_elf(flags, 1);
    blob_ehdr()->e_machine = 62;    // EM_X86_64
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_MACHINE, "non-AArch64 rejected");

    // Bad file version.
    size = build_elf(flags, 1);
    blob_ehdr()->e_version = EV_NONE;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_FILE_VER, "file EV_NONE rejected");

    // Bad phentsize.
    size = build_elf(flags, 1);
    blob_ehdr()->e_phentsize = 32;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_PHENTSIZE, "wrong phentsize rejected");
}

void test_elf_rwx_rejected(void) {
    struct elf_image img;
    size_t size;
    u32 flags[1];

    // PF_R | PF_W | PF_X — full RWX. ARCH §28 I-12 violation.
    flags[0] = PF_R | PF_W | PF_X;
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_RWX_REJECTED,
        "RWX (R+W+X) segment must be rejected (ARCH §28 I-12)");

    // PF_W | PF_X without PF_R is also rejected.
    flags[0] = PF_W | PF_X;
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_RWX_REJECTED,
        "WX without R must also be rejected");

    // Sanity: RX (no W) is fine.
    flags[0] = PF_R | PF_X;
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "RX is fine");

    // Sanity: RW (no X) is fine.
    flags[0] = PF_R | PF_W;
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "RW is fine (entry happens to land in RW segment; "
                     "Phase 3+ exec checks executability)");

    // OS-specific bits set alongside RW must NOT cause RWX rejection.
    flags[0] = PF_R | PF_W | PF_MASKOS;
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "OS-specific bits don't trigger W^X check");
}

void test_elf_bounds_rejection(void) {
    u32 flags[1] = { PF_R | PF_X };
    size_t size;
    struct elf_image img;

    // size < sizeof(Ehdr).
    build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, 32, &img),
        ELF_LOAD_TOO_SMALL, "size < Ehdr rejected");

    // NULL blob.
    TEST_EXPECT_EQ(elf_load(NULL, 100, &img),
        ELF_LOAD_NULL_INPUT, "NULL blob rejected");

    // NULL out.
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, NULL),
        ELF_LOAD_NULL_INPUT, "NULL out rejected");

    // No phdrs (phnum = 0).
    size = build_elf(flags, 1);
    blob_ehdr()->e_phnum = 0;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_NO_PHDRS, "phnum=0 rejected");

    // Too many phdrs (> ELF_MAX_PHNUM = 256).
    size = build_elf(flags, 1);
    blob_ehdr()->e_phnum = 257;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_TOO_MANY_PHDRS, "phnum > 256 rejected");

    // phtab beyond size: claim 2 phdrs but only 1 fits.
    size = build_elf(flags, 1);
    blob_ehdr()->e_phnum = 2;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_PHTAB_OOB, "phtab beyond size rejected");

    // filesz > memsz (impossible for a valid binary).
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_filesz = 0x2000;
    blob_phdrs()[0].p_memsz  = 0x1000;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_FILESZ, "filesz > memsz rejected");

    // Segment data extends past size.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_offset = size - 4;
    blob_phdrs()[0].p_filesz = 100;
    blob_phdrs()[0].p_memsz  = 100;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_SEG_OOB, "file_offset + filesz > size rejected");

    // R5-G F61 close: misaligned blob pointer rejected. Pass a +1
    // offset into a non-zero portion of the blob — guaranteed to be
    // 1-byte-aligned-but-not-8-byte-aligned. The cast inside elf_load
    // would be UB; the new precondition catches it.
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob + 1, size - 1, &img),
        ELF_LOAD_BAD_ALIGN, "1-byte-aligned blob rejected (R5-G F61)");
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob + 4, size - 4, &img),
        ELF_LOAD_BAD_ALIGN, "4-byte-aligned blob rejected (R5-G F61)");

    // R5-G F62 close: misaligned e_phoff rejected. After the bound
    // check passes (phoff + phtab_bytes <= size), the cast to
    // struct Elf64_Phdr * requires e_phoff % 8 == 0. An attacker-
    // crafted phoff = 65 (odd) would misalign the Phdr table.
    size = build_elf(flags, 1);
    blob_ehdr()->e_phoff = 65;     // odd; bounds-fits but misaligned
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, TEST_ELF_BLOB_SIZE, &img),
        ELF_LOAD_PHTAB_OOB, "misaligned e_phoff rejected (R5-G F62)");

    // R5-G F68 close: too many PT_LOAD segments rejected. Build a
    // binary with > ELF_MAX_LOAD_SEGMENTS PT_LOAD entries; verify
    // ELF_LOAD_TOO_MANY_LOADS.
    {
        u32 many[ELF_MAX_LOAD_SEGMENTS + 1];
        for (int i = 0; i < ELF_MAX_LOAD_SEGMENTS + 1; i++) {
            many[i] = PF_R | PF_X;
        }
        size = build_elf(many, ELF_MAX_LOAD_SEGMENTS + 1);
        TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
            ELF_LOAD_TOO_MANY_LOADS,
            "> ELF_MAX_LOAD_SEGMENTS PT_LOAD entries rejected (R5-G F68)");
    }
}

void test_elf_policy_rejection(void) {
    u32 flags[1] = { PF_R | PF_X };
    size_t size;
    struct elf_image img;

    // PT_INTERP rejected (static binaries only at v1.0).
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_type = PT_INTERP;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_HAS_INTERP, "PT_INTERP rejected");

    // R5-G F63 close: PT_DYNAMIC rejected (static-only policy).
    // PT_DYNAMIC is the dynamic-link table — a binary carrying it but
    // no PT_INTERP would silently pass the old impl; the new impl
    // rejects it explicitly.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_type  = PT_DYNAMIC;
    blob_phdrs()[0].p_flags = PF_R | PF_W;     // sane flags; reject is by type
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_HAS_DYNAMIC, "PT_DYNAMIC rejected (R5-G F63)");

    // PT_GNU_STACK with PF_X rejected (NX-stack policy). Use PF_R|PF_X
    // (no PF_W) so the W^X hoisted check (R5-G F64) doesn't fire first
    // — this test is specifically about the exec-stack policy, which
    // catches PF_X regardless of PF_W.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_type  = PT_GNU_STACK;
    blob_phdrs()[0].p_flags = PF_R | PF_X;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_EXEC_STACK, "PT_GNU_STACK with PF_X rejected (NX-stack)");

    // PT_GNU_STACK with full RWX is caught by W^X first (the hoisted
    // check fires before the GNU_STACK case). Verify ordering.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_type  = PT_GNU_STACK;
    blob_phdrs()[0].p_flags = PF_R | PF_W | PF_X;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_RWX_REJECTED,
        "PT_GNU_STACK with full RWX caught by W^X (hoisted check fires first)");

    // PT_GNU_STACK with R+W (NX) is fine — but still need a PT_LOAD,
    // so this case has phnum=1 with only GNU_STACK → no LOAD segments.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_type  = PT_GNU_STACK;
    blob_phdrs()[0].p_flags = PF_R | PF_W;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_NO_PHDRS,
        "GNU_STACK without PT_LOAD → no loadable segments rejected");

    // e_entry == 0 rejected.
    size = build_elf(flags, 1);
    blob_ehdr()->e_entry = 0;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_ENTRY, "entry == 0 rejected");

    // e_entry outside any LOAD segment.
    size = build_elf(flags, 1);
    blob_ehdr()->e_entry = 0x99999999;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_ENTRY, "entry outside any LOAD segment rejected");

    // R5-G F69 close: entry boundary tests. Default segment is
    // [0x10000, 0x11000) (memsz = 0x1000). Boundaries: vaddr exact,
    // vaddr+memsz-1 (last byte), vaddr+memsz (one past — reject).
    size = build_elf(flags, 1);
    blob_ehdr()->e_entry = 0x10000;            // first byte of segment
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "entry == vaddr accepted (R5-G F69 boundary)");

    size = build_elf(flags, 1);
    blob_ehdr()->e_entry = 0x10FFF;            // last byte of segment
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "entry == vaddr+memsz-1 accepted (R5-G F69 boundary)");

    size = build_elf(flags, 1);
    blob_ehdr()->e_entry = 0x11000;            // one past end
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_ENTRY,
        "entry == vaddr+memsz (one past end) rejected (R5-G F69 boundary)");

    // Final sanity: original valid blob still parses.
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "valid blob still parses after the rejection matrix");
}
