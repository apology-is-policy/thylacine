// ELF64 ARM64 loader (P2-Ga).
//
// Per ARCHITECTURE.md §6.4 (W^X enforcement layers) + §11.2 (exec
// syscall) + §28 invariant I-12 (W^X). At v1.0 P2-Ga the loader is a
// PARSING + VALIDATION library: it inspects an ELF64 blob in memory,
// rejects malformed or W^X-violating inputs, and produces a structured
// segment list. The actual MAPPING of segments into a process address
// space is deferred to Phase 3 (when address spaces + page-fault
// handler land); at v1.0 the loader's output is consumed by tests.
//
// W^X policy (per §6.4 + §28 I-12):
//   - PT_LOAD segments with both PF_W and PF_X flags set are rejected.
//   - .text segments are RX (PF_R | PF_X).
//   - .rodata is R (PF_R).
//   - .data is RW (PF_R | PF_W).
//   - .bss is RW (PF_R | PF_W; filesz=0, memsz>0).
//
// Format constraints (per System V gABI + ARM64 ELF supplement):
//   - ELFCLASS64 (64-bit).
//   - ELFDATA2LSB (little-endian; ARM64 native).
//   - EV_CURRENT version.
//   - EM_AARCH64 machine.
//   - ET_EXEC, or ET_DYN placed at ELF_PIE_LOAD_BIAS (DISTRO D-2).
//
// As built:
//   - PT_INTERP rejected. The DISTRO D-4 rewrite-to-ldso route reads it
//     at the vivarium exec chokepoint, not here.
//   - PT_DYNAMIC accepted for ET_DYN (a PIE inherently carries one) and
//     rejected for ET_EXEC. The loader never PROCESSES it: a static-PIE
//     self-relocates from its own entry, and stock ldso self-relocates in
//     _dlstart before it reads anything. Thylacine has no relocator and
//     needs none on either route.
//   - Symbol table parsing not performed.
//
// Later refinement:
//   - Per-exec PIE base randomization (an I-16-adjacent seam; today the
//     bias is one constant, see ELF_PIE_LOAD_BIAS).
//   - Symbol resolution for loader-callbacks.

#ifndef THYLACINE_ELF_H
#define THYLACINE_ELF_H

#include <thylacine/types.h>

// ============================================================================
// ELF64 on-disk structures (per System V gABI + ARM64 ELF supplement).
// ============================================================================

// e_ident[] indices.
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define EI_PAD        9
#define EI_NIDENT     16

// e_ident[EI_MAG*] expected bytes.
#define ELFMAG0       0x7f
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'

// e_ident[EI_CLASS] — class (architecture word size).
#define ELFCLASSNONE  0
#define ELFCLASS32    1
#define ELFCLASS64    2

// e_ident[EI_DATA] — data encoding.
#define ELFDATANONE   0
#define ELFDATA2LSB   1   // little-endian (ARM64 native)
#define ELFDATA2MSB   2   // big-endian

// e_ident[EI_VERSION] — version.
#define EV_NONE       0
#define EV_CURRENT    1

// e_ident[EI_OSABI] — OS / ABI.
#define ELFOSABI_NONE 0   // System V
#define ELFOSABI_GNU  3   // GNU/Linux (== ELFOSABI_LINUX). lld stamps this on
                          // any output carrying a GNU feature -- for the CL-4
                          // clang++ it is SHF_GNU_RETAIN (0x200000) on .bss,
                          // a purely LINK-time "do not GC this section" flag
                          // with no runtime meaning at all (that binary was
                          // checked: ET_EXEC, no PT_DYNAMIC, zero RELA
                          // sections, zero R_AARCH64_IRELATIVE, zero
                          // STT_GNU_IFUNC).
                          //
                          // Accepted alongside NONE because EI_OSABI is
                          // advisory: it does not change how a static ET_EXEC
                          // is mapped, none of this loader's real gates
                          // (class/data/version/type/machine, segment bounds,
                          // W^X) read it, and Linux accepts both. A GNU binary
                          // that genuinely needs IFUNC has its IRELATIVE relocs
                          // applied by its own static crt in userspace; if that
                          // never happens the call faults in EL0 and the Proc
                          // takes a snare:segv -- self-inflicted, never a
                          // kernel concern.

// e_type — object file type.
#define ET_NONE       0
#define ET_REL        1
#define ET_EXEC       2   // executable file, absolute p_vaddr
#define ET_DYN        3   // shared / PIE, p_vaddr relative to the load base
#define ET_CORE       4

