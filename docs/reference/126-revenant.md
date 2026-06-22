# 126 — REVENANT: file-backed demand-paged exec + the Image cache

> As-built reference for the REVENANT arc (file-backed demand-paged exec, the
> Plan 9 `Image` model realized as `BURROW_TYPE_FILE`). Design: `docs/REVENANT.md`
> (= `docs/EXEC-LOAD-DESIGN.md`). Invariant: ARCH §28 **I-36** (the seven
> file-backed-exec soundness conditions). Keystone: ARCH §6.5 (userspace writable
> file `mmap` permanently refused; kernel-internal read-only immutable-snapshot
> exec sound + sanctioned).

## Purpose

A binary is **roused page by page, on fault**, into a running Proc — never
slurped whole. REVENANT retires the pre-arc exec model (read the entire ELF into
a single contiguous `kmalloc` blob bounded by `SYS_SPAWN_BLOB_MAX = 1 MiB`, then
eager-copy each `PT_LOAD` segment) and replaces it with the Plan 9 `Image` model:

- **Text** (`R+X`, read-only) is mapped **file-backed** over the executable's
  kernel-pinned `Spoor`, **demand-paged** one page per fault, and **shared
  read-only across Procs** running the same binary via a qid-keyed `Image` cache.
- **Data** (`R+W`) is **eager-copied** per-segment into a **private anonymous**
  Burrow at exec (D4 v1.0 scope; anon-COW is the v1.x optimization). No
  userspace-visible file-backed *writable* mapping is ever created.

The `SYS_SPAWN_BLOB_MAX` 1-MiB whole-binary cap is gone — only the ELF
header+phdrs are read eagerly (bounded by `EXEC_ELF_HEADER_MAX = 16 KiB`). **A
binary of any size execs** (the boot proof execs an unstripped ~1.11-MiB
`net-echo`). This resolves #229 — the on-system toolchain (#67: clang/lld/git,
multi-MB) can now load.

It sits between the ELF loader (`kernel/elf.c`, parse + validate), the VMA +
Burrow machinery (`kernel/vma.c`, `kernel/burrow.c`), and the page-fault
dispatcher (`arch/arm64/fault.c`). The kernel stays ELF/linker-ignorant beyond
parse; dynamic linking is refused permanently for native userspace (REVENANT §7;
the content-keyed RAM-dedup "substitute" is KSM, declined as ASLR-defeating).

## The seven I-36 conditions and where each is enforced

