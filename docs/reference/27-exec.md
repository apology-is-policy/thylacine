# Reference: kernel-internal exec (P3-Eb)

## Purpose

`exec_setup(p, blob, blob_size, *entry_out, *sp_out)` is the bridge between the parsed-ELF representation (P2-Ga `elf_load`) and the address-space machinery (P3-Da/Db/Dc VMA + demand paging). Given a clean Proc + an ELF blob, it populates the Proc's address space with one VMA per PT_LOAD segment + a user-stack VMA.

At v1.0 P3-Eb `exec_setup` is **kernel-internal only** — it does NOT transition the calling thread to EL0. The ERET-to-EL0 step is the asm trampoline at P3-Ed. Tests at P3-Eb validate the address-space population in isolation; end-to-end userspace runs at P3-Ed.

ARCH §16: "exec is the boundary between the kernel-internal Proc/Thread model and userspace. The exec primitive parses a binary, creates VMOs for each segment, maps them via the VMA layer, allocates a user stack, and arranges for the next ERET to land in EL0 at the binary's entry point."

## Public API

### `<thylacine/exec.h>`

```c
#define EXEC_USER_STACK_SIZE   (16ull * 1024)
#define EXEC_USER_STACK_TOP    0x0000000080000000ull
#define EXEC_USER_STACK_BASE   (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out);
```

#### Constraints (v1.0)

- `p` must be a non-kproc Proc (`pgtable_root != 0`; kproc has 0).
- `p` must currently have **no VMAs** (`p->vmas == NULL`). Phase 5+ "exec replaces in place" semantics will tear down existing VMAs first.
- Each PT_LOAD segment's `vaddr` and `file_offset` must be page-aligned (low 12 bits zero). Real toolchains (clang, gcc) page-align by default; the leniency for non-zero alignment lands post-v1.0.

#### Side effects on success

- One VMA per PT_LOAD segment, backed by a fresh anonymous BURROW. The BURROW's pages contain the segment's bytes from `blob[file_offset..]` (filesz bytes); the tail (memsz - filesz) is zero.
- One user-stack VMA at `[EXEC_USER_STACK_BASE, EXEC_USER_STACK_TOP)` backed by a fresh zeroed anonymous BURROW.
- All caller-held BURROW handles dropped via `burrow_unref`. The mapping_count (held by the VMA) keeps each BURROW alive until `proc_free`'s `vma_drain`.
- `*entry_out = img.entry`, `*sp_out = EXEC_USER_STACK_TOP`.

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
    // Same shape as exec_map_segment but for the fixed stack range.
```

`exec_setup` orchestrates: validates args, calls `elf_load`, iterates segments, then maps the user stack.

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
RETURN 0; *entry_out = img.entry; *sp_out = EXEC_USER_STACK_TOP
```

