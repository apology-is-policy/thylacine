# Reference: kernel-internal exec (P3-Eb)

## Purpose

`exec_setup(p, blob, blob_size, *entry_out, *sp_out)` is the bridge between the parsed-ELF representation (P2-Ga `elf_load`) and the address-space machinery (P3-Da/Db/Dc VMA + demand paging). Given a clean Proc + an ELF blob, it populates the Proc's address space with one VMA per PT_LOAD segment + a user-stack VMA.

At v1.0 P3-Eb `exec_setup` is **kernel-internal only** — it does NOT transition the calling thread to EL0. The ERET-to-EL0 step is the asm trampoline at P3-Ed. Tests at P3-Eb validate the address-space population in isolation; end-to-end userspace runs at P3-Ed.

ARCH §16: "exec is the boundary between the kernel-internal Proc/Thread model and userspace. The exec primitive parses a binary, creates VMOs for each segment, maps them via the VMA layer, allocates a user stack, and arranges for the next ERET to land in EL0 at the binary's entry point."

## Public API

### `<thylacine/exec.h>`

```c
#define EXEC_USER_STACK_SIZE         (1024ull * 1024)   // 1 MiB
#define EXEC_USER_STACK_TOP          0x0000000080000000ull
#define EXEC_USER_STACK_BASE         (EXEC_USER_STACK_TOP - EXEC_USER_STACK_SIZE)
#define EXEC_USER_STACK_GUARD_SIZE   0x1000ull
#define EXEC_USER_STACK_GUARD_BASE   (EXEC_USER_STACK_BASE - EXEC_USER_STACK_GUARD_SIZE)

// System V process-startup frame (P6-pouch-kernel-auxv) — see
// "Initial process stack" below. EXEC_INIT_STACK_SIZE is a 16-aligned
// computed macro; it resolves to 192 (9 auxv entries: AT_HWCAP, then
// AT_VDSO_CLOCK at #343, then AT_ENTRY at DISTRO D-2).
#define EXEC_INIT_AUXV_COUNT     9
#define EXEC_INIT_STACK_SIZE     192   // argc+argv+envp + 9 auxv + 16 random
#define EXEC_INIT_RANDOM_OFFSET  176   // EXEC_INIT_STACK_SIZE - 16

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out);
```

