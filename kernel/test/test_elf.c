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
void test_elf_pie_load_bias(void);

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

    // Bad OSABI -- an unrecognized value is still rejected.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_OSABI] = 99;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_OSABI, "unrecognized OSABI rejected");

    // CL-4: ELFOSABI_GNU (3) IS accepted, alongside NONE. lld stamps it on any
    // output carrying a GNU feature -- for the CL-4 clang++ merely
    // SHF_GNU_RETAIN on .bss, which has no runtime meaning. Without this the
    // whole device toolchain fails to exec, so pin acceptance: a future
    // re-tightening must fail HERE rather than silently in a boot.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_OSABI] = ELFOSABI_GNU;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "ELFOSABI_GNU accepted");

    // Bad e_type (REL not EXEC).
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_REL;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_TYPE, "ET_REL rejected");

    // D-2: ET_DYN is now ACCEPTED (it was ELF_LOAD_BAD_TYPE until then).
    // ET_CORE is the remaining third type and stays refused, so this pair
    // shows the gate narrowed rather than opened.
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_DYN;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "ET_DYN accepted (D-2)");

    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_CORE;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_TYPE, "ET_CORE still rejected");

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

// DISTRO D-2: ET_DYN placement.
//
// The design's claim is that a PIE differs from an executable in exactly one
// way -- position -- so the whole change is one bias applied at one place.
// That claim is only worth anything if BOTH halves are checked: the PIE moves,
// and the ET_EXEC does NOT. The ET_EXEC leg here is the control; without it a
// bias accidentally applied to every image would still pass the PIE leg.
void test_elf_pie_load_bias(void) {
    const u32 flags[3] = { PF_R | PF_X, PF_R, PF_R | PF_W };
    size_t size;
    struct elf_image img;

    // build_elf lays segments at 0x10000 + i * 0x10000 with entry 0x10000.
    // As ET_DYN every one of those is an OFFSET from the load base.
    size = build_elf(flags, 3);
    blob_ehdr()->e_type = ET_DYN;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "PIE parses");
    TEST_EXPECT_EQ(img.load_bias, ELF_PIE_LOAD_BIAS, "load_bias recorded");
    TEST_EXPECT_EQ((u64)img.type, (u64)ET_DYN, "e_type recorded");
    TEST_EXPECT_EQ(img.entry, ELF_PIE_LOAD_BIAS + 0x10000ull,
        "entry biased");
    TEST_EXPECT_EQ(img.segments[0].vaddr, ELF_PIE_LOAD_BIAS + 0x10000ull,
        "segment 0 biased");
    TEST_EXPECT_EQ(img.segments[1].vaddr, ELF_PIE_LOAD_BIAS + 0x20000ull,
        "segment 1 biased");
    TEST_EXPECT_EQ(img.segments[2].vaddr, ELF_PIE_LOAD_BIAS + 0x30000ull,
        "segment 2 biased");
    // The bias is POSITIONAL only: file offsets, sizes and permission bits
    // are untouched, which is why every downstream gate keeps working.
    TEST_EXPECT_EQ(img.segments[0].file_offset, 0ull, "file_offset unbiased");
    TEST_EXPECT_EQ(img.segments[2].flags, (u32)(PF_R | PF_W), "flags intact");

    // THE CONTROL: the identical blob as ET_EXEC is unbiased.
    size = build_elf(flags, 3);
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "ET_EXEC parses");
    TEST_EXPECT_EQ(img.load_bias, 0ull, "ET_EXEC load_bias is 0");
    TEST_EXPECT_EQ(img.entry, 0x10000ull, "ET_EXEC entry unbiased");
    TEST_EXPECT_EQ(img.segments[0].vaddr, 0x10000ull, "ET_EXEC vaddr unbiased");

    // PT_DYNAMIC: legal on a PIE (every one carries it), still refused on an
    // ET_EXEC. Both directions, because accepting it everywhere would pass
    // the first assertion alone.
    size = build_elf(flags, 3);
    blob_ehdr()->e_type = ET_DYN;
    blob_phdrs()[2].p_type  = PT_DYNAMIC;
    blob_phdrs()[2].p_flags = PF_R | PF_W;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "PT_DYNAMIC accepted on ET_DYN");

    size = build_elf(flags, 3);
    blob_phdrs()[2].p_type  = PT_DYNAMIC;
    blob_phdrs()[2].p_flags = PF_R | PF_W;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_HAS_DYNAMIC, "PT_DYNAMIC still rejected on ET_EXEC");

    // PT_INTERP stays rejected on a PIE too -- D-2 does not run interpreters.
    // (Stock ld-musl-aarch64.so.1 has no PT_INTERP, which is why the D-2 gate
    // binary loads through this unchanged.)
    size = build_elf(flags, 3);
    blob_ehdr()->e_type = ET_DYN;
    blob_phdrs()[1].p_type = PT_INTERP;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_HAS_INTERP, "PT_INTERP rejected on ET_DYN too");

    // e_entry == 0 means two different things by type. On an ET_EXEC it is a
    // null entry (refused, unchanged). On a PIE it means "entry at the load
    // base", a real address once biased -- so it must be ACCEPTED, and the
    // zero-check has to run on the biased value to tell them apart.
    size = build_elf(flags, 3);
    blob_ehdr()->e_type  = ET_DYN;
    blob_ehdr()->e_entry = 0;
    blob_phdrs()[0].p_vaddr = 0;         // make segment 0 cover offset 0
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "PIE with e_entry 0 loads (entry at the load base)");
    TEST_EXPECT_EQ(img.entry, ELF_PIE_LOAD_BIAS, "its entry IS the load base");

    size = build_elf(flags, 3);
    blob_ehdr()->e_entry = 0;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_BAD_ENTRY, "ET_EXEC with e_entry 0 still refused");

    // The window bound. A p_vaddr that would carry the biased segment past
    // ELF_PIE_LOAD_LIMIT is refused BY THE LOADER, not left to the mapper.
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_DYN;
    blob_phdrs()[0].p_vaddr = ELF_PIE_LOAD_LIMIT;   // biased: way past the top
    blob_ehdr()->e_entry    = ELF_PIE_LOAD_LIMIT;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_PIE_OOB, "PIE segment past the window refused");

    // ...and the same vaddr is fine as an ET_EXEC, since the window is a
    // property of the PIE placement and not a new rule for fixed images.
    size = build_elf(flags, 1);
    blob_phdrs()[0].p_vaddr = ELF_PIE_LOAD_LIMIT;
    blob_ehdr()->e_entry    = ELF_PIE_LOAD_LIMIT;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_OK, "the window binds PIEs only");

    // A p_vaddr near the top of the VA space must not wrap into a small
    // (apparently in-window) address once biased.
    size = build_elf(flags, 1);
    blob_ehdr()->e_type = ET_DYN;
    blob_phdrs()[0].p_vaddr = (u64)-1 - 0x800ull;
    blob_ehdr()->e_entry    = blob_phdrs()[0].p_vaddr;
    TEST_EXPECT_EQ(elf_load(g_test_elf_blob, size, &img),
        ELF_LOAD_PIE_OOB, "a biased vaddr that would wrap is refused");
}

