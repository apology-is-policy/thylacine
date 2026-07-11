# REVENANT.md — file-backed demand-paged exec + the static-linking conviction

> **REVENANT** *(confirmed thematic name; user signoff 2026-06-19)* — a revenant
> returns from death. A binary rests in the immutable Stratum archive (a
> content-addressed fossil) and is **roused page by page, on fault**, into a
> running Proc — never slurped whole. The name carries the project's de-extinction
> motif (cf. Lazarus, Halls of Extinction). The Plan 9-derived mechanism keeps its
> heritage name: the **Image** (the qid-keyed shared-text cache).

Binding scripture for the exec-load arc. Design-first: this document + the ARCH /
ROADMAP / VISION edits in the same commit land BEFORE any code, per CLAUDE.md
"Design conversation -> scripture commit." User-signed-off 2026-06-19 (three
votes, below). Motivated by **#229** (the v1.0 toolchain depends on a post-v1.0
mechanism — a scheduling contradiction this arc resolves). Arc task **#230**.

---

## 1. The problem

Thylacine spawns a binary by **slurping the whole ELF** into a single contiguous
`kmalloc` blob bounded by `SYS_SPAWN_BLOB_MAX = 1 MiB`
(`kernel/syscall.c::exec_load_from_namespace`), then `exec_setup` **eagerly**
allocates an anonymous Burrow per `PT_LOAD` segment and copies it in. Two eager
copies, one of them a 1-MiB-capped *contiguous* allocation.

Consequences:

- **A binary larger than 1 MiB cannot be spawned.** The net-7c TLS feasibility
  spike hit this: a native rustls client is ~1.0 MiB (TLS-1.3-only) and the full
  https tool would exceed it.
- **The v1.0 on-system toolchain (#67 / RW-13 D3: clang/lld/git) is multi-MB**
  and cannot load at all. The enabling mechanism — file-backed demand-paged exec
  (`BURROW_TYPE_FILE` + a page cache) — was reserved in ARCH but marked
  *post-v1.0*. A v1.0 deliverable depending on a post-v1.0 mechanism is the #229
  contradiction.
- Even under the cap, the slurp is wasteful: it reads (and the spawn waits for)
  100% of a binary that may execute 10% of its code, and it shares **nothing**
  across processes running the same binary.

The fix is not a bigger cap (a constant bump just relocates the
buddy-fragmentation wall and is a band-aid — see §8). The fix is to stop
slurping: **demand-page the binary from the file, page by page, on fault** —
which is exactly what the Plan 9 heritage does, and what Thylacine deviated from.

---

## 2. Decisions (user-signed-off 2026-06-19)

- **D1+D2 — Approve both.** Land (a) the file-backed demand-paged exec
  architecture (the Plan 9 Image model realized as `BURROW_TYPE_FILE`), and (b)
  the **keystone correction** narrowing ARCH §6.5's network-coherence claim:
  userspace *writable* file `mmap` stays permanently refused; *kernel-internal,
  read-only, integrity-verified, immutable-snapshot* file-backed exec is sound
  and becomes the sanctioned path.
- **D3 — Permanent conviction.** Native-userspace **dynamic linking is refused
  permanently** (static-only is promoted from a v1.0 default to a conviction).
  The sole reopen is the Linux-binary-compat tier (a Fuchsia-style
  capability-sandboxed loader). The content-keyed RAM-dedup "substitute" is
  declined — it is KSM, and KSM defeats ASLR (§7).
- **D4 — Eager-copy data into anon at v1.0.** Text is demand-paged + shared
  across processes; writable `.data`/`.bss` segments are eager-copied into a
  private anonymous Burrow per segment at exec (simplest). Anon-COW for data is
  a clean v1.x optimization the demand-page handler already accommodates.

---

## 3. Prior art (the research, condensed)

Five research streams (Plan 9; Fuchsia + Genode; Mach/Hurd external pagers;
dynamic linking; the Thylacine tree map). Full briefs + citations in
`memory/project_exec_load_arc.md`. The load-bearing findings:

### 3.1 Plan 9 (the heritage — and the design)

Plan 9 `exec` reads **only the a.out header**, then `attachimage(SG_TEXT|SG_RONLY,
chan, ...)` backs the text segment **by the executable's Chan** and demand-pages
it via `fixfault` -> `pio()` -> `devtab read`. Nothing is slurped. A global
**Image cache**, keyed on the executable's **qid** (`c->qid.path` + the full
qid + mount identity + type), means every process running the same binary shares
**one** Image — one set of physical text pages, read once on the first faults
and reused by every subsequent exec. A second layer (the kernel page cache,
keyed `(type, dev, qid)`) dedups across opens. Image pages are reclaimable under
memory pressure.