// DISTRO D-2 -- where an ET_DYN image lands.
//
// A PIE's p_vaddr values are offsets from a load base the loader chooses.
// elf_load adds this bias to e_entry and to every PT_LOAD's vaddr, so
// everything downstream (the segment mapper, the file-backed/eager arm
// split, the AT_PHDR translation) reads FINAL addresses and needed no
// change: the W^X gate, the page-sharing refusal and the alignment terms
// are byte-identical, only the numbers moved.
//
// Chosen against the exec VA plan in <thylacine/exec.h>, with the images
// that actually load measured (2026-08-06):
//   native ET_EXEC (joey)        0x400000 .. 0x47e349
//   pouch  ET_EXEC (pouch-hello) 0x200000 .. 0x208770
//   stock  ET_DYN  (Alpine ldso)      0x0 ..  0xc2f10   (~780 KiB span)
//   stock  ET_DYN  (Alpine busybox)   0x0 ..  0xf0b10   (~963 KiB span)
//
// 512 MiB sits ~100x above the highest ET_EXEC extent, so a PIE and a
// fixed-address executable can never be confused for one another in a
// fault report, and no ET_EXEC we ship can collide with the PIE window.
// The 1.5 GiB limit gives a PIE a 1 GiB span and still leaves ~511 MiB
// clear below the stack guard (exec.h pins that inequality).
//
// 64 KiB-aligned on purpose: stock aarch64 ELFs carry p_align 0x10000, so
// a bias that is a multiple of it preserves the gABI's
// `p_vaddr == p_offset (mod p_align)` congruence exactly -- which is what
// keeps a segment's file-offset alignment (and therefore its eligibility
// for the shared file-backed arm) the same biased as unbiased.
//
// ONE CONSTANT, DELIBERATELY. Per-exec randomization is a recorded
// I-16-adjacent seam (docs/DISTRO.md section 1 non-goals); when it lands,
// this becomes a per-exec value and the natural shape is a parameter on
// elf_load rather than a constant here.
#define ELF_PIE_LOAD_BIAS    0x0000000020000000ull   // 512 MiB
#define ELF_PIE_LOAD_LIMIT   0x0000000060000000ull   // 1.5 GiB (exclusive)
_Static_assert(ET_DYN == 3,        "ELF ABI pin: e_type shared/PIE");
_Static_assert((ELF_PIE_LOAD_BIAS & 0xFFFFull) == 0,
               "PIE bias must be 64 KiB-aligned so a stock aarch64 ELF's "
               "p_vaddr == p_offset (mod p_align) congruence survives it");
_Static_assert(ELF_PIE_LOAD_BIAS < ELF_PIE_LOAD_LIMIT,
               "the PIE window must be non-empty");

// e_machine — architecture.
#define EM_AARCH64    183

// The acceptance constants the loader gates on are ABI-fixed by the ELF
// spec; pin them so a header edit (or a conflicting include) cannot
// silently shift what elf_load accepts.
_Static_assert(ELFCLASS64 == 2,    "ELF ABI pin: e_ident[EI_CLASS] 64-bit");
_Static_assert(ELFDATA2LSB == 1,   "ELF ABI pin: e_ident[EI_DATA] little-endian");
_Static_assert(EV_CURRENT == 1,    "ELF ABI pin: version");
_Static_assert(ELFOSABI_NONE == 0, "ELF ABI pin: System V OSABI");
_Static_assert(ELFOSABI_GNU == 3,  "ELF ABI pin: GNU/Linux OSABI");
_Static_assert(ET_EXEC == 2,       "ELF ABI pin: e_type executable");
_Static_assert(EM_AARCH64 == 183,  "ELF ABI pin: e_machine ARM64");

// p_type — program-header type.
#define PT_NULL       0
#define PT_LOAD       1   // a loadable segment (the only type we map)
#define PT_DYNAMIC    2   // dynamic-linking info (rejected at v1.0)
#define PT_INTERP     3   // interpreter path (rejected at v1.0)
#define PT_NOTE       4   // auxiliary info (skipped)
#define PT_SHLIB      5   // reserved
#define PT_PHDR       6   // program-header table itself (skipped)
#define PT_TLS        7   // thread-local storage (skipped at v1.0)
#define PT_GNU_STACK  0x6474e551    // stack permissions (consumed; verify NX)
#define PT_GNU_RELRO  0x6474e552    // read-only after relocs (skipped)

// p_flags — segment permissions.
#define PF_X          (1u << 0)
#define PF_W          (1u << 1)
#define PF_R          (1u << 2)
#define PF_MASKOS     0x0ff00000u   // OS-specific bits (ignored)
#define PF_MASKPROC   0xf0000000u   // proc-specific bits (ignored)