The user stack is 1 MiB (256 KiB since corvus-bringup-d — ML-KEM-768's FO-transform working set is tens of KiB; the prior 16 KiB overflowed) with a one-page guard VMA directly below it (P5-secondary-stack-guard — see "User-stack guard page").

#### Constraints (v1.0)

- `p` must be a non-kproc Proc (`pgtable_root != 0`; kproc has 0).
- `p` must currently have **no VMAs** (`p->vmas == NULL`). Phase 5+ "exec replaces in place" semantics will tear down existing VMAs first.
- Each PT_LOAD segment's `vaddr` and `file_offset` must be page-aligned (low 12 bits zero). Real toolchains (clang, gcc) page-align by default; the leniency for non-zero alignment lands post-v1.0.

#### Side effects on success

- One VMA per PT_LOAD segment, backed by a fresh anonymous BURROW. The BURROW's pages contain the segment's bytes from `blob[file_offset..]` (filesz bytes); the tail (memsz - filesz) is zero.
- One user-stack VMA at `[EXEC_USER_STACK_BASE, EXEC_USER_STACK_TOP)` (1 MiB) backed by a fresh anonymous BURROW -- SPARSE since L-4a, plus a one-page **guard VMA** at `[EXEC_USER_STACK_GUARD_BASE, EXEC_USER_STACK_BASE)` directly below it (`prot==0`, no BURROW — see "User-stack guard page").
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
    // 1. burrow_create_anon_lazy + burrow_map for the 1 MiB stack range (L-4a).
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

`exec_map_user_stack` installs, directly below the 1 MiB user stack, a one-page **guard VMA** at `[EXEC_USER_STACK_GUARD_BASE, EXEC_USER_STACK_BASE)` via `vma_alloc_guard` — a `prot==0`, no-BURROW reserved range (see `docs/reference/26-vma.md`).

Two properties:
- **Fault on overflow.** A stack overflow past `EXEC_USER_STACK_BASE` crosses into the guard VMA; `userland_demand_page` rejects the `prot==0` VMA (`FAULT_UNHANDLED_USER`) instead of the access silently corrupting a lower VMA.
- **Reservation.** `vma_insert`'s overlap rejection keeps the page unmapped: a future mapping allocator (Phase 5+ `mmap` / heap) cannot place anything flush against the stack.

The guard owns no physical page (no BURROW) — it costs one `struct Vma` and nothing else; `vma_drain` at `proc_free` frees it cleanly (the NULL-BURROW path). If an ELF segment's VMA already occupies the guard range, `vma_insert` rejects the guard and `exec_map_user_stack` returns -1 — `exec_setup` then fails and the caller disposes the partial Proc, the correct outcome for a binary mapping over its own stack guard. Closes corvus-bringup-d audit F7.

### Initial process stack — argc / argv / envp / auxv (P6-pouch-kernel-auxv)

After mapping the segments + the user stack, `exec_setup` calls `exec_build_init_stack` to write a **System V process-startup frame** into the top of the user stack. A C runtime (pouch — the Thylacine POSIX libc; `docs/POUCH-DESIGN.md`) reads `argc`, `argv`, `envp`, and the auxiliary vector from this frame at entry. `*sp_out` points at the frame's `argc` word.

**ONE SHAPE since #140** (LINEAGE L-6c). There used to be two — a fixed 176-byte "no argv" Shape A and a variable argv-bearing Shape B — and each wrote its own lone NULL for `envp`, which is why no Thylacine process had ever had a POSIX environment: the defect had two homes. The general arithmetic (`EXEC_INIT_STRUCTURED` / `EXEC_INIT_FRAME_SIZE`, both in `exec.h`) reduces to the old fixed frame *exactly* when `argc` and `envc` are both zero, and `exec.h` pins that with a `_Static_assert` rather than asserting it in prose. So the table below is still the frame an argument-less program with an empty environment gets, byte for byte.

General layout, low → high address (`R` = `round_up(structured, 16)`, the `AT_RANDOM` offset):

| Offset from sp | Bytes | Contents |
|---|---|---|
| 0                            | 8            | `argc` |
| 8 + 8*i                      | 8            | `argv[i]`, i = 0..argc-1 — user VAs into the strings region |
| 8 + 8*argc                   | 8            | `argv[]` terminator (NULL) |
| 16 + 8*argc + 8*j            | 8            | `envp[j]`, j = 0..envc-1 |
| 16 + 8*argc + 8*envc         | 8            | `envp[]` terminator (NULL) |
| 24 + 8*argc + 8*envc         | 144          | `auxv[]` — up to nine `Elf64_auxv_t` (16 B each) |
| ...                          | 0–15         | alignment padding (zero, from `KP_ZERO`) |
| R                            | 16           | `AT_RANDOM` entropy block |
| R + 16                       | argv_data_len| argv strings — concatenated, NUL-terminated |
| R + 16 + argv_data_len       | env_data_len | envp strings — same packing, `NAME=VALUE\0` records |
| frame_size                   | —            | `EXEC_USER_STACK_TOP` |

At `argc == 0, envc == 0` that is: `argc` at 0, the two NULLs at 8 and 16, auxv at 24, pad at 168, `AT_RANDOM` at 176, `EXEC_USER_STACK_TOP` at 192 — `EXEC_INIT_STACK_SIZE`.

**Where the environment comes from.** The builder takes env DATA, not a Proc, and that is forced rather than stylistic: `exec_load_into` builds into a *detached* address space and commits only after everything failable has succeeded (LINEAGE L-2a), so at that moment the Proc still holds the OLD environment. `exec_stage_env` does the projection for the callers whose answer is "whatever this Proc already has" — every `SYS_SPAWN_*` thunk (projecting the child's already-cloned `/env`) and the native `SYS_EXECVE`, whose ABI has no envp argument and therefore means *preserve*. A phenotyped Linux `execve` packs its own block from the guest's `envp`. The block is bounded independently of the `/env` maxima (`EXEC_ENV_MAX` / `EXEC_ENV_DATA_MAX`, argued in `exec.h`) and an environment that exceeds it is refused with `T_E_2BIG`, never truncated.

The auxv entries (`a_type`, `a_val`), written by `exec_fill_auxv` (both frame shapes route through it):

| a_type | a_val |
|---|---|
| `AT_PHDR` (3)    | user VA of the ELF program-header table, or 0. **Load-bearing for dynamic loading**: a directly-exec'd stock ldso decides it was invoked as a program by testing this against its own self-relocated `base + e_phoff` (`musl ldso/dynlink.c:1834`), so the biased value D-2 produces is what makes that branch correct — see the D-2 note below |
| `AT_PHENT` (4)   | `e_phentsize` (56 — `sizeof(Elf64_Phdr)`) |
| `AT_PHNUM` (5)   | `e_phnum`, or 0 when `AT_PHDR` is unresolved |
| `AT_PAGESZ` (6)  | `PAGE_SIZE` (4096) |
| `AT_HWCAP` (16)  | the Linux-compatible arm64 CPU-feature word (`g_hw_features.linux_hwcap` — FP/ASIMD/AES/PMULL/SHA1/SHA2/SHA512/SHA3/CRC32/ATOMICS/ASIMDDP at the Linux uapi bit numbers, derived from ID_AA64ISAR0/PFR0 at boot; `hwcap_CPUID` is never set — see `12-hardening.md`) |
| `AT_RANDOM` (25) | user VA of the 16-byte entropy block (`sp + 176` with no argv and an empty env) |
| `AT_ENTRY` (9)   | the loaded image's FINAL entry — `img->entry`, which already carries the PIE bias for an ET_DYN and equals `e_entry` for an ET_EXEC (DISTRO D-2). Unconditional |
| `AT_VDSO_CLOCK` (0x5654) | user VA of the RO clock page — OPTIONAL, present only when the vDSO page mapped (see `11-timer.md`); when absent the AT_NULL terminator moves up and the slot stays zeroed padding |
| `AT_NULL` (0)    | 0 — vector terminator |

**What AT_ENTRY is and is not for (DISTRO D-2).** It is the standard SysV tag — `getauxval(AT_ENTRY)` answers it, and the v1.x in-kernel dual-image `PT_INTERP` lift would need it to name the PROGRAM's entry while `AT_PHDR` named the program's phdrs. It is **not** what makes stock ldso work, despite what the D-2 design text said before this was measured. musl discriminates direct invocation on `AT_PHDR` (above); inside that branch it *writes* `aux[AT_ENTRY]` itself from the app it just mapped (`dynlink.c:1914`) and only reads it at the final `CRTJMP` (`:2075`). Confirmed by sabotage, not just by reading: with AT_ENTRY forced to 0 on the PIE path the stock-ldso boot gate still PASSES, and with AT_PHDR forced to 0 it FAILS — while the unit suite stays 1363/1363 in both cases, so the gate is what caught it. See `docs/DISTRO.md` §5 and task #186.

The minimum a static musl process needs (per POUCH-DESIGN.md §12.1) plus the informational entries. `AT_HWCAP` is the STANDARD SysV tag — musl's `getauxval`, libsodium's armcrypto runtime gate, and the Go runtime's `internal/cpu` init read it directly; consumers treat a clear bit as feature-absent (fail-safe on crypto-less cores). Other optional entries (`AT_SECURE`, `AT_CLKTCK`, ...) remain deliberately omitted — a C runtime supplies its own defaults for absent entries. All known consumers scan the vector by tag to `AT_NULL`; nothing parses by fixed offset.

**The initial sp** = `EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE`. It is 16-byte aligned — the AArch64 SysV ABI requirement — because `EXEC_INIT_STACK_SIZE` is rounded up to a 16-byte multiple and `EXEC_USER_STACK_TOP` is itself aligned. The header pins this with `_Static_assert(EXEC_INIT_STACK_SIZE % 16 == 0)`.

**AT_PHDR resolution.** The program headers live at file offset `img.phoff` (exposed by `elf_load` — see `docs/reference/21-elf.md`). `exec_build_init_stack` scans the loaded segments for the one whose file range `[file_offset, file_offset + filesz)` covers the entire phdr table, and translates: `AT_PHDR = seg.vaddr + (phoff - seg.file_offset)`. A well-formed static binary's first PT_LOAD spans the ELF header + phdrs, so this resolves to a valid, mapped, readable user VA. If no loaded segment covers the table, `AT_PHDR` and `AT_PHNUM` are reported 0 — a C runtime then skips the phdr walk, which is correct for a program with no `PT_TLS`.

**AT_RANDOM.** 16 bytes of kernel-CSPRNG entropy (`kern_random_bytes`), which a C runtime uses to seed its stack-protector canary + pointer-mangling cookie. The kernel-side scratch buffer is zero-initialised before the CSPRNG call, so a short read can never ship kernel-stack residue into userspace. The 8-byte pad slot is never written — it stays zero from the BURROW's `KP_ZERO` allocation. The frame therefore carries no uninitialised bytes and no kernel addresses.

The frame is written into the stack BURROW's backing pages through the kernel direct map — the BURROW is located via `vma_lookup(p, EXEC_USER_STACK_BASE)` after `exec_map_user_stack` has installed it. This is the same mechanism `exec_map_segment` uses for segment bytes. The frame is data (read by EL0 as data, never executed), so no I-cache maintenance is needed.

**Freestanding binaries are unaffected.** Thylacine-native binaries built against `libt` / `libthyla-rs` have a `_start` that calls `main` directly and never reads the stack frame; the 144-byte frame simply sits above their initial sp, ignored. The frame is consumed only by a SysV-aware C runtime (pouch). Every existing binary (joey, corvus, the bringup probes) boots unchanged with the new sp.

### I-cache coherence spans the whole executable segment (#107)

Both eager paths — `exec_map_segment` (blob) and `map_eager_from_file` — route their I-cache maintenance through `exec_make_exec_coherent(kva, size)`, where **`size` is the whole page-rounded segment span, not `filesz`**, and the call is **not** gated on `filesz > 0`.

The rule is easy to get wrong in the other direction, because the bytes past `filesz` really are zero: `alloc_pages(KP_ZERO)` zeroed them. But that zeroing is a *data-side* write. It does not evict I-cache lines that a prior occupant of those recycled physical pages may have left behind — nothing in `mm/` performs any I-cache maintenance on free or alloc — and the tail is mapped executable along with the rest of the segment. A branch into it would therefore fetch stale instructions rather than trapping on the zeros. Zeroed memory and an I-cache with no stale lines for those PAs are different properties.

This is the rule the REVENANT FILE fault arm already applied: `arch/arm64/fault.c` syncs `PAGE_SIZE` per page-in, not the valid byte count, and its comment states the same hazard ("EL0 could fetch a stale line from a prior occupant of this recycled PA -> wrong-instruction execution"). The two eager paths were the ones that had drifted from it.

Reachability: no binary this tree's toolchain emits has the shape — a scan of all 794 ELFs in `build/` + `usr/` found 794 `PF_X` PT_LOADs, every one with `memsz == filesz`, because `ld` places `.bss` in a separate RW PT_LOAD. A normal `PF_R|PF_X` segment therefore satisfies `round_up(filesz) == round_up(memsz)` and takes the *file-backed* dispatch arm, where per-page `PAGE_SIZE` syncs already covered it. The eager arm receives exactly the `memsz > filesz` case — which `elf_load` accepts, since it rejects only `filesz > memsz` — so a crafted ELF reaches it, and any Proc that can write and exec a file can craft one. Narrow reachability is not a disposition; the comment claiming the tail was safe was wrong regardless.

The `filesz == 0` ungating matters for the same reason: a `PF_X` PT_LOAD with `filesz == 0` and non-zero `memsz` is pure bss, loads fine, and under the old `if (seg->filesz > 0)` nesting would have been mapped executable with **no** maintenance at all.

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

`kernel/test/test_exec.c` — twelve tests (the registry in `kernel/test/test.c`
is the authority; this list drifted to nine while #45 added two and #107-audit
F1/F2 added one):

- `exec.setup_smoke`: minimal valid ELF; verify single segment VMA at vaddr + user stack VMA + entry/sp out params.
- `exec.setup_segment_data_copied`: ELF with 256 bytes of recognizable data; verify bytes are copied into BURROW backing pages (read via direct map); tail of page is zero.
- `exec.setup_constraints`: NULL inputs / NULL out params / kproc-rejected (covered indirectly by p->vmas check) / corrupt ELF magic / unaligned segment vaddr — all return -1.
- `exec.setup_multi_segment`: text RX + rodata R + data RW; verify all three VMA prot bits + user stack.
- `exec.setup_lifecycle_round_trip`: 2-segment exec + proc_free → `phys_free_pages` returns to baseline (all VMOs + sub-tables freed).
- `exec.user_stack_guard`: verify the user-stack guard VMA — present at `[GUARD_BASE, STACK_BASE)`, `prot==0`, `burrow==NULL`, distinct from the stack VMA — and that a VMA overlapping the guard is rejected by `vma_insert` (the reservation property). Closes corvus-bringup-d audit F7.
- `exec.setup_auxv` (P6-pouch-kernel-auxv): ELF whose first PT_LOAD covers the program headers; reads the System V startup frame back from the stack BURROW and verifies the argc/argv/envp NULLs, all six auxv entries (types + values), a resolved `AT_PHDR`, `sp == EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE` (16-aligned), and the `AT_RANDOM` block in-range + CSPRNG-populated (non-zero).
- `exec.setup_auxv_no_phdr_segment` (P6-pouch-kernel-auxv): ELF whose loaded segments do not cover the phdr table; verifies the unresolved-fallback path — `AT_PHDR == 0` / `AT_PHNUM == 0` — with the rest of the frame well-formed.
- `exec.setup_bss_tail_icache_synced` (#107): a `PF_R|PF_X` segment with `filesz == 0x40` and `memsz == 0x2000`; asserts the I-cache maintenance was issued at the segment's kernel VA over the **whole two-page span**, not the 0x40 copied bytes. Emulated targets model a coherent I-cache, so the stale fetch itself is unobservable in-guest — the assertion is that the *work was done*, the same posture as the W1.5 patcher's `g_alt_applied == g_alt_total`. Revert-probed: narrowing the span back to `seg->filesz` takes the suite to 1232/1233 FAIL on exactly this assertion.

Each test synthesizes an ELF in a static aligned buffer (`g_elf_blob`); the same idiom as `test_elf.c::build_elf`.

## Error paths

- `exec_setup` returns -1 on any failure. Caller (currently kernel test code) disposes of the Proc via `proc_free` with `state=ZOMBIE`. The Proc's `vma_drain` correctly releases whatever VMAs were installed before the failure, restoring BURROW refcounts.
- `burrow_create_anon` OOM during a segment map: that segment's BURROW never installed; prior segments remain.
- `burrow_map` overlap (e.g., two PT_LOAD segments with overlapping vaddr ranges): the overlap is rejected at `vma_insert`; `exec_map_segment` calls `burrow_unref` (rolling back the implicit `burrow_acquire_mapping` taken inside `vma_alloc`) and returns -1.

## Performance characteristics

- ELF parse: ~tens of microseconds for typical (≤ 16) PT_LOAD segments.
- Per-segment cost: one `burrow_create_anon` (one `alloc_pages(order)`) + one byte-copy of `filesz` bytes + one `burrow_map`. For a 4 KiB segment with 4 KiB filesz: roughly 10 µs (allocation) + 4 µs (memcpy) + 5 µs (burrow_map's vma_alloc + vma_insert). Larger segments scale linearly with filesz for the memcpy.
- User stack: one `burrow_create_anon_lazy(1 MiB)` + one `burrow_map` (L-4a: the alloc+zero of 256 pages is gone; one page is populated for the init frame), plus a `vma_alloc_guard` + `vma_insert` for the guard VMA (no allocation — negligible). Roughly 30 µs.

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

4. ~~**BURROW_TYPE_ANON only**. v1.0 anonymous VMOs eagerly allocate backing pages.~~ **Superseded twice.** REVENANT R-4 gave non-writable segments `BURROW_TYPE_FILE` (demand-paged from the Image cache, no per-exec memcpy). LINEAGE L-4a gave the remaining private backing `BURROW_TYPE_ANON_LAZY` — see "Sparse private backing (L-4a)" below. `BURROW_TYPE_ANON` survives on the exec path for **executable** segments only.

5. **No copy-on-write (COW) for shared text**. Two execs of the same binary each allocate fresh VMOs + copy bytes. Phase 5+ COW lets multiple Procs share read-only segment VMOs.

6. **User stack is `EXEC_USER_STACK_SIZE` (1 MiB, not the 256 KiB this said until L-4a) at a fixed VA**, with a one-page guard VMA directly below it (P5-secondary-stack-guard). Since L-4a it is RESERVED at that size but backed sparsely, so "growable stack via demand-page-on-fault" is BUILT (task #49) — the growth is downward into the sparse Burrow's unfaulted slots, and the guard VMA still bounds it. Per-Proc stack VA randomization (ASLR for stack) remains outstanding.

7. **Caller is responsible for partial-state cleanup**. On `exec_setup` failure (non-zero return), the Proc is in a partial state with some VMAs installed and some not. Caller (test code at v1.0; future exec syscall handler) calls `proc_free` with `state=ZOMBIE` to clean up.

## Sparse private backing (LINEAGE L-4a)

Exec's private anonymous backing is **sparse**: the Burrow reserves every page the
segment's `memsz` covers, but only the pages carrying real file bytes are allocated
at exec. Everything past `filesz` is `.bss` and demand-zeroes on first touch, through
the `BURROW_TYPE_ANON_LAZY` fault arm the overcommit model already had.

This was not a memory-footprint chore. It is the substrate **copy-on-write needs**: a
COW break replaces ONE page, and eager anon owns one indivisible buddy block, so
there is nothing per-page for a share count to index. See `docs/LINEAGE.md` section
2.9.

### What converted, and the gate

| site | what it loads | after L-4a |
|---|---|---|
| `exec_map_segment` | the blob path — **loads joey**, so a live boot path | sparse iff not executable |
| `map_eager_from_file` | writable data + degenerate bss tails (REVENANT's private arm) | sparse iff not executable |
| `exec_map_user_stack` | the 1 MiB user stack | always sparse (RW, never executable) |

`seg_may_be_sparse()` admits a segment iff **`(flags & PF_X) == 0`**. The predicate is
"not executable", not "writable", and the reason is a safety one: **the demand-zero
fault arm performs no I-cache maintenance.** An executable `.bss` tail arriving through
it would map executable with a prior occupant's cache lines live — the #107 hazard,
which the eager paths close by syncing the span and REVENANT's file-backed arm closes
per page. Rather than teach a third arm to sync, every executable page stays on a path
that already does.

W^X (I-12) makes `PF_W` imply `!PF_X`, so all writable data is covered either way; the
extra reach is the rare read-only segment with a bss tail, which is free to make sparse
and which no fork will ever break.

### The two new Burrow primitives

```c
int   burrow_lazy_populate(struct AddrSpace *as, bool exempt,
                           struct Burrow *v, size_t first, size_t n);
void *burrow_lazy_slot_kva(struct Burrow *v, size_t slot);
```

`burrow_lazy_populate` is the demand-zero fault arm's body hoisted to exec time and run
over a range. It is **all-or-nothing**: it charges the whole run before allocating (so a
run straddling `PROC_PAGE_MAX` is refused whole rather than half-populated), and on any
shortfall it frees every page it installed and returns the entire charge, leaving the
Burrow exactly as found. It takes `as->lock` (the stated precondition of
`addrspace_charge_pages`) and `v->lock`, and enters the buddy allocator under **neither**
— burrow.c's leaf-lock discipline.

`burrow_lazy_slot_kva` hands back a raw page pointer, which is sound only under its
stated precondition: **the Burrow is private to the caller** — created, populated, and
filled before it is mapped or reachable from a second thread. That is exec's situation
and nothing else's.

### The init frame moved to a scratch buffer

A sparse stack has no contiguous kva to lay the argv/envp/auxv frame out in, so
`exec_build_init_stack` builds it in a transient `kzalloc` and copies it in per-page at
the end. Every field write in the layout body is byte-identical; only `frame` changed
from "a pointer into the Burrow" to "a pointer into scratch". The scratch is bounded by
`EXEC_INIT_STACK_MAX_SIZE` (~68 KiB worst case; 176 bytes in the argv-less shape) against
the 1 MiB the eager stack cost unconditionally.

The consequence is that `exec_build_init_stack` is now **failable** — it returns `0` as
the sentinel (a real sp always sits just below `EXEC_USER_STACK_TOP`), and both callers
turn that into `-1`.

Its populate run is the page holding the frame's FIRST byte through the top of the
stack. The frame is 16-aligned but not page-aligned, so its first byte generally lands
mid-page; everything below that page stays sparse and demand-zeroes as the program
descends.

### Measured

| binary | RW `PT_LOAD` FileSiz | MemSiz | eager cost | after L-4a |
|---|---|---|---|---|
| corvus | 128 B | 24 MiB | `alloc_pages(13)` = **32 MiB** allocated AND zeroed | 1 page |
| joey | 8 B | 345 KiB | order-7 = 512 KiB | 1 page |
| ut, net-echo | 0 B | 97 B | order-0 | 0 pages |

Every writable `PT_LOAD` in the tree carries at most 128 bytes of file data; the segment
is essentially all `.bss`, and `map_eager_from_file` sized the allocation by `memsz`
(task #130). The stack was 256 pages allocated and zeroed at every exec regardless of
depth (task #49).

### I-32: exec-image pages joined the page axis

`burrow_map_in` charges only the VMA axis, so eager exec pages were **uncharged** — the
I-32 row calls the exec image "one-shot bounded". The lazy path charges per page, and
L-4a's pre-populate charges the run it makes resident.

So `page_count` now tracks true RSS across exec, which is what ARCH section 6.5 already
claimed it did. The cost is that **stack growth can now fail** where before it could
not — gracefully, the way the overcommit model already fails: `proc_fault_terminate`,
per Proc, never a box extinction. A stack is 256 pages against `PROC_PAGE_MAX` = 65536
(~0.4%), and the TCB is exempt.

### Tests

`exec.writable_segment_is_sparse` (4 MiB `memsz` behind 64 bytes of `filesz`: 1024 slots
reserved, exactly 1 resident, bytes preserved, tail zero, charge == 2) and
`exec.stack_is_sparse` (1 MiB reserved, 1 resident, and the text segment still eager).

Both are revert-probed, and **independently**: forcing every segment eager fails only the
first; over-populating the stack fails only the second. The two mechanisms — the segment
gate and the frame-run computation — are separately load-bearing.

## Naming rationale

`exec_setup` (not `exec` proper) — emphasizes that this is the load-and-map step, NOT the transition-to-EL0 step. The full exec syscall (Phase 5+) is `exec()` + the asm trampoline; `exec_setup` is the address-space-population half.

## DISTRO D-4 — the PT_INTERP rewrite to the interpreter

`elf_load` has always reported `PT_INTERP` as `ELF_LOAD_HAS_INTERP`, and until D-4 that
was only ever a refusal. D-4 upgrades it from diagnosis to **dispatch** for a
`PHENO_LINUX` image, and leaves it a refusal for every native one. The kernel still loads
exactly ONE image per exec, before and after — what changes is WHICH image.

### Where it lives, and why that placement is load-bearing

Inside `exec_load_into` (`kernel/exec.c`), immediately after the first `elf_load`. Two
consequences fall out of that choice rather than being arranged:

- **ONE mechanism, both entries.** The in-container `execve` (`viv_execve` ->
  `sys_execve_core`) and the runner's ENTRY spawn (`sys_spawn_full_argv_thunk` ->
  `exec_setup_from_spoor`) both funnel here, so there is no second copy of the decision.
- **`/proc/<pid>/exe` stays faithful.** The Proc-side stamps (`proc_set_name`,
  `proc_set_exe_path`) run in the two CALLERS, which still hold the ORIGINAL `exe`. The
  2026-08-05 vote listed "`/proc/self/exe` reports ldso" as an accepted gap; it does not,
  and the reason is purely that the rewrite is below the stamps.

`exec_load_into` gained `struct Proc *nsp` for this. It is read-only in the strict sense
— no field of it is written — and supplies exactly two things: the phenotype that gates
the rewrite, and the namespace the interpreter resolves through. `NULL` disables the
rewrite entirely and restores the pre-D-4 behaviour byte for byte.

### The steps

1. `elf_read_interp` (`kernel/elf.c`) extracts the path from the already-read header
   prefix — bounded, NUL-checked, `ELF_INTERP_MAX` = 255. Anything unreadable,
   unterminated, empty, or over-long is reported ABSENT, never truncated: a truncated
   `/lib/ld-musl-aarch64.so.1` is `/lib/ld`, which names a different file.
2. `exec_resolve_from_namespace(nsp, ...)` — the SAME `OEXEC`-gated helper the program
   itself came through, so "what is executable from this namespace" has one answer and
   not two. The interp path is container-relative and crosses its own symlink, which is
   why D-1 precedes D-4.
3. `exec_interp_argv` builds the rebuilt vector (below).
4. `exec_read_header` + `elf_load` again, now on the interpreter.

**One level, structurally.** The block is straight-line, not a loop, so the second
`elf_load` sees the interpreter's phdrs and an interpreter carrying its own `PT_INTERP`
falls through to the unchanged refusal.

### The argv shape

```
[interp_path, "--argv0", orig_argv0, "--", orig_path, orig argv[1..]]
```

`argc + 4` slots. musl's direct mode uses ONE slot for both "which file to load" and
"what argv[0] becomes" (`dynlink.c:1901` then `:1913`), so passing the path alone would
hand the program the name it RESOLVED from rather than the name its caller INVOKED it by
— and `argv[0]` is a dispatch input for both programs this arc runs (busybox picks its
applet from `basename(argv[0])`; a login shell is identified by a leading `-`).
`--argv0` separates them; musl applies it at `dynlink.c:2071`. `--` is unconditional so a
program path beginning with `--` is not eaten by the option parser. musl consumes exactly
the four inserted slots and rewrites argc in `argv[-1]` (`dynlink.c:1891`), so the
program observes its original `argc` and vector.

`orig_path` comes from a NEW `prog_name` parameter threaded from the callers, **not** from
`exe->path`. That is I-33: the Spoor's `Path` is cosmetic and no syscall result may turn
on it, so an exec that failed because a path-alloc OOM'd would be the invariant's own
counterexample. The argv spawn carries the name inline in `struct spawn_full_argv_args`
(the resolution that consumed it happened in the parent); the two native-only spawn
thunks pass `NULL`, which is the honest answer — a `PHENO_LINUX` Proc cannot reach a
native syscall number at all, so neither can produce a dynamic image.

### Disposal

The rewrite allocates two things — the interpreter's pinned Spoor and the rebuilt argv
block — and `exec_load_into` was split into a thin wrapper plus `exec_load_body` so both
have exactly ONE disposal site. The body reports what it took through out-params, so all
dozen of its early returns are covered by construction. This is the D-3c F1/F5 lesson
applied ahead of time: a cleanup that was right at three sites and missing at the fourth.

### What this does NOT do

No auxv change. Direct mode is selected by `aux[AT_PHDR] == ldso.phdr`
(`dynlink.c:1834`), which is automatically true once the ldso IS the loaded image — our
`AT_PHDR` (`exec.c:655`) and musl's `laddr(&ldso, e_phoff)` are the same value for a
segment-0-at-file-offset-0 PIE. `AT_BASE` and `AT_ENTRY` are for the in-kernel dual-image
model the 2026-08-05 vote REJECTED.

### Known gaps (the DISTRO.md section 3.2 ledger)

| Surface | Behaviour | Why |
|---|---|---|
| `argc == 0` | becomes `argc == 1`, `argv[0] == ""` | the loader's command line must name a pathname |
| mode `0111` (X, not R) | refused at load | the ldso re-opens the program `O_RDONLY`; the kernel's `OEXEC` gate still runs first, so this only SUBTRACTS reachability |
| the program path | resolved twice (kernel peek, then ldso `open`) | benign — the kernel's resolution only decides "this needs an interpreter"; a file swapped between them is caught by `map_library`, never by the kernel mapping wrong bytes |
| non-musl interpreters | `--argv0`/`--` are a musl CLI dependency | glibc distros are already a recorded seam; failure is LOUD (usage text, exit 1) |

### Tests, and what each can and cannot see

`exec.interp_argv_shape` asserts the block BYTE FOR BYTE. Not optional coverage:
`exec_build_init_stack` EXTINCTS when the NUL count disagrees with argc, so an off-by-one
in the slot arithmetic is a dead kernel rather than a failed exec. It is also the ONLY
place the `--argv0` claim is discriminable — in a container a caller's `argv[0]` always
equals the path it resolved.

`elf.read_interp` covers the shared bounded walk from the side that ACTS on its answer
(`elf_brand_hint` is now a caller of it), including the absent-not-truncated cases.

In-guest: `D4-A-byname-getconf-4096` and `D4-B-argv0-is-the-program`, boot-fatal in the
L-6c leg list.

**Three-way discrimination, measured 2026-08-10 — the two layers are complementary, and
neither alone is sufficient:**

| Sabotage | Gate | Unit suite |
|---|---|---|
| control (none) | PASS (D4-A + D4-B) | 1389/1389 |
| S1 — rewrite disabled | REDDENS at exactly `D4-A`; `D3-A` stays green | **fully blind** (1389/1389 through it) |
| S2 — `argv[0] := path` (the vote's literal shape) | **fully green through it** (GATE PASS) | reddens on exactly its own leg |

S1 is why the gate exists: it names by-NAME execution, which no unit test reaches. S2 is
why the unit test exists: the claim it carries has no in-guest producer.