Coherence is **qid.version checked at open, never per-page**; the Image is pinned
to that version for the process's life. The convention — **binaries are
immutable; you atomically *replace*, never rewrite in place** (a new file -> a
new qid) — is exactly Thylacine's FS-gamma rename-swap + Stratum's qid.version.

**Thylacine's slurp is the deviation from the heritage. The Plan 9 Image model
*is* the proper design.**

### 3.2 Fuchsia + Genode (the capability-uK SOTA)

Both prove two theses Thylacine already embodies: **(1) the kernel needs zero
knowledge of ELF or linking** — only a memory-object (VMO / ROM dataspace ~
Burrow), a map-with-protection (`zx_vmar_map` / `Region_map::attach` ~ VMA), and
thread-start; the dynamic linker is ordinary userspace. **(2) what-binary/
library-a-process-can-load is a capability, not a path** — Fuchsia via a
per-process loader-service object; Genode via parent-mediated ROM sessions.
**Thylacine's equivalent is the per-Proc 9P namespace** — the binary/library is
what the namespace can *name*; the namespace is the confinement knob (Genode's
idiom, no separate loader-service object needed). W^X: Genode's "the mapper sets
the executable bit per segment; the child can't self-promote" is exactly
Thylacine's kernel-refuses-W&X-keyed-off-`p_flags`. Fuchsia's executable-as-a-
right (`vmex`) is the designated future mechanism *if* JIT ever enters scope —
for a static, no-mmap v1.0 the kernel simply never grants a post-load executable
mapping, which is strictly stronger.

### 3.3 Mach/Hurd (the network-coherence crux)

ARCH §6.5 justified refusing file-backed mapping with *"there is no coherent way
to demand-page a file served over the network."* Mach/Hurd's external-pager
model **refutes that as an absolute**: the FS server *is* the pager (`libpager`;
`ext2fs`, and **`nfs` demand-pages remote files today**); a fault becomes
`memory_object_data_request` to the pager over IPC. The defensible *residue* is
three narrow conditions, not "no way":

