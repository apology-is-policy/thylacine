# 21 — ELF64 ARM64 loader (P2-G)

The ELF64 ARM64 loader — parses + validates an ELF binary blob in memory, enforcing the W^X invariant at the loader layer per ARCH §6.4 + §28 I-12. v1.0 P2-Ga lands the **parsing + validation** library; the actual MAPPING of segments into a process address space is deferred to Phase 3 (when address spaces + page-fault handler land); the `exec()` syscall surface lands at Phase 5+.

---

## Purpose

Three layers enforce the W^X invariant:

1. **PTE bit layer** (`arch/arm64/mmu.c`): the kernel page-table writer rejects PTEs with both write + execute bits.
2. **`mprotect` syscall layer** (Phase 5+): rejects transitions from R+W to R+X (or R+W+X).
3. **ELF loader layer** (this subsystem): rejects ELF segments with `PF_W | PF_X` flags set together at parse time.

Each layer independently catches a class of violation. The ELF loader is the **earliest** point a malicious or buggy binary could request RWX memory; rejecting at parse means the request never reaches the mapping subsystem.

Other v1.0 P2-Ga policy:

- **ELFCLASS64 + ELFDATA2LSB + EV_CURRENT + ELFOSABI_NONE**: standard ARM64 binaries.
- **ET_EXEC** only. ET_DYN (PIE / shared) deferred to dynamic-linker support post-v1.0.
- **EM_AARCH64** only.
- **No PT_INTERP** (static binaries only at v1.0).
- **NX stack**: PT_GNU_STACK with PF_X is rejected.
- **Sane bounds**: phnum ≤ 256; phtab + segments within blob size; filesz ≤ memsz.

---

## Public API — `<thylacine/elf.h>`

```c
// ELF64 on-disk structures (System V gABI). Sizes pinned via _Static_assert.

struct Elf64_Ehdr {                  // 64 bytes
    u8  e_ident[16];
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
_Static_assert(sizeof(struct Elf64_Ehdr) == 64, ...);

struct Elf64_Phdr {                  // 56 bytes
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};
_Static_assert(sizeof(struct Elf64_Phdr) == 56, ...);

// Loader output — parsed segment list + entry point.

#define ELF_MAX_LOAD_SEGMENTS 16

struct elf_load_segment {
    u64 vaddr;        // virtual address to load at
    u64 file_offset;
    u64 filesz;
    u64 memsz;
    u32 flags;        // PF_R/W/X (W&X never both set)
};

struct elf_image {
    u64                       entry;
    int                       n_segments;
    struct elf_load_segment   segments[ELF_MAX_LOAD_SEGMENTS];
};

int elf_load(const void *blob, size_t size, struct elf_image *out);
```

### `elf_load(blob, size, out)` — return semantics

| Return value | Meaning |
|---|---|
| `ELF_LOAD_OK` (0) | success; `*out` populated. |
| `ELF_LOAD_NULL_INPUT` (-1) | blob == NULL OR out == NULL |
| `ELF_LOAD_TOO_SMALL` (-2) | size < sizeof(Elf64_Ehdr) |
| `ELF_LOAD_BAD_MAGIC` (-3) | e_ident[0..3] != \x7fELF |
| `ELF_LOAD_BAD_CLASS` (-4) | e_ident[EI_CLASS] != ELFCLASS64 |
| `ELF_LOAD_BAD_DATA` (-5) | e_ident[EI_DATA] != ELFDATA2LSB |
| `ELF_LOAD_BAD_VERSION` (-6) | e_ident[EI_VERSION] != EV_CURRENT |
| `ELF_LOAD_BAD_OSABI` (-7) | e_ident[EI_OSABI] != ELFOSABI_NONE |
| `ELF_LOAD_BAD_TYPE` (-8) | e_type != ET_EXEC |
| `ELF_LOAD_BAD_MACHINE` (-9) | e_machine != EM_AARCH64 |
| `ELF_LOAD_BAD_FILE_VER` (-10) | e_version != EV_CURRENT |
| `ELF_LOAD_BAD_PHENTSIZE` (-11) | e_phentsize != sizeof(Elf64_Phdr) |
| `ELF_LOAD_NO_PHDRS` (-12) | e_phnum == 0 OR no PT_LOAD found |
| `ELF_LOAD_TOO_MANY_PHDRS` (-13) | e_phnum > 256 |
| `ELF_LOAD_PHTAB_OOB` (-14) | phoff + phnum*phentsize > size |
| `ELF_LOAD_HAS_INTERP` (-15) | PT_INTERP present (dynamic; v1.0 static) |
| `ELF_LOAD_RWX_REJECTED` (-16) | PT_LOAD with PF_W & PF_X both set — **I-12 violation** |
| `ELF_LOAD_BAD_FILESZ` (-17) | filesz > memsz |
| `ELF_LOAD_SEG_OOB` (-18) | file_offset + filesz > size |
| `ELF_LOAD_TOO_MANY_LOADS` (-19) | > ELF_MAX_LOAD_SEGMENTS PT_LOAD entries |
| `ELF_LOAD_BAD_ENTRY` (-20) | e_entry == 0 OR not in any LOAD segment |
| `ELF_LOAD_EXEC_STACK` (-21) | PT_GNU_STACK with PF_X (NX-stack policy) |