Failure at any step returns -1 with the partial state intact (caller's responsibility to dispose).

## Spec cross-reference

No new TLA+ spec at P3-Eb. The function is a sequence of already-spec'd primitives:
- `burrow_create_anon` / `burrow_unref` mapped to `burrow.tla` actions.
- `burrow_map` (high-level entry) wraps `vma_alloc + vma_insert + burrow_acquire_mapping`.
- The orchestration is structurally simple under the v1.0 single-thread-Proc invariant.

Phase 5+ exec(2) syscall semantics — exec replaces the calling Proc's image atomically; failure must roll back to the prior image — is the spec-extension point. v1.0 P3-Eb's "create fresh Proc, exec into it" pattern doesn't have those failure-atomicity requirements.

## Tests

`kernel/test/test_exec.c` — five tests:

- `exec.setup_smoke`: minimal valid ELF; verify single segment VMA at vaddr + user stack VMA + entry/sp out params.
- `exec.setup_segment_data_copied`: ELF with 256 bytes of recognizable data; verify bytes are copied into BURROW backing pages (read via direct map); tail of page is zero.
- `exec.setup_constraints`: NULL inputs / NULL out params / kproc-rejected (covered indirectly by p->vmas check) / corrupt ELF magic / unaligned segment vaddr — all return -1.
- `exec.setup_multi_segment`: text RX + rodata R + data RW; verify all three VMA prot bits + user stack.
- `exec.setup_lifecycle_round_trip`: 2-segment exec + proc_free → `phys_free_pages` returns to baseline (all VMOs + sub-tables freed).

Each test synthesizes an ELF in a static aligned buffer (`g_elf_blob`); the same idiom as `test_elf.c::build_elf`.

## Error paths

- `exec_setup` returns -1 on any failure. Caller (currently kernel test code) disposes of the Proc via `proc_free` with `state=ZOMBIE`. The Proc's `vma_drain` correctly releases whatever VMAs were installed before the failure, restoring BURROW refcounts.
- `burrow_create_anon` OOM during a segment map: that segment's BURROW never installed; prior segments remain.
- `burrow_map` overlap (e.g., two PT_LOAD segments with overlapping vaddr ranges): the overlap is rejected at `vma_insert`; `exec_map_segment` calls `burrow_unref` (rolling back the implicit `burrow_acquire_mapping` taken inside `vma_alloc`) and returns -1.

## Performance characteristics

- ELF parse: ~tens of microseconds for typical (≤ 16) PT_LOAD segments.
- Per-segment cost: one `burrow_create_anon` (one `alloc_pages(order)`) + one byte-copy of `filesz` bytes + one `burrow_map`. For a 4 KiB segment with 4 KiB filesz: roughly 10 µs (allocation) + 4 µs (memcpy) + 5 µs (burrow_map's vma_alloc + vma_insert). Larger segments scale linearly with filesz for the memcpy.
- User stack: one `burrow_create_anon(16 KiB)` + one `burrow_map`. Roughly 15 µs.

Total exec_setup for a small static ELF: ~50–200 µs. The largest cost is the byte-copy for segments with large filesz; Phase 5+ may switch to mmap-style "borrow" semantics for read-only segments to avoid the copy.

## Status

- **Implemented at P3-Eb**: `exec_setup` + segment + stack helpers + 5 tests + reference doc.
- **Stubbed**: ERET-to-EL0 transition (the asm trampoline at P3-Ed).
- **Stubbed**: SVC syscall handler (P3-Ec).
- **Stubbed**: ELF fixture build infrastructure + end-to-end exec test (P3-Ed).
- **Stubbed**: exec syscall surface (Phase 5+ syscall layer).

Commit landing point: `9f0d1b6`.

## Known caveats / footguns

1. **Page-aligned segments only at v1.0**. Real ELF spec permits `vaddr ≡ offset (mod p_align)` with non-zero low bits. v1.0 rejects. Toolchain output (clang, gcc) page-aligns by default so this is rarely an issue in practice.

2. **Single BURROW per segment**. If two PT_LOAD segments share a virtual page (e.g., a code segment ending mid-page where the rodata segment starts), v1.0 may reject the rodata segment due to vma_insert overlap. Real toolchains pad PT_LOADs to page boundaries; this is rarely an issue. Phase 5+ may merge overlapping segments into a single BURROW with per-page prot.

3. **No replace-in-place at v1.0**. `p->vmas != NULL` is rejected. The `exec(2)` syscall semantics — replace the calling Proc's image atomically with rollback on failure — lands at Phase 5+.

4. **BURROW_TYPE_ANON only**. v1.0 anonymous VMOs eagerly allocate backing pages. Phase 5+ BURROW_TYPE_FILE (Stratum-backed) allows the segment data to come directly from the page cache without a per-exec memcpy — significant for large binaries.

5. **No copy-on-write (COW) for shared text**. Two execs of the same binary each allocate fresh VMOs + copy bytes. Phase 5+ COW lets multiple Procs share read-only segment VMOs.

6. **User stack is fixed-size 16 KiB at fixed VA**. Phase 5+ adds growable stack via demand-page-on-fault below stack base, plus per-Proc stack VA randomization (ASLR for stack).

7. **Caller is responsible for partial-state cleanup**. On `exec_setup` failure (non-zero return), the Proc is in a partial state with some VMAs installed and some not. Caller (test code at v1.0; future exec syscall handler) calls `proc_free` with `state=ZOMBIE` to clean up.

## Naming rationale

`exec_setup` (not `exec` proper) — emphasizes that this is the load-and-map step, NOT the transition-to-EL0 step. The full exec syscall (Phase 5+) is `exec()` + the asm trampoline; `exec_setup` is the address-space-population half.