1. Fully-coherent **writable multi-writer** DSM over a network is genuinely
   expensive (Mach's XMM was "too expensive," superseded). Refuse *that*.
2. A network-backed **mapping's failure surface is worse** than read/write — a
   dead/slow pager yields `SIGBUS`-on-a-load or an **un-cancellable wedged
   fault**, not a checkable `-EIO`.
3. A **raw file-mapping is a trust hazard** — the real Hurd "[VULN] No read-only
   mappings" bug: the single shared pager cache became a cross-client corruption
   channel until rights-reduced via memory-object *proxies*.

**Stratum satisfies all three for free.** A binary identified by content hash
from an **immutable snapshot** is the easy case the whole DSM-coherence
literature wished for: no torn writes (immutable -> condition 2's class cannot
occur), no multi-writer coherence (no writers -> condition 1's expensive regime
never applies), integrity-verified pages (Merkle -> condition 3's hostile-server
injection is closed). seL4 (the contrast) drops paging entirely for
verifiability and yields static images, not a general-purpose OS — Thylacine's
mission requires exec-from-FS, so it must *engineer the sound conditions*, which
Stratum hands it.

### 3.4 Dynamic linking (refuse)

The disk argument for dynamic linking is **dead on Thylacine**: Stratum is
content-addressed, so byte-identical static library code (e.g. rustls embedded in
many tools) is **already stored once** on disk. The only surviving argument —
cross-binary RAM page sharing — is small (a small no_std native set; same-binary
copies already share via the Image; there is no executable page-cache sharing
today to regress). And the one mechanism that would capture it without a linker —
a content-keyed page cache — **is not novel: it is KSM** (Kernel Same-page
Merging), which defeats ASLR via a write-timing side channel (CVE-2015-2877,
CVE-2024-0564) and is the *Dedup Est Machina* read/write primitive Microsoft
disabled memory dedup over. For an OS whose differentiator is the full hardening
stack *including ASLR*, that is a self-inflicted wound. Dynamic linking also
fights four standing decisions (refuse-file-mmap, 9P read/write coherence, W^X,
static-only) and adds `ld.so` as a per-process audit-bearing trusted component.

**Refuse it. Permanently. For native userspace.** (§7 is the full record.)

---

## 4. The architecture

### 4.1 `BURROW_TYPE_FILE` — the file-backed Burrow

The Burrow (VMO) gains its long-reserved fourth backing type. A
`BURROW_TYPE_FILE` Burrow is backed not by anonymous pages, by an MMIO range, or
by a pinned DMA chunk, but by **a range of a file** — specifically a
kernel-pinned `Spoor` (the executable's Chan) at a base offset. It holds:

- a `spoor_ref` on the backing Spoor (pinned at exec, **never re-resolved** at
  fault — the I-30 "pin at submit, never re-read at completion" discipline,
  applied to "pin the text Chan at exec, never re-walk the namespace at fault");
- the file base offset + length for the segment it backs;
- the qid (`{dc, devno, qid.path, qid.vers}`) sampled at exec, the cache key and
  the coherence token;
- the dual refcount (handle_count + mapping_count, the #847/I-7 lifecycle) — a
  shared text Burrow stays alive while any Proc maps it.

### 4.2 Exec: parse the header, map segments file-backed

`exec_load_from_namespace` stops slurping. The new shape:

1. Resolve the program in the caller's namespace (`stalk(STALK_OPEN, OEXEC)`,
   unchanged — #58) and **pin the resulting Spoor**.
2. Read **only the ELF header + program headers** — a small, bounded read (a few
   KB). `SYS_SPAWN_BLOB_MAX` degrades to a sanity bound on the *header*, not the
   whole binary. The 1-MiB wall is gone.
3. For each `PT_LOAD` segment, create a VMA over a Burrow:
   - **Non-writable (`PF_W` clear — `R+X` text AND `R`-only rodata; the #45
     extension, §4.6):** a `BURROW_TYPE_FILE` Burrow over the pinned Spoor at
     the segment's file offset. Demand-paged; **shared read-only across Procs**
     via the Image cache (§4.4); the PTE prot is the segment's ELF prot (`R+X`
     for text, `R`-only [XN] for rodata). W^X-clean by construction (`elf_load`
     rejects `PF_W|PF_X`; nothing non-writable is ever writable). Gated by
     `round_up(filesz) == round_up(memsz)` (every mapped page has a file page
     behind it) — a whole-bss-page tail routes eager. Originally text-only;
     generalized to all non-writable segments at #45 (§4.6).
   - **Data (`R+W`):** at v1.0 (D4), eager-copy the segment's file bytes into a
     **private anonymous** Burrow at exec (per-segment, not the whole ELF), `+`
     zero-fill `.bss`. No file-backed writable mapping is ever exposed — this
     preserves the no-userspace-mmap conviction (§5). v1.x: anon-COW (share the
     file's clean pages read-only until first write, then fault a private copy).
4. Map the user stack + build the init frame (unchanged).

### 4.3 The demand-page fault arm

`userland_demand_page` already dispatches on `burrow_type` (ANON / MMIO / DMA)
under `vma_lock`. The new `BURROW_TYPE_FILE` arm, on a fault into a text VMA:

1. Compute the file offset (`burrow base + in-VMA offset`).
2. **Consult the Image cache** (§4.4) for the page at `(qid, offset)`. On a hit,
   install the PTE pointing at the shared physical page — no read.
3. On a miss, **`dev->read` one page** from the pinned Spoor at the offset (the
   same 9P/devramfs read path the slurp used today; a devramfs `/bin` binary is a
   memcpy, a Stratum-backed binary is one `Tread`). Install it in the Image
   cache, then install the PTE `R+X`.
4. The read **must be #811-death-interruptible** (§4.5).

This mirrors the existing DMA fault arm structurally. No new W^X surface: the PTE
is `R+X` for text, the 4-layer W^X enforcement is unchanged, and a file-backed
VMA inherits the `vma_alloc` W&X reject.

### 4.4 The Image cache (Plan 9 Image, qid-keyed)

A kernel-global, refcounted cache of read-only text pages, keyed on the
executable's **file identity** `(dc, devno, qid.path, qid.vers)` + page offset.
Two Procs running the same binary share one set of physical text pages. Pages are
**reclaimable** under memory pressure (tail-chained, evicted by a reclaim pass),
so a large resident toolchain text doesn't permanently pin RAM. Coherence is the
qid.version sampled at exec: a binary atomically replaced (FS-gamma rename-swap ->
a new qid.version) is a new cache entry; the running process is pinned to the
version it exec'd. This is the **safe**, file-identity-keyed sharing — it is
**not** KSM (it does not content-scan across unknown content; an attacker learns
nothing they didn't already know, namely that Proc X runs binary Y). The
content-keyed cross-binary dedup that *is* KSM is declined (§7).

### 4.5 Death-interruptible, fail-closed faults (the genuinely-new requirement)

The single condition stock Mach lacked: a page fault that does a `dev->read` over
9P **must be cancellable**. A dead or wedged FS server must turn the in-flight
fault into a **per-Proc termination** (`proc_fault_terminate`, a `snare:*`-class
fault), never an indefinitely-wedged thread and never a box extinction. Thylacine
already built the primitive — the universal death-interruptible sleep (#811 /
I-9 generalized, the per-Thread `wait_lock` register-then-observe + the
`el0_return_die_check`). The page-in path registers on the per-Thread `wait_lock`
exactly like every other blocking 9P op. A page-in I/O error (the
`memory_object_data_error` analog) maps to a `snare:bus`-class per-Proc
terminate, *attributable* (which page, which Spoor) — **never** a silent
zero-fill of executable text, **never** a whole-system stall. The hostile-ecode
bounding (I-14) extends to the page-in path.

### 4.6 R-only (rodata) segments ride the file-backed path (#45, the CHASE addendum — 2026-07-11)

The original dispatch file-backed only `PF_X` text; the R-only rodata segment
was lumped into the eager-copy class with writable data. The CHASE D44
decomposition measured what that costs: a modern Go binary carries roughly
HALF its bytes in the R-only segment (compile: 8.62 MiB R+X / **7.89 MiB R** /
0.99 MiB RW filesz; the `go` command: 5.41 / **5.92** / 0.41), so every exec
eagerly `dev->read` the R-only megabytes into a PRIVATE anon copy — ~89% of
the per-exec eager bytes, ~792 MiB re-read across one cold `go build`'s 91
compile execs, and a private per-live-Proc RAM copy under parallel builds.

An R-only segment is semantically text-minus-the-X-bit: immutable, never
written, safely shared. The dispatch predicate generalizes from `PF_X` to
`PF_R && !PF_W` (the `PF_R` requirement — #45 audit F2 — keeps a no-access
`flags==0` PT_LOAD on the eager path; it can never be faulted, so file-backing
it would only pin an idle Image slot):

    file_shareable = (flags & PF_R) && !(flags & PF_W) &&
                     round_up(filesz) == round_up(memsz)

Text behavior is byte-identical (`elf_load` rejects `PF_W|PF_X`, so
`PF_X ⇒ !PF_W`). Writable segments keep the D4 eager-copy-into-anon path.

Soundness — why this is an extension, not new mechanism:

- **All seven I-36 conditions apply verbatim** — none is text-specific.
  Condition 4 governs WRITABLE data, which stays eager-copied anon; condition
  3's W^X: rodata maps `R`-only (no W, no X) — trivially clean.
- **The fault arm is already prot-general**: `file_install_locked` /
  `file_install_cluster_locked` install at `vma->prot`; the #317 I-cache sync
  is gated on `freq->exec` — a rodata page is never synced, which is sound
  because each FILE Burrow backs exactly ONE segment at ONE **exec-class**, so
  no page ever migrates from a non-exec to an exec mapping. **This is ENFORCED,
  not assumed (#45 audit F1)**: the Image key is `(dc, devno, qid.path,
  qid.vers, file_offset, size, exec)` — the `exec` component splits a crafted
  ELF's aliased R+X + R-only PT_LOADs (identical file window, different X-ness)
  into DISTINCT Burrows, so no single FILE Burrow is ever mapped at both an
  executable and a non-executable prot. Without it, a rodata-first fill (no
  sync) then a text resident-hit would execute stale I-cache lines — the #317
  hazard, reachable by a crafted binary; the exec key closes it. A legit binary
  is unaffected (the same file's same segment always carries the same X bit ->
  the same key -> maximal cross-Proc sharing).
- **The Image cache already keys per-segment** `(dc, devno, qid.path,
  qid.vers, file_offset, size)`; a binary now occupies two entries (text +
  rodata). `IMAGE_CACHE_MAX` doubles 64 → 128 to keep the effective binary
  capacity (~4 KiB more BSS; the LRU-idle eviction + the full-of-live bypass
  degrade are unchanged).
- **The last-page tail** past `filesz` is the same file padding the text path
  already exposes (page-aligned segment starts — enforced by the dispatch —
  make cross-segment page overlap impossible; an EOF tail is KP_ZERO via the
  audited short-read loop discipline).
- **torpor R-5 F1 composes**: the pre-fault before `torpor_lock` is
  address-based (it faults in whatever backs the word), so a futex word in a
  FILE rodata page cannot newly sleep under the lock. A rodata futex word can
  never be WRITTEN by userspace, so a matched wait sleeps until an explicit
  wake — userspace's own bug, same as Linux; no kernel hazard.
- **I-32 posture unchanged**: file pages keep the v1.0 charge model (the
  per-page charge is the recorded v1.x seam); rodata widens the shared
  uncharged set exactly as text does.

What this buys (the D44 measurement driving the change): the first exec per
boot demand-pages only the touched rodata subset (64-page read-ahead
clusters); every later exec Image-hits — zero reads, zero copies (previously
an ~8.9 MiB eager copy per compile exec even when Larder-served). Live Procs
share one resident rodata copy (a RAM win under parallel builds).

---

## 5. The keystone: correcting ARCH §6.5

The standing prose ("File-backed `mmap` is refused — deliberately, permanently…
there is no coherent way to demand-page a file served over the network") is
**overstated** (Mach/Hurd refutes the absolute; §3.3). The corrected scripture:

- **Userspace writable file `mmap(fd)` stays permanently refused** — the Plan 9
  conviction holds, for the *correct* reasons: multi-writer coherence over 9P is
  expensive; a writable network-backed mapping's failure surface (`SIGBUS` /
  un-cancellable wedge) is worse than read/write's checkable `-EIO`; and a raw
  file-mapping is a capability trust hazard (the Hurd CVE). pouch keeps surfacing
  `mmap(fd)` as `ENOSYS`.
- **Kernel-internal, read-only, integrity-verified, immutable-snapshot
  file-backed exec is sound and sanctioned** — it is the benign animal the
  refusal never actually covered. It is how the kernel loads binaries; it never
  exposes a file-backed *mapping* to userspace; data segments terminate in
  anonymous memory.

The distinction is **userspace-writable-mapping (refused) vs.
kernel-internal-read-only-immutable-exec (sound)** — not "files are read, never
mapped" applied indiscriminately.

---

## 6. The new invariant (file-backed-exec soundness)

The 7 necessary conditions from the Mach/Hurd analysis (§3.3), jointly
sufficient, become a §28 invariant. Kernel-internal file-backed exec is sound iff:

1. **Immutable backing identity** — map a content-addressed snapshot (a pinned
   qid.version), never a mutable path. *(Stratum, free.)*
2. **Integrity-verified pages** — every demand-paged page is Merkle-checked
   before install. *(Stratum, free; closes the Hurd-CVE class.)*
3. **Read-only, rights-reduced, capability-mediated mapping** — never a raw
   pager/server handle; the mapping is a Burrow/Spoor with monotonically-reduced
   rights (I-2/I-5/I-6); W^X holds (I-12). *(Existing.)*
4. **Private COW (or eager-copy) for writable data** — `.data` terminates in
   anonymous memory, never written back to the snapshot; shared text is never
   copied. *(The designed COW seam / D4 eager-copy.)*
5. **Death-interruptible page faults** — a wedged/dead server -> per-Proc
   terminate, never a wedged thread, never a box extinction. *(#811 / I-9, the
   primitive already exists.)*
6. **Bounded, fail-closed fault errors** — a page-in error -> `snare:bus`-class
   per-Proc terminate, attributable; never a silent zero-fill of text. *(I-14
   extends.)*
7. **Resource-accounted pages** — file-backed pages charged against the per-Proc
   floor (I-32) like anon; shared text charged once (the I-7 dual-refcount).

Conditions 1-2 are free from Stratum; 3-4-7 are existing mechanisms; 5-6 are the
genuinely-new bits, and the hard primitive (5) already exists. The conditions
are segment-class-generic: an R-only (rodata) segment satisfies them
identically — the #45 extension (§4.6) routes it through the same path with
the X bit dropped. The invariant
number is assigned in the ARCH §28 edit (the Imperium arc reserves I-35; this
takes the next free slot).

---

## 7. The static-linking conviction (dynamic linking refused, permanently)

A standing conviction, recorded so it is not relitigated each time a large
library appears:

> **Native Thylacine userspace is statically linked. Dynamic linking is refused.**
> The disk argument is dissolved by Stratum content-addressing (identical static
> code is stored once); the only surviving argument (cross-binary RAM sharing) is
> small, and the sole mechanism that would capture it without a linker is KSM, an
> ASLR-defeating dedup side channel (CVE-2015-2877; *Dedup Est Machina*) that a
> hardening-first OS must not adopt. Dynamic linking additionally fights the
> refuse-file-mmap, 9P-read/write-coherence, and W^X positions and adds `ld.so`
> as a per-process trusted attack surface (`LD_PRELOAD`, GOT-overwrite,
> loader-gadgets). This is Plan 9's conclusion, strengthened by Thylacine's
> content-addressed FS and hardening-first stance.

**The sole reopen** is the Linux-binary-compat tier: foreign glibc-dynamic ELFs
are best-effort, and *if* that tier is built, dynamic linking is confined to it
as a **capability-sandboxed loader** (Fuchsia-style: libraries supplied as
capabilities, never ambient `LD_*` path search; full-RELRO + BIND_NOW + PIE) —
inside the explicitly-degraded compat blast radius, never the model for native
programs. PIE/ASLR + `.so` support stay post-v1.0.

**The trigger symptoms are solved without a linker:**

- **Toolchain size (clang/lld)** is a *distribution* problem, not a linking
  problem (the bulk is the toolchain's own code; dynamic linking barely dents
  it): Stratum dedups the shared blocks, and the toolchain ships as a
  separately-installable / lazy-fetched dataset rather than baked into the base
  image.
- **rustls RAM duplication** (if it ever measures as a real problem) is a TLS
  *service* — a single native `tlsd` Proc that owns rustls, reached over
  `/srv` (the Plan 9 "shared functionality is a server, not a shared library"
  idiom; fits the netd/corvus pattern). Candidate, not built pre-need.

### 7.1 The content-addressed-dedup insight (the real NOVEL, honestly bounded)

The genuinely Thylacine-distinctive truth: **content-addressed storage captures
dynamic linking's *disk* benefit with zero linker complexity** — and that is
already true (Stratum). The proposed extension to a content-keyed *RAM* page
cache is **not** novel and **not** sound (it is KSM; §7). The honest framing:
content-addressing is a full substitute for dynamic linking's disk win and a
non-substitute for its RAM win — and that is fine, because the RAM win is small
and the only way to claim it costs more security (ASLR) than it saves.

---

## 8. Why not just raise `SYS_SPAWN_BLOB_MAX`

Recorded so the band-aid isn't reached for later. The blob is a single
*contiguous* `kmalloc` per spawn (`alloc_pages(order)`; 1 MiB = order-8, 2 MiB =
order-9). The buddy can serve it (MAX_ORDER=18 = 1 GiB), but a high-order
contiguous allocation gets **fragmentation-dependent** as the system runs — a
2 MiB spawn can intermittently fail at steady state (exactly when a user runs a
tool), a new load-dependent failure class. It is also a *global* cap (widens what
any binary can be + the transient-memory DoS surface, uncharged by I-32), the
slurp cost scales, and — decisively — it **doesn't fix the structural limiter, it
relocates it**: the next big binary hits the new wall. Demand-paged exec removes
the contiguous slurp entirely (every fault is an order-0 page). The cap becomes a
header-sanity bound.

---

## 9. Sequencing + scope

- **Sequencing:** the implementation arc inserts **after the net arc**
  (net-7c-2 / net-7d / net-8) and **before the Imperium/Authority arc**
  (user-directed). The scripture (this commit) lands now; net continues
  unaffected.
- **v1.0 scope:** demand-page text (shared via the Image cache) + eager-copy data
  into anon (D4). This is what the #67 toolchain forces into v1.0/Phase 8, and it
  resolves #229.
- **v1.x / post-v1.0 seams:** anon-COW for data; PIE + userspace ASLR; the
  reclaim-pass tuning; the `tlsd` TLS service (if measured); the Linux-compat-tier
  sandboxed loader (the dynamic-linking reopen, if that tier is built).

---

## 10. Implementation plan (after this scripture commit; no code yet)

Sub-chunks, each landed + tested independently; one focused audit over the three
audit-trigger surfaces (exec / W^X / demand-page):

- **R-1:** `BURROW_TYPE_FILE` + `burrow_create_file(Spoor)` + the dual-refcount
  lifecycle (clunk the Spoor on last unref) + tests.
- **R-2:** the `userland_demand_page` `BURROW_TYPE_FILE` arm — `dev->read` one
  page, install `R+X`, **death-interruptible** (#811), fail-closed
  (`snare:bus`) + tests.
- **R-3:** the Image cache (qid-keyed, refcounted, reclaimable) + cross-Proc
  text sharing + tests.
- **R-4:** `exec_load_from_namespace` / `exec_setup` rework — parse the header,
  map text file-backed + data eager-anon; retire the slurp; the cap becomes a
  header bound. Boot proof: spawn a >1 MiB binary.
- **R-5:** the focused audit (the 7-condition invariant; the Hurd-CVE class; the
  death-interruptible fault; the W^X surface) + the SMP gate + close.
- **Docs:** ARCH (§6.5 + the new invariant + §25.4 audit row), a reference doc,
  ROADMAP, this doc kept current; #229 resolved.

The realization is *small in kernel terms* — a new fault arm + a constructor + a
cache — because the VMA already carries `burrow + offset`, the fault handler
already dispatches on type, the dual-refcount + W^X layers already exist, and the
death-interruptible primitive already exists. The convictions hold, correctly
scoped.

---

## 11. Naming rationale

**REVENANT** *(confirmed; user signoff 2026-06-19)* — the arc that rouses a binary
from the immutable archive page by page, on fault. Fits the de-extinction motif
(Lazarus, Halls of Extinction, Menagerie). The Plan 9-derived shared-text cache
keeps its heritage name, **Image** (like bind / mount / 9P — Plan 9 concepts are
not renamed). `BURROW_TYPE_FILE` keeps the reserved enum name.