---

## Implementation

`kernel/elf.c` (~150 LOC).

### Validation pipeline

The loader runs a 5-stage validation pipeline; each stage gates the next:

1. **Stage 1 — `e_ident` validation**: magic + class + data + version + OSABI.
2. **Stage 2 — `e_type` / `e_machine` / `e_version`**: ET_EXEC + EM_AARCH64 + EV_CURRENT.
3. **Stage 3 — program-header table layout**: phentsize correct; phnum bounded; phtab within size (with overflow protection).
4. **Stage 4 — per-segment validation**: iterate program headers, collecting PT_LOAD entries with W^X / bounds enforcement; reject PT_INTERP + PT_GNU_STACK-with-PF_X.
5. **Stage 5 — entry validation**: e_entry is within at least one PT_LOAD segment's vaddr range.

Failure at any stage returns the corresponding negative error code; `*out` is left in an undefined (possibly partial) state.

### Overflow protection

Every additive size-check uses overflow-safe arithmetic:

```c
static bool u64_add_overflow(u64 a, u64 b, u64 *out) {
    if (a > ((u64)-1) - b) return true;
    *out = a + b;
    return false;
}
```

Used at: `phoff + phnum*phentsize` (Stage 3); `file_offset + filesz` (Stage 4); `vaddr + memsz` (Stage 5 entry check).

`phnum * phentsize` uses widening multiplication (`u32 * u32 → u64`) since both operands fit in u32.

### W^X enforcement

The check is on the architectural permission bits only (`PF_W | PF_X`); OS-specific (`PF_MASKOS`) and proc-specific (`PF_MASKPROC`) bits are masked off before the comparison:

```c
u32 wx_bits = p->p_flags & (PF_W | PF_X);
if (wx_bits == (PF_W | PF_X)) {
    return ELF_LOAD_RWX_REJECTED;
}
```

This catches both R+W+X (full RWX) and W+X without R. R+X (text), R (rodata), R+W (data), and combinations with OS bits set are all accepted.

### Permission bit storage

The loader strips OS/proc-specific bits before storing in `elf_load_segment.flags`:

```c
seg->flags = p->p_flags & (PF_R | PF_W | PF_X);
```

Phase 3+ exec uses these flags to derive PTE permissions when mapping the segment. The OS/proc bits are dropped because they don't translate to PTE state.

### Entry validation

`e_entry == 0` is rejected outright (matches musl + glibc loaders' policy; a valid binary's entry is never zero).

`e_entry` must fall within `[vaddr, vaddr + memsz)` of at least one PT_LOAD segment. At v1.0 the loader doesn't verify the segment is executable — Phase 3+ exec adds that check. The loader's job is structural; runtime gating is the caller's.

---

## Spec cross-reference

There is **no formal TLA+ spec** for the ELF loader. Per `CLAUDE.md`'s spec-first policy:

> Features that usually don't (pure computation, test helpers, config parsing, CLI glue): skip the spec; just write + test. Use judgment.

ELF parsing is config parsing. The W^X invariant is a single-pass precondition check, not a state machine; TLC modeling would be overkill. The validation pipeline is exhaustively covered by per-error-class tests (6 test cases, each verifying one or more rejection paths).

ARCH §28 I-12 (W^X) is enforced at three layers (PTE bit, mprotect, ELF loader). The PTE bit layer is the runtime root of trust; `mprotect` is a state-machine spec target (Phase 5+ when the syscall surface lands); the ELF loader is parse-time static check.

---

## Tests

