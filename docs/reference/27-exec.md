# Reference: kernel-internal exec (P3-Eb)

## Purpose

`exec_setup(p, blob, blob_size, *entry_out, *sp_out)` is the bridge between the parsed-ELF representation (P2-Ga `elf_load`) and the address-space machinery (P3-Da/Db/Dc VMA + demand paging). Given a clean Proc + an ELF blob, it populates the Proc's address space with one VMA per PT_LOAD segment + a user-stack VMA.

At v1.0 P3-Eb `exec_setup` is **kernel-internal only** — it does NOT transition the calling thread to EL0. The ERET-to-EL0 step is the asm trampoline at P3-Ed. Tests at P3-Eb validate the address-space population in isolation; end-to-end userspace runs at P3-Ed.

ARCH §16: "exec is the boundary between the kernel-internal Proc/Thread model and userspace. The exec primitive parses a binary, creates VMOs for each segment, maps them via the VMA layer, allocates a user stack, and arranges for the next ERET to land in EL0 at the binary's entry point."

## Public API

### `<thylacine/exec.h>`

```c
#define EXEC_USER_STACK_SIZE         (256ull * 1024)
#define EXEC_USER_STACK_TOP          0x0000000080000000ull
#define EXEC_USER_STACK_BASE         (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)
#define EXEC_USER_STACK_GUARD_SIZE   0x1000ull
#define EXEC_USER_STACK_GUARD_BASE   (EXEC_USER_STACK_BASE - EXEC_USER_STACK_GUARD_SIZE)

// System V process-startup frame (P6-pouch-kernel-auxv) — see
// "Initial process stack" below. EXEC_INIT_STACK_SIZE is a 16-aligned
// computed macro; it resolves to 176 (8 auxv entries since AT_HWCAP;
// AT_VDSO_CLOCK landed at #343).
#define EXEC_INIT_AUXV_COUNT     8
#define EXEC_INIT_STACK_SIZE     176   // argc+argv+envp + 8 auxv + 16 random
#define EXEC_INIT_RANDOM_OFFSET  160   // EXEC_INIT_STACK_SIZE - 16

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out);
```

