# 126 — REVENANT: file-backed demand-paged exec + the Image cache [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-revenant-doc-absorb`).
The REVENANT arc — a binary roused page by page on fault (the Plan 9 `Image` model
as `BURROW_TYPE_FILE`), governed by the seven I-36 soundness conditions. An
audit-trigger surface (ARCH §28 I-36); every condition is enforced in code and
carried, more currently, by the owning dossiers (all fresh):

- **R-1, the substrate** — `burrow_create_file` (a byte range of a file behind a
  pinned Spoor, sparse `filepages[]` per-page, the I-30 pin held for the Burrow's
  life, `burrow_free_deferred`):

      vault/system/kernel/memory/sub-kernel-burrow.md

- **R-2 / R-5 / R-6, the demand-page fault arm** — the file-backed miss that
  *sleeps* (`file_fault_req`, the deliberate lock-break protocol), death-
  interruptible by inheritance (#811), and the fail-closed `FAULT_USER_BUS` /
  `snare:bus` that reports a backing-read failure as the *filesystem's* fault, never
  a silent zero-fill of executable text:

      vault/system/kernel/memory/sub-kernel-fault.md

- **R-3, the Image cache** — `image_lookup_or_create`, the seven-field key whose
  **qid version** makes close-to-open coherence free (an atomically-replaced binary
  is a new key), the backing-size stamp (#194, condition 7), and the
  eviction-cannot-race-a-mapper proof:

      vault/system/kernel/execution/sub-kernel-image.md

- **R-4, file-backed exec** — `exec_setup_from_spoor` (reads only the ELF
  header+phdrs, maps text file-backed and data eager-anon) and
  `exec_resolve_from_namespace` (the `stalk(OEXEC)` resolve + pin):

      vault/system/kernel/execution/sub-kernel-exec.md

- **R-4, the I-cache contract** — `arch_icache_sync_range` (#317: clean-to-PoU +
  invalidate-to-PoU inner-shareable, before any freshly-written R+X PTE):

      vault/system/kernel/memory/sub-kernel-mmu.md

- **condition 3, the W^X layers** — `elf_load` rejects `PF_W|PF_X` at parse, and
  the private eager-copy of writable data (condition 4) never writes back to the
  snapshot:

      vault/system/kernel/execution/sub-kernel-elf.md
      vault/system/kernel/memory/sub-kernel-addrspace.md

Conditions 1–2 come free from Stratum's content-addressed Merkle FS (the FS server
is the integrity authority; the kernel installs what `dev->read` returns); the
genuinely-new bits are 5–6, and the hard primitive (death-interruptible sleep)
pre-existed as #811.

**What this file got WRONG or MISSED by the time it was absorbed** — little; it is
an as-built I-36 reference and current (it carries #45, #149, and the #194/D-3c
past-EOF bound). The change is distribution: the seven-condition table and the
R-1..R-5 machinery now live across the six dossiers above, each at more depth —
notably the Image cache's eviction-safety proof and the fault arm's lock-break
protocol, which are the load-bearing arguments this surface turns on. The
per-page I-32 charge for *shared* text remains a v1.x refinement (charging a shared
page per-Proc is unsound), as the doc and `sub-kernel-fault` both record.