| # | Condition | Enforcement site |
|---|---|---|
| 1 | Immutable backing identity (a pinned `qid.vers`, never a mutable path) | `burrow_create_file` samples `(dc, devno, qid.path, qid.vers)` at exec; the Image key includes `qid.vers` so an atomically-replaced binary (FS-gamma rename-swap → new `qid.vers`) is a *new* cache entry; a running Proc is pinned to the version it exec'd. |
| 2 | Integrity-verified pages (Merkle, before install) | Stratum-side (the dev9p `read` path delivers Merkle-verified bytes; the v1.0 devramfs `/bin` path is a trusted in-kernel memcpy). The kernel installs whatever `dev->read` returns; the FS server is the integrity authority. |
| 3 | Read-only, rights-reduced, capability-mediated mapping; W^X | `map_text_file_backed` maps `R+X` (`vma_prot_for_elf`); `elf_load` rejects `PF_W\|PF_X` (W^X layer 3); `vma_alloc` rejects W+X (layer 2); the PTE constructor enforces it (layer 1). No raw pager/server handle is exposed — only a Burrow over a kernel-pinned Spoor. |
| 4 | Private COW (eager-copy v1.0) for writable data | `map_eager_from_file` copies `filesz` bytes into a **private anonymous** Burrow per segment; the `[filesz, size)` tail is KP_ZERO (`.bss`). Never written back to the snapshot; shared text is never copied. |
| 5 | Death-interruptible page faults | The page-in `dev->read` blocks; it is `#811`-death-interruptible **by inheritance** (the dev9p read registers on the per-Thread `wait_lock`; a dying Proc unwinds at `el0_return_die_check` and the read returns `< 0`). No new wait/wake code. |
| 6 | Bounded, fail-closed fault errors | A page-in I/O error (`got < 0`) → `FAULT_USER_BUS` → `proc_fault_terminate(NOTE_NAME_SNARE_BUS, vaddr)` — attributable per-Proc terminate, **never** a silent zero-fill of executable text, **never** a box extinction. |
| 7 | Resource-accounted pages | Bounded **structurally** at v1.0 (one-shot exec, like the #65 exec-image posture; shared text bounded by `IMAGE_CACHE_MAX`). Per-page I-32 charge is a v1.x refinement (charging a *shared* text page per-Proc is unsound — see Known caveats). |

Conditions 1–2 are free from Stratum's content-addressed Merkle FS; 3–4–7 are
existing mechanisms; 5–6 are the genuinely-new bits, and the hard primitive (5)
already existed (#811).

## Public API

```c
// kernel/include/thylacine/burrow.h — the substrate (R-1)
struct Burrow *burrow_create_file(struct Spoor *spoor, u64 file_offset, size_t length);
//   ADOPTS one ref on `spoor` (no spoor_ref bump); the store is the last step, so
//   every error path returns NULL having taken NO ref (caller retains + clunks).
//   Allocates NO backing pages — only the sparse `filepages[]` array (page_count
//   slots, all NULL). burrow_free_internal frees every resident slot + the array,
//   then spoor_clunks the adopted Spoor (the I-30 pin held for the Burrow's life).

// kernel/include/thylacine/image.h — the Image cache (R-3)
void           image_cache_init(void);   // call after burrow_init
struct Burrow *image_lookup_or_create(struct Spoor *spoor, u64 file_offset, size_t length);
//   The exec text-segment entry point. ALWAYS consumes `spoor` on a non-NULL
//   return (adopted into a fresh FILE Burrow on a miss; spoor_clunk'd as redundant
//   on a hit). Returns a Burrow with ONE handle ref the caller owns — use it like
//   burrow_create_anon's return (burrow_map, then burrow_unref). NULL return
//   (length==0 / overflow / OOM) takes NO ref + does NOT consume `spoor`.

// kernel/include/thylacine/exec.h — the file-backed exec path (R-4)
#define EXEC_ELF_HEADER_MAX 16384u
#define EXEC_FILE_MAX       (256ull * 1024 * 1024)   // header-sanity bound, not a slurp
int exec_setup_from_spoor(struct Proc *p, struct Spoor *exe, size_t exe_size,
                          const char *argv_data, u32 argv_data_len, u32 argc,
                          u64 *entry_out, u64 *sp_out);
//   The production exec body. BORROWS `exe` (the caller/thunk clunks it). Reads
//   ONLY the ELF header+phdrs; maps text file-backed + data eager-anon.

// kernel/syscall.c — namespace resolution (R-4; was exec_load_from_namespace)
struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                          size_t name_len, size_t *size_out);
//   Resolve via stalk(STALK_OPEN, OEXEC) in the caller's Territory + stat (bounded
//   by EXEC_FILE_MAX) + PIN the Spoor; the ref is TRANSFERRED to the caller.

// arch/arm64/page.h — the I-cache contract (R-4 / #317)
void arch_icache_sync_range(const void *addr, size_t len);
//   dc cvau (clean to PoU) + ic ivau (invalidate I-cache to PoU, inner-shareable
//   broadcast) + dsb ish + isb, per CTR_EL0 line stride. Called before installing
//   any R+X PTE for a freshly-written text page.
```

## Implementation

### `BURROW_TYPE_FILE` (the substrate, R-1) — `kernel/burrow.c`

A fourth Burrow backing type alongside ANON / MMIO / DMA. A FILE Burrow holds the
adopted `Spoor` (the I-30 pin), the file base offset, the cache-key scalars
(`file_dc` / `file_devno` / `file_qid_path` / `file_qid_vers` sampled at create),
and the sparse `struct page **filepages` array (`page_count` slots, NULL until
faulted). It allocates **no** contiguous page chunk (`pages == NULL`); the per-page
physical pages fault in lazily.

- **`burrow_acquire_mapping` FILE liveness** is `spoor != NULL` (filepages MAY be
  all-NULL — the normal fresh-mapping state, not a UAF; the `{0,0}` resurrection
  guard already covers the freed case).
- **`burrow_free_internal` FILE arm** runs at `{handle_count==0 && mapping_count==0}`,
  so no VMA maps the Burrow and no concurrent fault touches `filepages` — the walk
  needs no lock. It frees every resident `filepages[i]` (order-0 each), then the
  array, then `spoor_clunk`s the adopted Spoor. Runs **outside** `v->lock` (leaf
  discipline; `spoor_clunk` → `dev->close` may sleep on a 9P Tclunk).
- The `#847`/I-7 dual-refcount lifecycle is shared with ANON/MMIO/DMA: a shared
  text Burrow stays alive while any Proc maps it OR any handle is open.

### The demand-page fault arm (R-2) — `arch/arm64/fault.c`

`userland_demand_page` dispatches on `burrow_type` under `p->vma_lock`. The
structural crux: a FILE page-in does a **blocking** `dev->read` (a 9P round-trip),
which cannot run under the `vma_lock` spinlock. So the path splits:

1. **Fast path** (`demand_page_locked`, under `vma_lock`): a resident `filepages`
   hit installs the `R+X` PTE immediately. On a **miss**, it `burrow_ref`s the
   Burrow (the pin), fills a caller-zeroed `struct file_fault_req` (`burrow`,
   `spoor`, `file_offset`, `page_va`, `slot`, `exec`), and signals `freq->needed`.
   The pin is safe because the VMA holds a mapping ref → the Burrow is alive *now*.
2. **Slow path** (`file_demand_page_slow`, **no lock**): `alloc_pages(0, KP_ZERO)`,
   then **loop** `dev->read` to fill the page (one page is `<= PAGE_SIZE` from the
   pinned Spoor at `file_offset`); `n < 0` → `FAULT_USER_BUS`; `n == 0` → EOF (the
   tail stays KP_ZERO = the `.bss`/zero-pad fill). The loop matches `exec_read_header`
   and `map_eager_from_file` — `dev->read` may legitimately short-return for an
   interior page (a conforming 9P server's choice), so a single read is unsafe.
   After a successful fill, if `freq->exec`, `arch_icache_sync_range(kva, PAGE_SIZE)`
   **before** the PTE is installed.
3. **Install-once** (`file_install_locked`, under the **re-acquired** `vma_lock`):
   re-lookup the VMA and re-validate `vma->burrow == freq->burrow` (the pin kept
   the Burrow + its SLUB address alive across the sleep, so this comparison is
   ABA-safe; a torn-down/remapped VMA bails and frees the page). Then under
   `v->lock`, install-once: if a sibling faulter already filled the slot, use the
   resident page + free our freshly-read one; else store ours. Install the PTE.
4. The pin is dropped (`burrow_unref(freq->burrow)`) **outside** `vma_lock` (the
   last unref's `spoor_clunk` may sleep).

**Lock order**: `vma_lock → v->lock` (the established order; `burrow_ref`/`unref`
take `v->lock` internally). Condition 5 (death-interruptible) is inherited from the
dev9p read; condition 6 (`FAULT_USER_BUS` → `snare:bus`) is the fail-closed leg.

### The I-cache contract (R-4 / #317) — `arch/arm64/mmu.c`

ARMv8 I/D caches are not coherent for instruction fetch. A text page filled via the
data path (`dev->read` writes through the D-cache) needs `dc cvau` + `ic ivau`
before EL0 fetches it, or a stale line from a prior occupant of the recycled PA is
executed (intermittent wrong-instruction). `arch_icache_sync_range` does this per
CTR_EL0 line stride. **The race-loser path is correct**: whichever page ends up in
a slot was synced by *its* creator before the under-`v->lock` store; `ic ivau` is
inner-shareable-broadcast and the spinlock acquire orders it before the loser's PTE
install, so every CPU's I-cache for that PA is coherent. The legacy in-memory
`exec_map_segment` (test/blob path) routes through the same helper (it dropped a
prior hardcoded-64B loop).

### The Image cache (R-3) — `kernel/image.c`

A kernel-global fixed table (`IMAGE_CACHE_MAX = 64`) of entries, each holding **one**
handle_count ref (a STRONG ref → text persists past the last unmap, the Plan 9
temporal cache) on a FILE Burrow, keyed on `(dc, devno, qid.path, qid.vers,
file_offset, size)`. `image_lookup_or_create`:

- **HIT** (pass 1, under `g_image_lock`): `burrow_ref` the cached Burrow for the
  caller; `spoor_clunk` the redundant passed Spoor (outside the lock).
- **MISS**: create **outside** the lock (`burrow_create_file` may sleep; it ADOPTS
  the Spoor), then **pass 2** re-search (the create race). On a lost race, `burrow_unref`
  the surplus fresh Burrow (frees it + clunks its adopted Spoor). Else install: the
  cache keeps the construction ref, a second `burrow_ref` is the caller's.
- **BYPASS**: a table full of *live* images (none idle) returns the fresh Burrow
  un-registered (lives on its mapping). Fail-safe degrade, never an exec failure.
- **EVICTION**: an insert into a full table evicts the LRU **idle** victim
  (`handle_count==1` [cache-only] `&& mapping_count==0`).

**Eviction safety (the SMP proof)**: under `g_image_lock`, both counts of a
`handle_count==1` entry are STABLE — the *only* path that refs a cached Burrow is
`image_lookup_or_create` (which takes `g_image_lock` first), so no in-flight mapper
can exist (it would hold `handle_count >= 2`); `mapping_count` cannot rise (a map
needs a lookup-ref first) and only falls via an unmap that does not touch the cache.
So a detached victim is reachable by no other path, and the `burrow_unref` outside
the lock cannot race a free. Lock order `g_image_lock → v->lock`, never the reverse
(the FILE free arm never re-enters the cache; the entry is detached before the
freeing unref). One `spoor_clunk` per Spoor on every path.

### File-backed exec (R-4) — `kernel/exec.c`

`exec_setup_from_spoor`:

1. `exec_read_header` reads the ELF header+phdrs into a bounded `kmalloc`
   (`EXEC_ELF_HEADER_MAX`), looping `dev->read`. The **OOB-deref guard**:
   `phoff + phnum*sizeof(Phdr) <= got` (overflow-safe; `phoff > got` checked before
   the subtraction). This bounds `elf_load`'s deref footprint — `elf_load` derefs
   *only* the ehdr `[0,64)` and the phdr table `[phoff, phoff+phnum*56)`, never
   `e_shoff`/section headers and never segment bytes (it value-checks
   `p_offset+p_filesz <= size`). 16 KiB holds the ehdr + the max phdr table
   (`ELF_MAX_PHNUM=256 × 56 + 64 = 14400`).
2. `elf_load(hdr, exe_size, &img)` — the **real file size** (`exe_size`, up to
   `EXEC_FILE_MAX`) is the bound for segment-extent validation; the header-only
   buffer is safe because of the guard above. `struct elf_image` is all scalars
   (no pointer into the buffer), so `kfree(hdr)` before `exec_build_init_stack`
   reads `img` is safe.
3. Per `PT_LOAD`: require page-aligned `vaddr` + `file_offset`, `memsz != 0`, then
   dispatch on `text_shareable = (PF_X) && round_up(vaddr+filesz) == round_up(vaddr+memsz)`:
   - **text-shareable** → `map_text_file_backed` (Image cache → shared FILE Burrow,
     `burrow_map` `R+X`, `burrow_unref`). BORROWS `exe`; `spoor_ref`s a fresh ref
     for the consuming Image lookup (on NULL it didn't consume → drop the fresh ref).
   - else → `map_eager_from_file` (private anon, loop `dev->read` `filesz` bytes,
     KP_ZERO tail = `.bss`; a PF_X segment routed here — a whole bss page past the
     file — still gets `arch_icache_sync_range`). A short read (`got != filesz`) →
     fail, no partial map.
4. User stack + the System V startup frame (`exec_map_user_stack`,
   `exec_build_init_stack` — unchanged; reads `img` metadata, not the file;
   AT_PHDR resolves into the first mapped segment's VA).

The five `SYS_SPAWN_*` families (`sys_spawn_for_proc` / `_with_fds_for_proc` /
`_with_caps_for_proc` / `_full_with_perms_for_proc` /
`_full_argv_with_perms_for_proc`) carry the pinned `exe` Spoor across `rfork`
instead of a blob: each parent body clunks `exe` on a pre-spawn failure and
*transfers* ownership to the thunk on a successful spawn (the thunk `spoor_clunk`s
`exe` after `exec_setup_from_spoor`). Resolution runs in the **parent**'s Territory;
the deferred header+data reads run in the **child** (death-interruptible vs the
child's death). The blob path (`exec_setup` / `exec_setup_with_argv`) is **kept for
the kernel exec tests** (test/legacy; the v1.x unification + delete is a seam).

## Data structures

```c
// struct Burrow (kernel/include/thylacine/burrow.h) — the FILE fields
struct Spoor *spoor;          // adopted (the I-30 pin); clunk'd on last unref
u64           file_offset;    // base offset of this segment in the file
int           file_dc;        // cache-key scalars, sampled at create
u32           file_devno;
u64           file_qid_path;
u32           file_qid_vers;
struct page **filepages;      // sparse [page_count], NULL until faulted

// struct image_entry (kernel/image.c)
bool used; int dc; u32 devno; u64 qid_path; u32 qid_vers;
u64 file_offset; size_t size; struct Burrow *burrow; u64 lru;

// struct file_fault_req (arch/arm64/fault.c) — the FILE-miss page-in request
bool needed; struct Burrow *burrow; struct Spoor *spoor;
u64 file_offset; u64 page_va; size_t slot; bool exec;
```

## State machines

A FILE Burrow page slot: **NULL (not resident)** → [fault: alloc + read + sync] →
**resident `struct page *`** (installed once; a racing faulter's page is freed).
Slots are cleared only at `burrow_free_internal` (`{0,0}`); never evicted
individually at v1.0 (the per-page reclaim is the v1.x seam — see Known caveats).

An Image entry: **free** → [MISS install] → **cached (strong ref, idle or mapped)**
→ [LRU eviction when idle] → **free**. `qid.vers` keys coherence (a replaced binary
is a new entry, never a mutated one).

## Spec cross-reference

Spec-to-code is **suspended** for REVENANT (the 2026-05-23 broadening): no
`specs/*.tla` module; I-36 is validated by prose (REVENANT §6 + this doc) + the
focused audit + the runtime + SMP gate. The composed invariants (I-7 dual-refcount
`burrow.tla`; I-9/#811 death-wake `death_wake.tla`; I-12 W^X; I-10/I-11 fid/tag via
`9p_client.tla`) keep their existing specs as pre-commit gates on their own
subsystems.

## Tests

- `kernel/test/test_burrow.c` (`burrow.file_*`): FILE Burrow create / adopt-and-clunk
  / resident-page free (loop-amplified — order-0 pages route through the per-CPU
  magazine, so a single-cycle `phys_free_pages` delta is invisible).
- `kernel/test/test_demand_page.c` (`demand_page.file_*`): smoke / read-error →
  `snare:bus` / multi-page.
- `kernel/test/test_image.c` (`image.*`): dedup-share + counts / distinct-qid /
  `qid.vers`-bump coherence / distinct-offset / LRU-eviction-bound /
  bad-arg-retains-spoor / the detach-under-lock eviction lifetime.
- `kernel/test/test_exec_namespace.c` (`exec_ns.*`): resolve-returns-pinned-Spoor /
  miss / deny.
- **Boot proof**: `net-echo` ships **unstripped** (~1.11 MiB, over the old 1-MiB
  cap; `tools/build.sh` removed it from `tls_strip_bins`); its boot-gating probe
  execs it file-backed + runs the full TLS-over-`/net` + loopback E2E
  (`net-8c-2 PASS`, `joey: net-8 PROBE OK`).
- **SMP gate**: `tools/ci-smp-gate.sh` (default + UBSan × smp4/smp8, N≥10) — now
  exercises the live FILE fault path under load; 0 corruption.

## Error paths

| Return | Trigger | Caller action |
|---|---|---|
| `exec_resolve_from_namespace == NULL` | name not resolvable / not a file / stat > `EXEC_FILE_MAX` | spawn fails `-1` |
| `exec_setup_from_spoor != 0` | malformed ELF / OOB header / a segment map fails / OOM | the partial Proc is disposed by the caller |
| `FAULT_USER_BUS` (fault arm) | a page-in `dev->read` error | `proc_fault_terminate(SNARE_BUS, vaddr)` — per-Proc terminate, never zero-fill |
| `FAULT_UNHANDLED_USER` (fault arm) | no VMA / permission denied / OOM / impossible shape change | `proc_fault_terminate(SNARE_SEGV, vaddr)` |
| `image_lookup_or_create == NULL` | length==0 / overflow / OOM | no ref taken; `spoor` not consumed (caller clunks) |

## Performance characteristics

- Exec start no longer waits for the whole binary: only the header (one or a few
  `dev->read`s of ≤16 KiB) is read eagerly; text pages fault in on first touch, so
  a binary that executes 10% of its code reads ~10% of its text.
- Cross-Proc text sharing: N Procs running one binary share one set of physical
  text pages (the Image), read once on the first faults.
- The contiguous high-order `kmalloc` slurp (and its fragmentation-dependent
  failure class) is gone — every fault is an order-0 page.

## Status

LANDED (R-1 `@4f7dbde` substrate; R-2 `@c4f97de` fault arm; R-3 `@67aa9e2` Image
cache; R-4 `@12454d0` file-backed exec; R-5 the focused audit + SMP gate + this
doc). Resolves #229. The arc (#231) is complete.

## Known caveats / footguns

- **Eager-copy data, not anon-COW** (D4). `.data`/`.bss` is copied per-segment into
  a private anon Burrow at exec. Anon-COW (share the clean file pages read-only
  until first write) is the v1.x optimization the demand-page handler already
  accommodates.
- **Per-page I-32 charge deferred** (condition 7). v1.0 bounds exec pages
  structurally (one-shot exec + `IMAGE_CACHE_MAX`). Per-page charging is a v1.x
  refinement: charging a *shared* text page against one Proc's floor is unsound
  (whose floor?), and a half-wired counter risks imbalance.
- **The Image is a STRONG-ref cache** (one handle_count per entry); text persists
  past the last unmap, bounded by the `IMAGE_CACHE_MAX = 64` LRU cap. A
  memory-pressure-triggered reclaim pass is the v1.x seam (REVENANT §9). A table
  full of *live* images degrades to a per-exec BYPASS, never an exec failure.
- **The blob path persists** (`exec_setup` / `exec_setup_with_argv`) for the kernel
  exec tests; unifying every test onto a spoor-over-memory-Dev + deleting the blob
  path is a v1.x seam.
- **Partial-tail of file-backed text** relies on the v1.0 toolchain (lld) zero-padding
  the gap between page-aligned segments; a fault-arm tail-zero on a partial-page
  text segment (`filesz < memsz` within the last page) is a v1.x hardening. The
  dispatch gate (`round_up(filesz)==round_up(memsz)`) routes any segment with a
  whole bss page beyond the file to the eager path (which KP_ZEROs the tail).
- **A thread death-interrupted while faulting a FILE page** exits `snare:bus` rather
  than its group-exit reason (cosmetic; the Proc still dies correctly via the
  idempotent group cascade — `proc_group_terminate`'s set-once `group_exit_msg`).
- **`file_install_locked` trusts the post-read `freq->slot`** rather than recomputing
  it from the re-looked-up VMA. Safe at v1.0 because no path remaps a FILE Burrow at
  a non-zero offset (FILE Burrows are created only by exec, mapped once at
  `burrow_offset == 0`, never handed to userspace to remap). A future path that maps
  a FILE Burrow at a chosen offset would need the recompute (a v1.x hardening if it
  arises).