The user stack is 256 KiB (since corvus-bringup-d — ML-KEM-768's FO-transform working set is tens of KiB; the prior 16 KiB overflowed) with a one-page guard VMA directly below it (P5-secondary-stack-guard — see "User-stack guard page").

#### Constraints (v1.0)

- `p` must be a non-kproc Proc (`pgtable_root != 0`; kproc has 0).
- `p` must currently have **no VMAs** (`p->vmas == NULL`). Phase 5+ "exec replaces in place" semantics will tear down existing VMAs first.
- Each PT_LOAD segment's `vaddr` and `file_offset` must be page-aligned (low 12 bits zero). Real toolchains (clang, gcc) page-align by default; the leniency for non-zero alignment lands post-v1.0.

#### Side effects on success

- One VMA per PT_LOAD segment, backed by a fresh anonymous BURROW. The BURROW's pages contain the segment's bytes from `blob[file_offset..]` (filesz bytes); the tail (memsz - filesz) is zero.
- One user-stack VMA at `[EXEC_USER_STACK_BASE, EXEC_USER_STACK_TOP)` (256 KiB) backed by a fresh zeroed anonymous BURROW, plus a one-page **guard VMA** at `[EXEC_USER_STACK_GUARD_BASE, EXEC_USER_STACK_BASE)` directly below it (`prot==0`, no BURROW — see "User-stack guard page").
- All caller-held BURROW handles dropped via `burrow_unref`. The mapping_count (held by the VMA) keeps each BURROW alive until `proc_free`'s `vma_drain`.
- A System V process-startup frame (argc / argv / envp / auxv) written into the top `EXEC_INIT_STACK_SIZE` bytes of the user stack — see "Initial process stack".
- `*entry_out = img.entry`; `*sp_out = EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE` (the user VA of the frame's `argc` word).

#### Side effects on failure

- Whatever VMAs were installed before the failing step remain installed. The Proc is in a partial state. v1.0 callers should dispose of the Proc (`proc_free` with `state=ZOMBIE`) on any non-zero return.

#### Returns

- `0` on success.
- `-1` on any failure: NULL inputs, `p` is kproc, `p` already has VMAs, ELF parse error, segment vaddr/file_offset misalignment, BURROW allocation OOM, vma_insert overlap.

## Implementation

### `kernel/exec.c`

The function decomposes into three helpers:

```c
static u32 vma_prot_for_elf(u32 elf_flags);
    // PF_R/W/X → VMA_PROT_READ/WRITE/EXEC.

static int exec_map_segment(struct Proc *p, const void *blob,
                            const struct elf_load_segment *seg);
    // 1. Reject non-page-aligned vaddr / file_offset.
    // 2. Round (vaddr + memsz) up to page → size for the BURROW.
    // 3. burrow_create_anon(size).
    // 4. Copy filesz bytes from blob[file_offset..] to burrow->pages[0..]
    //    via direct map (pa_to_kva).
    // 5. burrow_map(p, burrow, vaddr, size, prot).
    // 6. burrow_unref(burrow) — drop caller-held handle.

static int exec_map_user_stack(struct Proc *p);
    // 1. burrow_create_anon + burrow_map for the 256 KiB stack range.
    // 2. vma_alloc_guard + vma_insert for the one-page guard VMA
    //    directly below the stack (P5-secondary-stack-guard).

static u64 exec_build_init_stack(struct Proc *p, const struct elf_image *img);
    // P6-pouch-kernel-auxv. Writes the System V startup frame
    // (argc/argv/envp/auxv) into the top of the user stack; returns
    // the initial sp. See "Initial process stack".
```

`exec_setup` orchestrates: validates args, calls `elf_load`, iterates segments, maps the user stack, then builds the System V startup frame.

### Lifecycle interaction with BURROW refcount

```
exec_setup
   │
   ├── per segment:
   │      burrow_create_anon                  → handle_count=1, mapping_count=0
   │      burrow_map(p, ...)                  → mapping_count=1 (via vma_alloc)
   │      burrow_unref                        → handle_count=0, mapping_count=1; alive
   │                                          (mapping holds it)
   │
   └── user stack:
          (same pattern as segment)

proc_free
   │
   ├── vma_drain                           → for each VMA: vma_remove + vma_free
   │      │                                    → burrow_release_mapping → mapping_count--
   │      │                                    → at 0 (& handle_count==0 already):
   │      │                                       burrow_free_internal → free_pages
   │      │
   │      └── ... repeat for all VMAs
   │
   ├── handle_table_free                   → no-op for these VMOs (we never put
   │                                          them in the handle table)
   │
   └── proc_pgtable_destroy walker         → frees any L1/L2/L3 sub-tables
                                             populated by demand paging at runtime
```

The `burrow_unref` in `exec_setup` is the key step that lets the VMA-only-keeps-it-alive lifecycle work. Without it, the VMOs would have `handle_count=1` forever (no handle table entry to close), and `proc_free`'s `vma_drain` would only bring `mapping_count` to 0 — leaving `handle_count=1` and the pages alive. The burrow_unref drops handle_count to 0 immediately, so the eventual `mapping_count→0` triggers free.

### User-stack guard page (P5-secondary-stack-guard)

`exec_map_user_stack` installs, directly below the 256 KiB user stack, a one-page **guard VMA** at `[EXEC_USER_STACK_GUARD_BASE, EXEC_USER_STACK_BASE)` via `vma_alloc_guard` — a `prot==0`, no-BURROW reserved range (see `docs/reference/26-vma.md`).

Two properties:
- **Fault on overflow.** A stack overflow past `EXEC_USER_STACK_BASE` crosses into the guard VMA; `userland_demand_page` rejects the `prot==0` VMA (`FAULT_UNHANDLED_USER`) instead of the access silently corrupting a lower VMA.
- **Reservation.** `vma_insert`'s overlap rejection keeps the page unmapped: a future mapping allocator (Phase 5+ `mmap` / heap) cannot place anything flush against the stack.

The guard owns no physical page (no BURROW) — it costs one `struct Vma` and nothing else; `vma_drain` at `proc_free` frees it cleanly (the NULL-BURROW path). If an ELF segment's VMA already occupies the guard range, `vma_insert` rejects the guard and `exec_map_user_stack` returns -1 — `exec_setup` then fails and the caller disposes the partial Proc, the correct outcome for a binary mapping over its own stack guard. Closes corvus-bringup-d audit F7.

### Initial process stack — argc / argv / envp / auxv (P6-pouch-kernel-auxv)

After mapping the segments + the user stack, `exec_setup` calls `exec_build_init_stack` to write a **System V process-startup frame** into the top of the user stack. A C runtime (pouch — the Thylacine POSIX libc; `docs/POUCH-DESIGN.md`) reads `argc`, `argv`, `envp`, and the auxiliary vector from this frame at entry. `*sp_out` points at the frame's `argc` word.

The Shape-A frame is a fixed `EXEC_INIT_STACK_SIZE` (176) bytes — `argc == 0` (the argv-bearing Shape B is documented in `exec.h`) and room for the max eight-entry auxv. Layout, low → high address:

| Offset from sp | Bytes | Contents |
|---|---|---|
| 0   | 8   | `argc` — 0 |
| 8   | 8   | `argv[]` terminator (one NULL) |
| 16  | 8   | `envp[]` terminator (one NULL) |
| 24  | 128 | `auxv[]` — up to eight `Elf64_auxv_t` (16 B each) |
| 152 | 8   | alignment padding (zero, from `KP_ZERO`) |
| 160 | 16  | `AT_RANDOM` entropy block |
| 176 | —   | `EXEC_USER_STACK_TOP` |

The auxv entries (`a_type`, `a_val`), written by `exec_fill_auxv` (both frame shapes route through it):

| a_type | a_val |
|---|---|
| `AT_PHDR` (3)    | user VA of the ELF program-header table, or 0 |
| `AT_PHENT` (4)   | `e_phentsize` (56 — `sizeof(Elf64_Phdr)`) |
| `AT_PHNUM` (5)   | `e_phnum`, or 0 when `AT_PHDR` is unresolved |
| `AT_PAGESZ` (6)  | `PAGE_SIZE` (4096) |
| `AT_HWCAP` (16)  | the Linux-compatible arm64 CPU-feature word (`g_hw_features.linux_hwcap` — FP/ASIMD/AES/PMULL/SHA1/SHA2/SHA512/SHA3/CRC32/ATOMICS/ASIMDDP at the Linux uapi bit numbers, derived from ID_AA64ISAR0/PFR0 at boot; `hwcap_CPUID` is never set — see `12-hardening.md`) |
| `AT_RANDOM` (25) | user VA of the 16-byte entropy block (`sp + 160` in Shape A) |
| `AT_VDSO_CLOCK` (0x5654) | user VA of the RO clock page — OPTIONAL, present only when the vDSO page mapped (see `11-timer.md`); when absent the AT_NULL terminator moves up and the slot stays zeroed padding |
| `AT_NULL` (0)    | 0 — vector terminator |

The minimum a static musl process needs (per POUCH-DESIGN.md §12.1) plus the two informational entries. `AT_HWCAP` is the STANDARD SysV tag — musl's `getauxval`, libsodium's armcrypto runtime gate, and the Go runtime's `internal/cpu` init read it directly; consumers treat a clear bit as feature-absent (fail-safe on crypto-less cores). Other optional entries (`AT_SECURE`, `AT_CLKTCK`, ...) remain deliberately omitted — a C runtime supplies its own defaults for absent entries. All known consumers scan the vector by tag to `AT_NULL`; nothing parses by fixed offset.

**The initial sp** = `EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE`. It is 16-byte aligned — the AArch64 SysV ABI requirement — because `EXEC_INIT_STACK_SIZE` is rounded up to a 16-byte multiple and `EXEC_USER_STACK_TOP` is itself aligned. The header pins this with `_Static_assert(EXEC_INIT_STACK_SIZE % 16 == 0)`.

**AT_PHDR resolution.** The program headers live at file offset `img.phoff` (exposed by `elf_load` — see `docs/reference/21-elf.md`). `exec_build_init_stack` scans the loaded segments for the one whose file range `[file_offset, file_offset + filesz)` covers the entire phdr table, and translates: `AT_PHDR = seg.vaddr + (phoff - seg.file_offset)`. A well-formed static binary's first PT_LOAD spans the ELF header + phdrs, so this resolves to a valid, mapped, readable user VA. If no loaded segment covers the table, `AT_PHDR` and `AT_PHNUM` are reported 0 — a C runtime then skips the phdr walk, which is correct for a program with no `PT_TLS`.

**AT_RANDOM.** 16 bytes of kernel-CSPRNG entropy (`kern_random_bytes`), which a C runtime uses to seed its stack-protector canary + pointer-mangling cookie. The kernel-side scratch buffer is zero-initialised before the CSPRNG call, so a short read can never ship kernel-stack residue into userspace. The 8-byte pad slot is never written — it stays zero from the BURROW's `KP_ZERO` allocation. The frame therefore carries no uninitialised bytes and no kernel addresses.

The frame is written into the stack BURROW's backing pages through the kernel direct map — the BURROW is located via `vma_lookup(p, EXEC_USER_STACK_BASE)` after `exec_map_user_stack` has installed it. This is the same mechanism `exec_map_segment` uses for segment bytes. The frame is data (read by EL0 as data, never executed), so no I-cache maintenance is needed.

**Freestanding binaries are unaffected.** Thylacine-native binaries built against `libt` / `libthyla-rs` have a `_start` that calls `main` directly and never reads the stack frame; the 144-byte frame simply sits above their initial sp, ignored. The frame is consumed only by a SysV-aware C runtime (pouch). Every existing binary (joey, corvus, the bringup probes) boots unchanged with the new sp.

## Data structures

No new data structures at P3-Eb. `exec_setup` writes to existing surfaces:
- `struct Proc.vmas` (via `vma_insert`).
- `struct Burrow` (via `burrow_create_anon`).
- The buddy allocator (via `alloc_pages` inside `burrow_create_anon`).

## State machines

### exec_setup state flow

```
START
   │
   │ validate args (p magic / non-kproc / clean / blob / out-params)
   ▼
ELF_LOAD (parse + validate via elf_load)
   │
   │ for each PT_LOAD segment:
   ▼
SEGMENT_MAP
   │
   ├── alignment check (vaddr / file_offset page-aligned)
   ├── compute aligned size
   ├── burrow_create_anon
   ├── copy blob bytes via direct map
   ├── burrow_map (vma_alloc + vma_insert)
   └── burrow_unref
   │
   │ (loop until all segments mapped)
   ▼
STACK_MAP (burrow_create_anon + burrow_map for the user stack)
   │
   ▼
INIT_STACK (exec_build_init_stack — write the argc/argv/envp/auxv frame)
   │
   ▼
RETURN 0; *entry_out = img.entry
          *sp_out  = EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE
```

Failure at any step returns -1 with the partial state intact (caller's responsibility to dispose).

## Spec cross-reference

No new TLA+ spec at P3-Eb. The function is a sequence of already-spec'd primitives:
- `burrow_create_anon` / `burrow_unref` mapped to `burrow.tla` actions.
- `burrow_map` (high-level entry) wraps `vma_alloc + vma_insert + burrow_acquire_mapping`.
- The orchestration is structurally simple under the v1.0 single-thread-Proc invariant.

Phase 5+ exec(2) syscall semantics — exec replaces the calling Proc's image atomically; failure must roll back to the prior image — is the spec-extension point. v1.0 P3-Eb's "create fresh Proc, exec into it" pattern doesn't have those failure-atomicity requirements.

## Tests

`kernel/test/test_exec.c` — eight tests:

- `exec.setup_smoke`: minimal valid ELF; verify single segment VMA at vaddr + user stack VMA + entry/sp out params.
- `exec.setup_segment_data_copied`: ELF with 256 bytes of recognizable data; verify bytes are copied into BURROW backing pages (read via direct map); tail of page is zero.
- `exec.setup_constraints`: NULL inputs / NULL out params / kproc-rejected (covered indirectly by p->vmas check) / corrupt ELF magic / unaligned segment vaddr — all return -1.
- `exec.setup_multi_segment`: text RX + rodata R + data RW; verify all three VMA prot bits + user stack.
- `exec.setup_lifecycle_round_trip`: 2-segment exec + proc_free → `phys_free_pages` returns to baseline (all VMOs + sub-tables freed).
- `exec.user_stack_guard`: verify the user-stack guard VMA — present at `[GUARD_BASE, STACK_BASE)`, `prot==0`, `burrow==NULL`, distinct from the stack VMA — and that a VMA overlapping the guard is rejected by `vma_insert` (the reservation property). Closes corvus-bringup-d audit F7.
- `exec.setup_auxv` (P6-pouch-kernel-auxv): ELF whose first PT_LOAD covers the program headers; reads the System V startup frame back from the stack BURROW and verifies the argc/argv/envp NULLs, all six auxv entries (types + values), a resolved `AT_PHDR`, `sp == EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE` (16-aligned), and the `AT_RANDOM` block in-range + CSPRNG-populated (non-zero).
- `exec.setup_auxv_no_phdr_segment` (P6-pouch-kernel-auxv): ELF whose loaded segments do not cover the phdr table; verifies the unresolved-fallback path — `AT_PHDR == 0` / `AT_PHNUM == 0` — with the rest of the frame well-formed.

Each test synthesizes an ELF in a static aligned buffer (`g_elf_blob`); the same idiom as `test_elf.c::build_elf`.

## Error paths

- `exec_setup` returns -1 on any failure. Caller (currently kernel test code) disposes of the Proc via `proc_free` with `state=ZOMBIE`. The Proc's `vma_drain` correctly releases whatever VMAs were installed before the failure, restoring BURROW refcounts.
- `burrow_create_anon` OOM during a segment map: that segment's BURROW never installed; prior segments remain.
- `burrow_map` overlap (e.g., two PT_LOAD segments with overlapping vaddr ranges): the overlap is rejected at `vma_insert`; `exec_map_segment` calls `burrow_unref` (rolling back the implicit `burrow_acquire_mapping` taken inside `vma_alloc`) and returns -1.

## Performance characteristics

- ELF parse: ~tens of microseconds for typical (≤ 16) PT_LOAD segments.
- Per-segment cost: one `burrow_create_anon` (one `alloc_pages(order)`) + one byte-copy of `filesz` bytes + one `burrow_map`. For a 4 KiB segment with 4 KiB filesz: roughly 10 µs (allocation) + 4 µs (memcpy) + 5 µs (burrow_map's vma_alloc + vma_insert). Larger segments scale linearly with filesz for the memcpy.
- User stack: one `burrow_create_anon(256 KiB)` + one `burrow_map`, plus a `vma_alloc_guard` + `vma_insert` for the guard VMA (no allocation — negligible). Roughly 30 µs.

Total exec_setup for a small static ELF: ~50–200 µs. The largest cost is the byte-copy for segments with large filesz; Phase 5+ may switch to mmap-style "borrow" semantics for read-only segments to avoid the copy.

## Status

- **Implemented at P3-Eb**: `exec_setup` + segment + stack helpers + 5 tests + reference doc.
- **Stubbed**: ERET-to-EL0 transition (the asm trampoline at P3-Ed).
- **Stubbed**: SVC syscall handler (P3-Ec).
- **Stubbed**: ELF fixture build infrastructure + end-to-end exec test (P3-Ed).
- **Stubbed**: exec syscall surface (Phase 5+ syscall layer).
- **P6-pouch-kernel-auxv landed**: `exec_build_init_stack` writes the System V process-startup frame (argc / argv / envp / auxv) at the top of the user stack; `*sp_out` now points at the frame's `argc` word, 144 bytes below `EXEC_USER_STACK_TOP`. 2 new tests.

Commit landing point: `9f0d1b6` (P3-Eb); auxv frame at P6-pouch-kernel-auxv.

## Known caveats / footguns

1. **Page-aligned segments only at v1.0**. Real ELF spec permits `vaddr ≡ offset (mod p_align)` with non-zero low bits. v1.0 rejects. Toolchain output (clang, gcc) page-aligns by default so this is rarely an issue in practice.

2. **Single BURROW per segment**. If two PT_LOAD segments share a virtual page (e.g., a code segment ending mid-page where the rodata segment starts), v1.0 may reject the rodata segment due to vma_insert overlap. Real toolchains pad PT_LOADs to page boundaries; this is rarely an issue. Phase 5+ may merge overlapping segments into a single BURROW with per-page prot.

3. **No replace-in-place at v1.0**. `p->vmas != NULL` is rejected. The `exec(2)` syscall semantics — replace the calling Proc's image atomically with rollback on failure — lands at Phase 5+.

4. **BURROW_TYPE_ANON only**. v1.0 anonymous VMOs eagerly allocate backing pages. Phase 5+ BURROW_TYPE_FILE (Stratum-backed) allows the segment data to come directly from the page cache without a per-exec memcpy — significant for large binaries.

5. **No copy-on-write (COW) for shared text**. Two execs of the same binary each allocate fresh VMOs + copy bytes. Phase 5+ COW lets multiple Procs share read-only segment VMOs.

6. **User stack is fixed-size 256 KiB at a fixed VA**, with a one-page guard VMA directly below it (P5-secondary-stack-guard). Phase 5+ adds growable stack via demand-page-on-fault below stack base, plus per-Proc stack VA randomization (ASLR for stack).

7. **Caller is responsible for partial-state cleanup**. On `exec_setup` failure (non-zero return), the Proc is in a partial state with some VMAs installed and some not. Caller (test code at v1.0; future exec syscall handler) calls `proc_free` with `state=ZOMBIE` to clean up.

## Naming rationale

`exec_setup` (not `exec` proper) — emphasizes that this is the load-and-map step, NOT the transition-to-EL0 step. The full exec syscall (Phase 5+) is `exec()` + the asm trampoline; `exec_setup` is the address-space-population half.
