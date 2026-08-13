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
- **ET_EXEC or ET_DYN**. A PIE is placed at `ELF_PIE_LOAD_BIAS` (DISTRO D-2); ET_CORE / ET_REL stay refused.
- **EM_AARCH64** only.
- **No PT_INTERP** (the kernel loads one image per exec and runs no interpreter; DISTRO D-4 owns the rewrite-to-ldso route). `PT_DYNAMIC` is accepted on an ET_DYN and refused on an ET_EXEC.
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
    u64                       phoff;        // e_phoff — phdr-table file offset
    u16                       phnum;        // e_phnum
    u16                       phentsize;    // e_phentsize (== sizeof(Elf64_Phdr))
    int                       n_segments;
    struct elf_load_segment   segments[ELF_MAX_LOAD_SEGMENTS];
};

int elf_load(const void *blob, size_t size, struct elf_image *out);
```

`phoff` / `phnum` / `phentsize` (P6-pouch-kernel-auxv) carry the program-header table's location — `exec_setup` consumes them to build the `AT_PHDR` / `AT_PHENT` / `AT_PHNUM` entries of the process auxiliary vector (see `docs/reference/27-exec.md`). All three are validated by `elf_load` before being stored: `phentsize == sizeof(Elf64_Phdr)`; `phoff + phnum*phentsize ≤ size` (overflow-checked); `phoff` 8-byte aligned. `<thylacine/elf.h>` additionally defines the auxv ABI — the `AT_*` `a_type` constants + `struct Elf64_auxv_t` — consumed by `exec_build_init_stack`.

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
| `ELF_LOAD_BAD_TYPE` (-8) | e_type is neither ET_EXEC nor ET_DYN |
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
2. **Stage 2 — `e_type` / `e_machine` / `e_version`**: ET_EXEC or ET_DYN, + EM_AARCH64 + EV_CURRENT. The type selects the load bias (`0` / `ELF_PIE_LOAD_BIAS`) and nothing else branches on it again.
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
- **`elf.header_rejection`** — exhaustive header-field rejection: bad magic, ELFCLASS32, ELFDATA2MSB, EV_NONE (ident + file), bad OSABI, ET_REL, ET_CORE, non-AArch64 machine, wrong phentsize (ET_DYN is now ACCEPTED -- see `elf.pie_load_bias`).
- **`elf.rwx_rejected`** — R+W+X and W+X without R both produce `ELF_LOAD_RWX_REJECTED`. Sanity: RX, RW, RW+OS-specific bits all accepted.
- **`elf.bounds_rejection`** — too small, NULL inputs, phnum=0, phnum>256, phtab beyond size, filesz>memsz, segment OOB.
- **`elf.policy_rejection`** — PT_INTERP rejected; PT_GNU_STACK with PF_X rejected; e_entry=0 rejected; e_entry outside any LOAD segment rejected.

Each test constructs a synthetic ELF blob in a static buffer (`g_test_elf_blob`, 4 KiB), then mutates one field for each negative case. The helper `build_elf(flags[], n_loads)` populates the buffer with a known-good baseline.

---

## Known caveats / footguns

### No mapping at v1.0

The loader returns `struct elf_image` with segment metadata; nothing is mapped into any address space. Phase 3 wires the actual mapping — segment data → BURROW → burrow_map into the destination process's VMA tree.

### Static binaries only

`PT_INTERP` is rejected. Dynamic binaries (musl-dynamic, glibc-dynamic) require a userspace dynamic linker (`ld.so`) that loads its own segments + handles relocations. Deferred until the userspace layer is mature.

### ET_DYN / PIE placement (DISTRO D-2)

An ET_DYN's `p_vaddr` values are offsets from a base the loader chooses. `elf_load` adds `ELF_PIE_LOAD_BIAS` (`0x2000_0000`, 512 MiB) to `e_entry` and to every PT_LOAD's `vaddr`, records the bias + `e_type` in `struct elf_image`, and returns FINAL addresses. **That is the entire change.** Everything downstream — `exec_load_into`'s segment loop, the `file_shareable` gate, the `#149` page-sharing refusal, `seg_geometry`, the `AT_PHDR` translation — reads final addresses and needed no edit, which is why the W^X and segment gates are byte-identical either side of D-2.