// Elf64_Ehdr — ELF64 file header. 64 bytes; no compiler padding (every
// field is aligned to its natural boundary).
struct Elf64_Ehdr {
    u8  e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};
_Static_assert(sizeof(struct Elf64_Ehdr) == 64,
               "Elf64_Ehdr size pinned at 64 bytes (System V gABI)");

// Elf64_Phdr — ELF64 program header. 56 bytes.
struct Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};
_Static_assert(sizeof(struct Elf64_Phdr) == 56,
               "Elf64_Phdr size pinned at 56 bytes (System V gABI)");

// ============================================================================
// Auxiliary vector (auxv) — the System V process-startup ABI.
// ============================================================================
//
// The kernel hands a freshly exec'd process an auxiliary vector on its
// initial stack (see kernel/exec.c). A C runtime (pouch's musl-derived
// libc — POUCH-DESIGN.md) reads it to discover the page size, the
// program-header table, and startup entropy. Each entry is a
// (a_type, a_val) pair; the vector ends with an AT_NULL entry.

// a_type values. Only the subset Thylacine populates is defined here
// (POUCH-DESIGN.md §12.1 — the minimum a static musl process needs).
#define AT_NULL       0    // end-of-vector marker (a_val ignored)
#define AT_PHDR       3    // a_val = user VA of the program-header table
#define AT_PHENT      4    // a_val = size of one program-header entry
#define AT_PHNUM      5    // a_val = number of program headers
#define AT_PAGESZ     6    // a_val = system page size in bytes
// a_val = the entry point of the image the kernel loaded (e_entry +
// load_bias). DISTRO D-2. Standard SysV/Linux tag; getauxval(AT_ENTRY)
// answers it, and the v1.x in-kernel dual-image PT_INTERP lift (DISTRO
// section 3.2's recorded alternative) would need it to name the PROGRAM's
// entry while AT_PHDR named the program's phdrs.
//
// It is NOT what makes stock ldso work, despite what the D-2 design text
// said before this was measured (task #186). musl discriminates direct
// invocation on `aux[AT_PHDR] != ldso.phdr` (ldso/dynlink.c:1834) -- the
// AT_PHDR we emit for a directly-exec'd ldso equals its own base+e_phoff,
// so it takes the direct-invocation branch and WRITES this tag itself
// (dynlink.c:1914) before jumping through it (dynlink.c:2075).
#define AT_ENTRY      9
// a_val = the Linux-compatible arm64 hwcap word (g_hw_features.linux_hwcap:
// FP/ASIMD/AES/PMULL/SHA1/SHA2/CRC32/ATOMICS/SHA3/ASIMDDP/SHA512 at the
// Linux uapi bit numbers). The STANDARD SysV/Linux tag — musl's
// getauxval(AT_HWCAP) and the Go runtime's cpu init read it directly, so
// ported feature detection (libsodium's armcrypto gate, Go's crypto
// packages) sees the real CPU. Bits are truthful per ID_AA64ISAR0/PFR0;
// a consumer must fall back to its portable path on a clear bit.
#define AT_HWCAP      16
#define AT_RANDOM     25   // a_val = user VA of 16 bytes of entropy
// Thylacine-private auxv tag (a_val = user VA of the struct vdso_clock page;
// docs/VDSO-DESIGN.md + thylacine/vdso.h). Deliberately OUTSIDE the
// Linux/System V AT_ range so a pouch/musl binary stores-and-ignores it (musl
// queries only the standard tags); it must NEVER be confused with
// AT_SYSINFO_EHDR=33, which musl would parse as a vDSO ELF. Native readers
// (libthyla-rs, the Go fork) query it explicitly; absence -> fall back to
// SYS_CLOCK_GETTIME.
#define AT_VDSO_CLOCK 0x5654   // 'VT' — the clock vDSO page

// Elf64_auxv_t — one auxiliary-vector entry. 16 bytes; the initial
// stack carries an array of these terminated by an AT_NULL entry.
struct Elf64_auxv_t {
    u64 a_type;
    u64 a_val;
};
_Static_assert(sizeof(struct Elf64_auxv_t) == 16,
               "Elf64_auxv_t size pinned at 16 bytes (System V gABI)");

// ============================================================================
// Loader API.
// ============================================================================

// Maximum LOAD segments accepted at v1.0. Real binaries have 2-4
// (text, rodata, data, bss). 16 is generous; programs with > 16
// LOAD segments are pathological at v1.0 (the linker can be told to
// merge segments).
#define ELF_MAX_LOAD_SEGMENTS 16