// VIVARIUM V-1: the ADVISORY brand hint (docs/VIVARIUM.md §12.1).
//
// The load-bearing property under test is a NEGATIVE one: the hint must
// never brand a binary Linux on evidence that cannot bear it. Case (2) is
// the Q3 regression proper -- it fails if anyone "improves" elf_brand_hint
// by consulting EI_OSABI, which would mis-brand Clade's own native output.
void test_elf_brand_hint(void) {
    const u32 flags[1] = { PF_R | PF_X };
    size_t size;

    // (1) A static ELF hints UNKNOWN -- and this is ALSO the shape of the
    // v1.0 Linux target (musl-static has no PT_INTERP). The hint deliberately
    // cannot tell native from static-Linux; the vivarium declares instead.
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, size), ELF_BRAND_UNKNOWN,
        "a static ELF hints UNKNOWN (native and musl-static Linux alike)");

    // (2) THE Q3 REGRESSION: EI_OSABI == 3 (ELFOSABI_GNU == ELFOSABI_LINUX)
    // must NOT brand. Clade's native output carries it.
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_OSABI] = 3;
    TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, size), ELF_BRAND_UNKNOWN,
        "EI_OSABI == 3 does NOT brand Linux (Clade native output carries it)");

    // (3) PT_INTERP naming a Linux loader -- the ONE trusted positive signal.
    const char *loaders[2] = { "/lib/ld-musl-aarch64.so.1",
                               "/lib64/ld-linux-aarch64.so.1" };
    for (int k = 0; k < 2; k++) {
        size = build_elf(flags, 1);
        size_t n = 0;
        while (loaders[k][n] != '\0') n++;
        size_t at = size;                       // just past the headers
        for (size_t i = 0; i <= n; i++)
            g_test_elf_blob[at + i] = (u8)loaders[k][i];
        struct Elf64_Phdr *ph = blob_phdrs();
        ph[0].p_type   = PT_INTERP;
        ph[0].p_offset = at;
        ph[0].p_filesz = n + 1;
        TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, at + n + 1),
            ELF_BRAND_LINUX_LIKELY, "PT_INTERP naming a Linux loader hints LINUX");

        // (4) The SAME blob, but the interp string is not inside the buffer we
        // were handed (REVENANT hands a bounded prefix): UNKNOWN, never a
        // read past the end.
        TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, at),
            ELF_BRAND_UNKNOWN, "interp outside the handed prefix hints UNKNOWN");
    }

    // (5) An UNTERMINATED interp extent is untrusted (never scanned past).
    size = build_elf(flags, 1);
    {
        size_t at = size;
        for (size_t i = 0; i < 8; i++) g_test_elf_blob[at + i] = (u8)'A';
        struct Elf64_Phdr *ph = blob_phdrs();
        ph[0].p_type   = PT_INTERP;
        ph[0].p_offset = at;
        ph[0].p_filesz = 8;                      // no NUL within the extent
        TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, at + 8),
            ELF_BRAND_UNKNOWN, "an unterminated interp extent hints UNKNOWN");
    }

    // (6) A non-Linux interpreter earns no verdict.
    size = build_elf(flags, 1);
    {
        const char *other = "/bin/some-other-loader";
        size_t n = 0;
        while (other[n] != '\0') n++;
        size_t at = size;
        for (size_t i = 0; i <= n; i++) g_test_elf_blob[at + i] = (u8)other[i];
        struct Elf64_Phdr *ph = blob_phdrs();
        ph[0].p_type   = PT_INTERP;
        ph[0].p_offset = at;
        ph[0].p_filesz = n + 1;
        TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, at + n + 1),
            ELF_BRAND_UNKNOWN, "a non-Linux interpreter hints UNKNOWN");
    }

    // (7) Garbage / NULL / truncated inputs never produce a verdict.
    TEST_EXPECT_EQ(elf_brand_hint(NULL, 4096), ELF_BRAND_UNKNOWN,
        "NULL blob hints UNKNOWN");
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, 4), ELF_BRAND_UNKNOWN,
        "a sub-header size hints UNKNOWN");
    size = build_elf(flags, 1);
    blob_ehdr()->e_ident[EI_MAG0] = 0x00;        // not an ELF at all
    TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, size), ELF_BRAND_UNKNOWN,
        "a non-ELF blob hints UNKNOWN");
}