**No relocator is needed, and that is not an omission.** The two ET_DYN shapes that reach this loader both self-relocate before executing anything that depends on it: a static-PIE from its own `rcrt1.o` entry, and stock ldso in `_dlstart` (which computes its base PC-relatively, not from auxv). Thylacine therefore never applies an `R_AARCH64_RELATIVE`; it only has to place the image and say where it put it.

Bounds (`elf.c`, ET_DYN arm only):

- Every biased segment top must land below `ELF_PIE_LOAD_LIMIT` (`0x6000_0000`), overflow-checked in both additions → `ELF_LOAD_PIE_OOB`. Refusing here rather than at `vma_insert` means the *loader* states what it will place, instead of a general allocator rule catching it downstream.
- `exec.h` pins `ELF_PIE_LOAD_LIMIT <= EXEC_USER_STACK_GUARD_BASE` with a `_Static_assert`, so "a PIE can never be placed into the stack guard or above" is a build-time fact.
- The bias is 64 KiB-aligned so a stock aarch64 ELF's `p_vaddr == p_offset (mod p_align)` congruence (Alpine uses `p_align` `0x10000`) survives it — which is what keeps a segment's eligibility for the shared file-backed arm the same biased as unbiased.

The window sits ~100× above the highest ET_EXEC extent we ship (measured 2026-08-06: `joey` `0x400000..0x47e349`, `pouch-hello` `0x200000..0x208770`), so a PIE and a fixed-address executable can never be confused in a fault report. **One constant, deliberately** — per-exec randomization is a recorded I-16-adjacent seam (`docs/DISTRO.md` §1 non-goals); when it lands, the natural shape is a parameter on `elf_load` rather than a constant in `elf.h`.

### No symbol table parsing

The loader doesn't process the section header table (e_shoff / e_shnum / e_shstrndx) at v1.0. Symbol resolution is the dynamic linker's job; debug info is consumed by host tools.

### Endianness

ELFDATA2MSB (big-endian) is rejected. ARM64 supports both BE and LE; Thylacine targets LE only.

### Alignment requirements (R5-G)

The `blob` pointer passed to `elf_load` MUST be at least 8-byte aligned (`_Alignof(struct Elf64_Ehdr)`). Misaligned blobs are rejected with `ELF_LOAD_BAD_ALIGN`. Kernel callers using `kmalloc` / page-allocator memory satisfy this trivially; callers passing slices into other buffers must ensure alignment themselves.

Similarly, `e_phoff` MUST be 8-byte aligned (`_Alignof(struct Elf64_Phdr)`). Real linkers always emit `e_phoff = 64`, naturally aligned. Misaligned phoff is rejected with `ELF_LOAD_PHTAB_OOB`.

Both checks defend against UBSan-trapping kernel builds (`-fsanitize=alignment` is part of `-fsanitize=undefined`) — without them, an attacker submitting a misaligned ELF could trigger kernel BRK as a denial-of-service primitive.

**Both apply to `elf_read_interp` too, since #215 (2026-08-13).** They were written when `elf_load` was the only consumer of `e_phoff`; DISTRO D-4 promoted the `PT_INTERP` walk into a second public parser of the same attacker-controlled field and it inherited the bounds without the alignment — the premise R5-G established, voided by a later chunk. `elf_read_interp` now rejects both a misaligned `blob` and an odd `e_phoff`, answering **0** (its existing "no interpreter here") rather than a new error code, since it returns a length and 0 already covers every malformed case.

The two are independent and neither implies the other: `blob` is the kernel's own buffer, so only a caller can misalign it, while `e_phoff` comes straight off the wire and can misalign the Phdr cast however well-aligned the buffer is. `elf.read_interp` case (9) asserts each separately, with an aligned-relocation control on the `e_phoff` leg so a 0 is attributable to the alignment and not to a damaged fixture.

### W^X check is type-blind (R5-G F64)

The W^X check (`p->p_flags & (PF_W | PF_X) == (PF_W | PF_X)` → reject) is hoisted ABOVE the switch over `p->p_type`. Every program header is flag-checked regardless of type, so future segment types automatically inherit the defense. The PT_LOAD-only check would have left an attack surface where a future code-recognized PT_* type with PF_W|PF_X bypasses W^X.

### Interpreter policy (R5-G F63, narrowed at DISTRO D-2)

R5-G rejected `PT_INTERP` and `PT_DYNAMIC` together, as complementary evidence that a binary was dynamic. D-2 splits them, because they turned out to mean different things:

- **`PT_INTERP` → still rejected, unconditionally.** It names an interpreter, and the kernel loads exactly ONE image per exec and runs none. D-4's rewrite-to-ldso route reads this segment at the *vivarium exec chokepoint* and restarts resolution on the interpreter, so what reaches `elf_load` is the interpreter itself — which has no `PT_INTERP` of its own (measured: stock `ld-musl-aarch64.so.1` carries none). The reject stays correct on both sides of D-4.
- **`PT_DYNAMIC` → rejected on ET_EXEC, accepted on ET_DYN.** Every PIE carries one: a static-PIE for its self-applied RELA table, ldso for its own. The loader still never PROCESSES it (see "ET_DYN / PIE placement"), so accepting the segment costs nothing. An ET_EXEC carrying one is still refused — it wants an interpreter this loader does not run, and its relocations have no self-applying startup path.

`elf.pie_load_bias` asserts both directions, because accepting `PT_DYNAMIC` everywhere would satisfy a one-sided test.

**AS-BUILT at DISTRO D-4 (2026-08-10).** The reject is unchanged and still correct, but
it is no longer the whole story: `exec_load_into` now READS `ELF_LOAD_HAS_INTERP` as a
dispatch signal for a `PHENO_LINUX` image and restarts the load on the interpreter
(`docs/reference/27-exec.md`, "the PT_INTERP rewrite"). `elf_load` itself did not change
-- it is still handed exactly one image and still refuses one that names an interpreter,
which is what makes the one-level rule structural rather than enforced.

### `elf_read_interp` (DISTRO D-4)

```c
#define ELF_INTERP_MAX 255
size_t elf_read_interp(const void *blob, size_t size, char *out, size_t out_cap);
```

The bounded `PT_INTERP` walk, extracted so `elf_brand_hint` and the D-4 rewrite share ONE
copy of "is this offset inside the prefix, is this string terminated". PURE; safe on the
bounded header PREFIX `exec_read_header` produces. Returns the path's length, or **0 for
every absent case** -- unreadable, outside the prefix, unterminated, empty, longer than
`ELF_INTERP_MAX`, larger than the caller's buffer, or (since #215) misaligned in either
the `blob` pointer or `e_phoff` -- and clears `out` when it does.

PURE is load-bearing and is why the alignment rejections are silent: the file has no
console dependency, exactly as `elf_load` returns `ELF_LOAD_BAD_ALIGN` rather than
printing. A caller that hands a misaligned buffer therefore sees every dynamic binary
read as static, with no diagnostic; the guard's comment in `kernel/elf.c` is where that
reader is expected to land.

**The hint inherited the stricter bar, and that is recorded rather than
reverted** (D-4 round F3): an ELF whose interp string is over-long or
unterminated but which CONTAINS `ld-musl` would have been branded
`LINUX_LIKELY` by a raw byte scan and is now `UNKNOWN`. No soundness impact --
the hint is consulted only on an already-FAILED load and never changes an
outcome, so the load still fails identically; only the explanatory line is
withheld from a header too malformed to explain.

The bar here is stricter than the hint's, because D-4 ACTS on the answer: it resolves the
returned path and execs it. A truncated `/lib/ld-musl-aarch64.so.1` is `/lib/ld`, which
names a DIFFERENT file, so "absent" is the only safe report for a path that did not
survive whole. `elf.read_interp` asserts each case, including that the buffer is left
empty rather than holding the previous call's answer.

### Output zeroed on entry (R5-G F70)

`elf_load` now zeros `*out` at function entry (after NULL check). The contract still says "ignore on non-OK return," but partial-population on early-exit failure now leaves a defined-zero state rather than attacker-controlled data. Defensive against buggy callers that read `out->entry` regardless of return code.

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
| ET_DYN / PIE support | LANDED (DISTRO D-2) |
| PT_INTERP extraction (`elf_read_interp`) | LANDED (DISTRO D-4) |
| PT_INTERP dispatch (rewrite-to-ldso) | LANDED in `exec.c` (DISTRO D-4); `elf_load` still refuses |
| PT_DYNAMIC | Accepted on ET_DYN (D-2); refused on ET_EXEC |
| PT_TLS / PT_GNU_RELRO | Skipped -- a PT_LOAD already covers them |
| Symbol table parsing | Userspace dynamic linker scope |
| Alignment validation | Phase 3 (when mapping makes it relevant) |
| Entry-segment executability check | Phase 3 (exec-time) |
