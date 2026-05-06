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
//   - ET_EXEC at v1.0 (PIE/ET_DYN deferred to dynamic-linker support).
//
// At v1.0 P2-Ga:
//   - Static binaries only. PT_INTERP rejected.
//   - PT_DYNAMIC + PT_GNU_RELRO + relocations deferred to PIE / dynamic
//     linking support.
//   - Symbol table parsing not performed.
//
// Phase 3+ refinement:
//   - VMO-backed segment mapping via vmo_map.
//   - Demand paging from the segment's VMO.
//   - exec() syscall surface in Phase 5+.
//
// Phase 5+ refinement:
//   - PIE / ET_DYN support with dynamic relocator.
//   - musl-style dynamic linker integration.
//   - Symbol resolution for ld.so / loader-callbacks.

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
#define ELFOSABI_NONE 0   // System V; what we accept

// e_type — object file type.
#define ET_NONE       0
#define ET_REL        1
#define ET_EXEC       2   // executable file (the v1.0 acceptable type)
#define ET_DYN        3   // shared / PIE (deferred)
#define ET_CORE       4

// e_machine — architecture.
#define EM_AARCH64    183

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
    u64                       entry;        // start address (e_entry)
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
    ELF_LOAD_BAD_TYPE       =  -8,    // e_type != ET_EXEC at v1.0
    ELF_LOAD_BAD_MACHINE    =  -9,    // e_machine != EM_AARCH64
    ELF_LOAD_BAD_FILE_VER   = -10,    // e_version != EV_CURRENT
    ELF_LOAD_BAD_PHENTSIZE  = -11,    // e_phentsize != sizeof(Elf64_Phdr)
    ELF_LOAD_NO_PHDRS       = -12,    // e_phnum == 0
    ELF_LOAD_TOO_MANY_PHDRS = -13,    // e_phnum > some sane bound
    ELF_LOAD_PHTAB_OOB      = -14,    // phoff + phnum*phentsize > size
    ELF_LOAD_HAS_INTERP     = -15,    // PT_INTERP present (dynamic; v1.0 static only)
    ELF_LOAD_RWX_REJECTED   = -16,    // PT_LOAD with PF_W | PF_X — I-12 violation
    ELF_LOAD_BAD_FILESZ     = -17,    // filesz > memsz
    ELF_LOAD_SEG_OOB        = -18,    // file_offset + filesz > size
    ELF_LOAD_TOO_MANY_LOADS = -19,    // > ELF_MAX_LOAD_SEGMENTS PT_LOAD entries
    ELF_LOAD_BAD_ENTRY      = -20,    // e_entry == 0 OR not in any LOAD segment
    ELF_LOAD_EXEC_STACK     = -21,    // PT_GNU_STACK with PF_X — NX-stack violation
};

// elf_load — parse + validate an ELF64 ARM64 blob.
//
// Inputs:
//   blob: pointer to the raw ELF bytes (size bytes long).
//   size: length of blob in bytes.
//   out:  receives the parsed image on success. Undefined on failure.
//
// Returns ELF_LOAD_OK on success; a negative error code on failure.
// On failure, *out is left in an undefined state (may be partially
// populated up to the point of failure; caller should ignore).
//
// On success:
//   out->entry         = e_entry
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

#endif // THYLACINE_ELF_H