- **`elf.parse_minimal_ok`** — single PT_LOAD R+X segment; verifies entry + segments[0].flags + memsz.
- **`elf.parse_multi_segment_ok`** — 3-segment binary (text RX + rodata R + data RW); each segment's flags preserved.
- **`elf.header_rejection`** — exhaustive header-field rejection: bad magic, ELFCLASS32, ELFDATA2MSB, EV_NONE (ident + file), bad OSABI, ET_REL, ET_DYN, non-AArch64 machine, wrong phentsize.
- **`elf.rwx_rejected`** — R+W+X and W+X without R both produce `ELF_LOAD_RWX_REJECTED`. Sanity: RX, RW, RW+OS-specific bits all accepted.
- **`elf.bounds_rejection`** — too small, NULL inputs, phnum=0, phnum>256, phtab beyond size, filesz>memsz, segment OOB.
- **`elf.policy_rejection`** — PT_INTERP rejected; PT_GNU_STACK with PF_X rejected; e_entry=0 rejected; e_entry outside any LOAD segment rejected.

Each test constructs a synthetic ELF blob in a static buffer (`g_test_elf_blob`, 4 KiB), then mutates one field for each negative case. The helper `build_elf(flags[], n_loads)` populates the buffer with a known-good baseline.

---

## Known caveats / footguns

### No mapping at v1.0

The loader returns `struct elf_image` with segment metadata; nothing is mapped into any address space. Phase 3 wires the actual mapping — segment data → VMO → vmo_map into the destination process's VMA tree.

### Static binaries only

`PT_INTERP` is rejected. Dynamic binaries (musl-dynamic, glibc-dynamic) require a userspace dynamic linker (`ld.so`) that loads its own segments + handles relocations. Deferred until the userspace layer is mature.

### No PIE / ET_DYN support

ET_DYN (position-independent executables) requires the dynamic relocator — applying R_AARCH64_RELATIVE etc. relocations against a chosen base. Deferred. The boot-time kernel KASLR relocator (in `arch/arm64/kaslr.c`) handles PIE for the kernel image; reusing it for userspace is forward work.

### No symbol table parsing

The loader doesn't process the section header table (e_shoff / e_shnum / e_shstrndx) at v1.0. Symbol resolution is the dynamic linker's job; debug info is consumed by host tools.

### Endianness

ELFDATA2MSB (big-endian) is rejected. ARM64 supports both BE and LE; Thylacine targets LE only.

### Maximum LOAD segments = 16

Real binaries have 2-4. 16 is generous. A binary with > 16 LOAD segments is pathological at v1.0 (the linker can be told to merge segments). Phase 5+ may grow this if a real-world binary needs it.

### Maximum phnum = 256

Sane upper bound. Real binaries have 5-15 program headers. Bounds the validation loop's total work.

### No alignment validation at v1.0

`p_align` and the `vaddr % p_align == p_offset % p_align` constraint are not validated. Phase 3+ mapping will need to verify this. At parse time we accept any alignment.

### Entry executability not verified

The loader checks that `e_entry` is within some LOAD segment's vaddr range, but doesn't verify the segment has PF_X. Phase 3+ exec adds the executability check. A binary with entry pointing into an RW segment parses successfully today but would crash at runtime trying to execute non-X memory.

### `filesz < memsz` (BSS)

The loader accepts `memsz > filesz` (the standard BSS pattern: filesz=0, memsz>0 means a zero-initialized segment). Phase 3+ mapping must allocate the extra `memsz - filesz` bytes from anonymous memory (or zero-fill).

---

## Status

| Component | State |
|---|---|
| `elf.h` API + `elf.c` impl | Landed (P2-Ga) |
| Header validation (magic/class/data/version/OSABI/type/machine) | Landed |
| Program-header table validation (phentsize/phnum/bounds) | Landed |
| Per-segment validation (W^X/bounds/filesz/memsz) | Landed |
| Entry validation | Landed |
| In-kernel tests | 6 added: parse_minimal_ok / parse_multi_segment_ok / header_rejection / rwx_rejected / bounds_rejection / policy_rejection |
| Mapping (segment → VMA) | Phase 3 (with demand-paging fault handler) |
| `exec()` syscall surface | Phase 5+ |
| ET_DYN / PIE support | Post-v1.0 |
| PT_INTERP / dynamic linking | Post-v1.0 |
| PT_DYNAMIC / PT_TLS / PT_GNU_RELRO | Post-v1.0 |
| Symbol table parsing | Userspace dynamic linker scope |
| Alignment validation | Phase 3 (when mapping makes it relevant) |
| Entry-segment executability check | Phase 3 (exec-time) |