// Per-segment permissions, derived from PF_R | PF_W | PF_X with the
// W^X invariant enforced (no segment has both PF_W and PF_X).
struct elf_load_segment {
    u64      vaddr;        // virtual address to load at
    u64      file_offset;  // offset in the source ELF blob
    u64      filesz;       // bytes from the file
    u64      memsz;        // bytes in memory (>= filesz; tail is zero)
    u32      flags;        // PF_R / PF_W / PF_X (PF_W & PF_X never both set)
};

struct elf_image {
    // FINAL addresses: for ET_DYN both `entry` and every segments[].vaddr
    // already carry `load_bias`. Consumers never add it themselves.
    u64                       entry;        // e_entry + load_bias
    u64                       load_bias;    // 0 for ET_EXEC; ELF_PIE_LOAD_BIAS for ET_DYN
    u16                       type;         // e_type (ET_EXEC | ET_DYN) — diagnostics
    u64                       phoff;        // e_phoff — file offset of the phdr table
    u16                       phnum;        // e_phnum — count of program headers
    u16                       phentsize;    // e_phentsize (== sizeof(struct Elf64_Phdr))
    int                       n_segments;   // count of valid segments[]
    struct elf_load_segment   segments[ELF_MAX_LOAD_SEGMENTS];
};

// elf_load_result — error codes for elf_load. Negative on failure;
// 0 (ELF_LOAD_OK) on success.
//
// Each error class is a distinct value to make test diagnostics clear.
enum elf_load_result {
    ELF_LOAD_OK             =   0,
    ELF_LOAD_NULL_INPUT     =  -1,    // blob == NULL or out == NULL
    ELF_LOAD_TOO_SMALL      =  -2,    // size < sizeof(Elf64_Ehdr)
    ELF_LOAD_BAD_MAGIC      =  -3,    // e_ident[0..3] != \x7fELF
    ELF_LOAD_BAD_CLASS      =  -4,    // e_ident[EI_CLASS] != ELFCLASS64
    ELF_LOAD_BAD_DATA       =  -5,    // e_ident[EI_DATA] != ELFDATA2LSB
    ELF_LOAD_BAD_VERSION    =  -6,    // e_ident[EI_VERSION] != EV_CURRENT
    ELF_LOAD_BAD_OSABI      =  -7,    // e_ident[EI_OSABI] != ELFOSABI_NONE
    ELF_LOAD_BAD_TYPE       =  -8,    // e_type is neither ET_EXEC nor ET_DYN
    ELF_LOAD_BAD_MACHINE    =  -9,    // e_machine != EM_AARCH64
    ELF_LOAD_BAD_FILE_VER   = -10,    // e_version != EV_CURRENT
    ELF_LOAD_BAD_PHENTSIZE  = -11,    // e_phentsize != sizeof(Elf64_Phdr)
    ELF_LOAD_NO_PHDRS       = -12,    // e_phnum == 0 OR no PT_LOAD found in phdrs
    ELF_LOAD_TOO_MANY_PHDRS = -13,    // e_phnum > some sane bound
    ELF_LOAD_PHTAB_OOB      = -14,    // phoff + phnum*phentsize > size, OR phoff misaligned
    ELF_LOAD_HAS_INTERP     = -15,    // PT_INTERP present (dynamic; v1.0 static only)
    ELF_LOAD_RWX_REJECTED   = -16,    // PT_LOAD with PF_W | PF_X — I-12 violation
    ELF_LOAD_BAD_FILESZ     = -17,    // filesz > memsz
    ELF_LOAD_SEG_OOB        = -18,    // file_offset + filesz > size
    ELF_LOAD_TOO_MANY_LOADS = -19,    // > ELF_MAX_LOAD_SEGMENTS PT_LOAD entries
    ELF_LOAD_BAD_ENTRY      = -20,    // e_entry == 0 OR not in any LOAD segment
    ELF_LOAD_EXEC_STACK     = -21,    // PT_GNU_STACK with PF_X — NX-stack violation
    ELF_LOAD_BAD_ALIGN      = -22,    // R5-G F61: blob is not 8-byte aligned
    ELF_LOAD_HAS_DYNAMIC    = -23,    // PT_DYNAMIC on an ET_EXEC (D-2: legal on ET_DYN)
    ELF_LOAD_PIE_OOB        = -24,    // D-2: biased ET_DYN segment leaves the PIE window
};