// elf.read_interp -- DISTRO D-4. The brand hint above is now a CALLER of this
// walk, so these cases cover the shared bounds logic from the side that ACTS on
// its answer: D-4 resolves the returned path and execs it, which makes every
// "absent" case a refusal-to-run rather than a missing diagnostic. The bar is
// therefore stricter than the hint's -- a partial or truncated path must be
// reported as ABSENT and never as a shorter path, because a shorter path names
// a DIFFERENT file that the kernel would then load.
// Plant `s` as this blob's PT_INTERP just past the headers. `extent` overrides
// p_filesz when non-zero (the unterminated case). Returns the size a caller
// must hand elf_read_interp for the whole string to be present.
static size_t plant_interp(const char *s, size_t extent) {
    const u32 flags[1] = { PF_R | PF_X };
    size_t at = build_elf(flags, 1);
    size_t n = 0;
    while (s[n] != '\0') n++;
    for (size_t i = 0; i <= n; i++) g_test_elf_blob[at + i] = (u8)s[i];
    struct Elf64_Phdr *ph = blob_phdrs();
    ph[0].p_type   = PT_INTERP;
    ph[0].p_offset = at;
    ph[0].p_filesz = extent ? (u64)extent : (u64)(n + 1);
    return at + n + 1;
}

void test_elf_read_interp(void) {
    const u32 flags[1] = { PF_R | PF_X };
    char out[ELF_INTERP_MAX + 1];
    size_t size;

    // (1) The real thing, byte for byte.
    {
        static const char ld[] = "/lib/ld-musl-aarch64.so.1";
        size_t whole = plant_interp(ld, 0);
        size_t got = elf_read_interp(g_test_elf_blob, whole, out, sizeof(out));
        TEST_EXPECT_EQ((u64)got, (u64)(sizeof(ld) - 1),
            "read_interp returns the interpreter path's length");
        int same = 1;
        for (size_t i = 0; i < sizeof(ld); i++) if (out[i] != ld[i]) same = 0;
        TEST_ASSERT(same, "read_interp copies the path NUL-terminated + verbatim");

        // The hint still agrees -- it is a caller of this now, so a refactor
        // that broke one silently would have to break both.
        TEST_EXPECT_EQ(elf_brand_hint(g_test_elf_blob, whole),
            ELF_BRAND_LINUX_LIKELY, "the brand hint reads through read_interp");

        // (2) A bounded PREFIX that stops before the string: ABSENT. `out` must
        // be left EMPTY, not holding the previous call's answer -- a caller
        // that checked only the return value and reused the buffer would
        // otherwise resolve a stale path.
        TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, whole - 1,
                                            out, sizeof(out)), 0ull,
            "an interp not wholly inside the handed prefix is ABSENT");
        TEST_EXPECT_EQ((u64)out[0], 0ull, "the absent case clears the buffer");
    }

    // (3) Unterminated within its own extent: ABSENT, never scanned past.
    {
        static const char part[] = "/lib/ld-musl";
        size_t whole = plant_interp(part, sizeof(part) - 1);  // extent drops the NUL
        TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, whole,
                                            out, sizeof(out)), 0ull,
            "an unterminated interp extent is ABSENT");
    }

    // (4) An EMPTY but terminated interp names nothing -- it must not resolve
    // as the empty path (which a namespace walk would answer for the root).
    {
        static const char empty[] = "";
        size_t whole = plant_interp(empty, 0);
        TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, whole,
                                            out, sizeof(out)), 0ull,
            "an empty interp string is ABSENT, not the empty path");
    }

    // (5) Longer than the bound: ABSENT, and specifically NOT truncated. This
    // is the case that would be a live defect rather than a missed feature --
    // a truncated "/lib/ld-musl-aarch64.so.1" is "/lib/ld" or "/lib", both of
    // which could exist and neither of which is the interpreter.
    {
        char longp[ELF_INTERP_MAX + 8];
        longp[0] = '/';
        for (size_t i = 1; i < sizeof(longp) - 1; i++) longp[i] = 'a';
        longp[sizeof(longp) - 1] = '\0';
        size_t whole = plant_interp(longp, 0);
        TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, whole,
                                            out, sizeof(out)), 0ull,
            "an over-long interp is ABSENT, never truncated");
        TEST_EXPECT_EQ((u64)out[0], 0ull, "and leaves no partial path behind");
    }

    // (6) A caller's buffer smaller than the path is the same answer: absent,
    // empty. The bound that rejects is whichever is tighter.
    {
        static const char ld[] = "/lib/ld-musl-aarch64.so.1";
        size_t whole = plant_interp(ld, 0);
        char small[8];
        TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, whole,
                                            small, sizeof(small)), 0ull,
            "a too-small caller buffer is ABSENT, never a partial copy");
        TEST_EXPECT_EQ((u64)small[0], 0ull, "and is left empty");
    }

    // (7) No PT_INTERP at all -- the static binary, the v1.0 native shape.
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, size,
                                        out, sizeof(out)), 0ull,
        "a static ELF has no interpreter");

    // (8) Degenerate inputs never produce a path.
    TEST_EXPECT_EQ((u64)elf_read_interp(NULL, 4096, out, sizeof(out)), 0ull,
        "NULL blob is ABSENT");
    size = build_elf(flags, 1);
    TEST_EXPECT_EQ((u64)elf_read_interp(g_test_elf_blob, size, out, 0), 0ull,
        "a zero-capacity buffer is ABSENT");
}