// elf_load — parse + validate an ELF64 ARM64 blob.
//
// Inputs:
//   blob: pointer to the raw ELF bytes (size bytes long). MUST be at
//         least 8-byte aligned (the alignment of struct Elf64_Ehdr).
//         Kernel callers using kmalloc / page-allocator memory satisfy
//         this trivially; callers passing slices into other buffers
//         must ensure alignment themselves. R5-G F61 closure.
//   size: length of blob in bytes.
//   out:  receives the parsed image on success. ZEROED on entry —
//         see R5-G F70 closure — so partial-population on early-exit
//         leaves a defined-zero state rather than attacker-controlled
//         data. Caller should still ignore on non-OK return per
//         convention.
//
// Returns ELF_LOAD_OK on success; a negative error code on failure.
// On failure, *out is left in an undefined state (may be partially
// populated up to the point of failure; caller should ignore).
//
// On success:
//   out->entry         = e_entry
//   out->phoff         = e_phoff (phdr-table file offset)
//   out->phnum         = e_phnum
//   out->phentsize     = e_phentsize
//   out->n_segments    = number of PT_LOAD segments accepted
//   out->segments[0..N-1] = PT_LOAD segment metadata
//
// Validation enforced:
//   - All header fields (magic, class, data, version, OSABI, type,
//     machine, file version, phentsize).
//   - phnum within a sane bound; phoff + phnum*phentsize within size.
//   - No PT_INTERP (rejecting dynamic binaries at v1.0).
//   - PT_GNU_STACK without PF_X (NX-stack invariant).
//   - For each PT_LOAD:
//       - PF_W and PF_X NOT both set (W^X — ARCH §28 I-12).
//       - filesz <= memsz.
//       - file_offset + filesz <= size.
//       - n_loads <= ELF_MAX_LOAD_SEGMENTS.
//   - e_entry within some PT_LOAD segment's vaddr range.
//
// Maps to ARCH §6.4 W^X enforcement (ELF loader layer) + §11.2 exec
// syscall + §28 I-12.
int elf_load(const void *blob, size_t size, struct elf_image *out);

// VIVARIUM V-1 (docs/VIVARIUM.md section 12.1): an ADVISORY brand hint.
//
// This function NEVER decides a phenotype. The Q3 resolution is binding: a Proc
// is never INFERRED into a non-default ABI, only DECLARED into one (by its
// vivarium). The hint exists solely so an obvious mismatch can be reported --
// a Linux-interp binary exec'd OUTSIDE a vivarium earns a diagnostic and a
// clean failure instead of a silent mis-decode.
//
// Why the byte cannot decide (the ground truth behind Q3):
//   - ELFOSABI_NONE (0) is carried by most musl-static LINUX binaries AND by
//     every native Thylacine binary -- it identifies nothing.
//   - ELFOSABI_GNU (3) == ELFOSABI_LINUX (3) means "a GNU/LLVM toolchain
//     emitted GNU extensions"; Clade's own NATIVE output carries it.
// So there is deliberately NO `NATIVE_LIKELY` verdict from any byte we can read
// today: absence of a Linux signal is NOT evidence of nativeness. A positive
// native brand (a `.note.thylacine`, VIVARIUM section 12.2) is the v1.x seam
// that would let this function speak in both directions.
//
// PURE: no allocation, no locks, no side effects; safe on a partial header
// buffer (REVENANT reads a bounded prefix) -- anything unreadable is UNKNOWN.
enum elf_brand {
    ELF_BRAND_UNKNOWN = 0,      // no signal, or unreadable -- the common case
    ELF_BRAND_LINUX_LIKELY = 1, // a positive Linux signal (interp / OSABI)
};

enum elf_brand elf_brand_hint(const void *blob, size_t size);

// DISTRO D-4: the interpreter path, extracted for the rewrite-to-ldso route
// (docs/DISTRO.md section 7.1). Same bounded PT_INTERP walk elf_brand_hint
// runs -- it is now the CALLER of this, so the bounds logic has one home.
//
// PURE, like the hint: no allocation, no locks, safe on the bounded header
// PREFIX exec_read_header produces. Everything unreadable, unterminated, or
// oversized is "absent" (0), never a partial answer -- a truncated interpreter
// path resolved as if whole is exactly the silent-wrong this loader refuses.
//
// Writes a NUL-terminated path to `out` (capacity `out_cap`, which must be at
// least ELF_INTERP_MAX + 1) and returns its LENGTH, or 0 for "no PT_INTERP /
// not usable". Never negative: a caller acts identically on every absent case.
#define ELF_INTERP_MAX 255      // bounds the on-stack copy; every real
                                // interpreter path is under 32 bytes
                                // ("/lib/ld-musl-aarch64.so.1" is 25)

size_t elf_read_interp(const void *blob, size_t size, char *out, size_t out_cap);

#endif // THYLACINE_ELF_H
