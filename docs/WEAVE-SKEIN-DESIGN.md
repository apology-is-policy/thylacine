# WEAVE-SKEIN — the scatter-gathered weave

**Status: RATIFIED 2026-09-09 (operator signoff on all four ballot items);
IMPLEMENTED the same day against `903fc5a7`. All four decisions landed as
ratified. READ SECTION 1.0 FIRST — implementation MEASURED that this document's
diagnosis of the operator's failure was wrong (the operative bound was
tapestryd's 32 MiB I-34 allowance, not the buddy's order-14 rounding), and the
correction is recorded there rather than quietly edited away.**

Direction approved ("scatter-gather sounds good"), then the §9 ballot returned
**all four as recommended**: a new `SYS_DMA_SEGMENTS` with a fail-closed
`SYS_DMA_MAP`; `SKEIN_BLOCK` = 2 MiB; the name **skein**; and scope **weave
only**. §9 records each with its consequence.

---

## 1. The defect, measured

The operator could not boot a 2560x1664 display:

```
tapestryd: t_dma_create_weave(51118080) failed -1
halcyond: FAIL session connect/create Create
login: session compositor unavailable -- console shell fallback
```

`51118080` = 2560 x 1664 x 4 x 3 — a triple-buffered weave, **48.8 MiB**.

`kernel/dma_handle.c::dma_create_body` page-aligns the request, checks it
against `KOBJ_DMA_WEAVE_MAX_SIZE` (64 MiB — it **passes**), then calls
`order_for_pages(12480)`, which rounds **up to the next power of two**: order
14 = 16384 pages = **64 MiB, contiguous and naturally aligned**, taken as a
single buddy block.

So a 48.8 MiB request consumes the entire envelope — 15.2 MiB (24%) wasted —
and succeeds only if a whole free 64 MiB aligned block exists.

### 1.0 CORRECTION (2026-09-09, at implementation): the cause named below is
### NOT what the operator hit

**The paragraphs that follow were wrong about the operative bound, and the
correction is the most useful thing in this document.** They are kept, struck
through by this note rather than deleted, because the shape of the error is
worth more than a clean document.

The real cause is a SECOND bound this document never considered: tapestryd's
warden manifest granted `dma = "pool: 32 MiB"`, so the I-34 allowance refuses a
48.75 MiB weave in `sys_dma_create_weave_handler` — `allowance_permits` returns
false and the handler returns -1 **before `kobj_dma_create_weave` is ever
called**. The buddy allocator was never reached. The one-line fix is the
manifest raise to 64 MiB, matching `KOBJ_DMA_WEAVE_MAX_SIZE`.

**Measured, not argued.** With the allowance at 64 MiB and the skein DISABLED
(`SKEIN_BLOCK` forced to 64 MiB, reproducing exact pre-skein single-span
behaviour), 2560x1664 boots clean: `exit=0`, 0 weave-create failures,
`scanout direct 0 slot 0 (2560x1664)`. So the order-14 allocation SUCCEEDS on a
freshly-booted 2 GiB guest. It was never the failure.

**Why the wrong cause was so convincing, which is the generalizable part:
the two bounds sit at the SAME 32 MiB threshold.** The allowance cap is 32 MiB;
the order 13 -> 14 boundary is also 32 MiB. So the boot evidence — 2048x1280
(30 MiB) works, 2560x1664 (48.75 MiB) does not — fits BOTH stories exactly, and
no amount of re-reading that evidence could separate them. **TWO CAUSES, ONE
READING: ONLY A SECOND AXIS CAN HELP.** The second axis was changing the
allocator and watching the failure not move.

**And the elimination list below contains a false entry**, which is how the
allowance escaped: it says "tapestryd's allowance is BROAD so it passes." It is
not broad. The warden narrows it, and the boot log says so on every boot
(`warden: bind virtio-pci:18 ... dma=0x2000000`). **A NEGATIVE OVER A SET YOU
DID NOT ENUMERATE IS A GUESS** — the item was listed as checked without being
read.

**What survives, and why the skein still landed.** The order-14 rounding is
real (`order_for_pages` rounds up; the code says so), and it makes a 48.75 MiB
weave depend on a free 64 MiB *naturally-aligned* block. That dependence is
luck about when in a system's life the compositor starts — and tapestryd is
`restart = on-crash`, so a restart on a long-uptime fragmented system is
precisely the unlucky case. The skein removes the dependence and cuts the waste
from 15.25 MiB (24%) to 0.25 MiB (0.5%), which is measured and holds regardless.
It is a fragility fix, NOT the bug fix. Both landed together; only the
manifest line fixed the reported failure.

---

~~**It does not, and that is measured.**~~ (Superseded by 1.0.) `/ctl/memory`
in the failing guest:

```
total:    524288 pages   (2048 MiB)
free:     483591 pages   (1889 MiB)
reserved:  25879 pages
```

~~1889 MiB free and the order-14 allocation still fails~~ — the reading was
consistent with fragmentation but did not PROVE it, because the allocation was
never attempted. Ruled out by elimination, each checked against the code: the
envelope test (51122176 < 67108864, passes); the syscall's own guards
(`syscall.c:581` — rights, size, **`allowance_permits` — THIS IS THE ONE, and
the claim that tapestryd's allowance is BROAD is false**); the I-32 page budget
(`PROC_PAGE_MAX` = 65536 pages = 256 MiB, and the DMA path does not charge it);
a hidden order cap (`MAX_ORDER` = 18, `DIRECTMAP_USABLE_RAM_MAX` = 8 GiB —
neither binds a 2 GiB guest).

**The cliff is at ~2.79 Mpx** for the ORDER story, and at 32 MiB for the
allowance story — the same place, which is the whole problem. 2048x1280 boots
(verified: 0 weave failures, `scanout direct 0 slot 0`, console up); 2560x1664
did not.

### 1.1 The contiguity is SELF-IMPOSED

`usr/tapestryd/src/gpu.rs:2781`:

```rust
pub fn attach_backing(&mut self, resource_id: u32, pa: u64, len: u32) {
    write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    w32(req_va + 24, resource_id);
    w32(req_va + 28, 1);          // nr_entries  <-- HARDCODED
    w64(req_va + 32, pa);
    w32(req_va + 40, len);
}
```

virtio-gpu's `RESOURCE_ATTACH_BACKING` takes `nr_entries` followed by an
**array** of `virtio_gpu_mem_entry {addr: le64, length: le32, padding: le32}`.
The device never asked for one contiguous run. We demand it of the buddy
allocator because this call site writes `1`.

### 1.2 The same rounding is a KNOWN deferral elsewhere

`kernel/include/thylacine/burrow.h:28-31` already says it of anon burrows:

> Eager allocation via `alloc_pages(order, KP_ZERO)` where order =
> ceil_log2(page_count). ... **Wasted memory possible for non-power-of-two
> page_count (acceptable for tests; production-grade per-page allocation
> deferred).**

So this is not a surprise; it is a documented deferral that the weave's size
finally made load-bearing. This design pays it off for the weave and leaves a
reusable mechanism for anon (§8).

### 1.3 The stale claim this refutes

`memory/project_next_session_main.md` (run 46d) records "up to ~5K fits the
64 MiB envelope." True of the ENVELOPE CHECK, false of the ALLOCATION, because
it was reasoned from `aligned_size > max_size` without `order_for_pages` in
view. **A BOUND THE CHECK PASSES IS NOT A BOUND THE ALLOCATOR CAN SERVE.**

---

## 2. Prior art

### 2.1 The heritage: Plan 9

Plan 9 does **not** ask the page allocator for large contiguous spans. It
keeps `xalloc` / `xspanalloc` — a separate allocator over a boot-time arena,
distinct from the general page pool — precisely so device buffers do not
depend on page-pool fragmentation. Our bug is the mistake Plan 9's structure
was built to avoid: we asked the general allocator for a 64 MiB span.

### 2.2 The capability microkernels

- **seL4** — untypeds. Physical memory is split into capabilities at boot; a
  large contiguous frame exists because the initial split set one aside.
  Reservation is the model, decided at boot by construction.
- **Genode** — the *platform driver* owns a DMA-capable pool reserved at boot
  and hands slices to device drivers as a session resource. Contiguity is a
  granted resource, which maps almost directly onto our allowance (§7).
- **Fuchsia** — `zx_vmo_create_contiguous` requires a BTI (bus transaction
  initiator) handle: contiguity is deliberately privileged. The preferred path
  is the IOMMU, so contiguity is not needed at all.

### 2.3 Linux, the most-trodden

Three layers, in increasing sophistication:

1. **Boot reservation** (`reserved-memory` DT nodes, `memblock_reserve`) —
   deterministic; the memory is dead when unused.
2. **CMA** — reserve a region but let *movable* allocations borrow it,
   migrating them out when a contiguous request arrives. The standard modern
   answer for GPU/camera/codec buffers on ARM. Cost: page-migration machinery.
3. **IOMMU / scatter-gather / dma-buf** — where the device can scatter-gather
   or an IOMMU exists, contiguity is unnecessary and buffers are page lists.

### 2.4 What the survey says

Every system either **reserves** (2.1, 2.2 seL4/Genode, 2.3 layers 1-2) or
**makes contiguity unnecessary** (2.2 Fuchsia, 2.3 layer 3). We do neither —
we gamble on the general allocator, which is the one option nobody chose.

**virtio-gpu puts us in the second family for free.** The device accepts a
page list today. On real hardware the V3D has its own MMU, so the same holds
there; a reserved arena would only ever be the conservative fallback for a
device with neither scatter-gather nor an IOMMU.

---

## 3. The design

### 3.1 The skein

A **skein** is a weave's backing store as a *list of blocks* rather than one
span — a length of yarn gathered in a loose coil instead of wound into a
single ball: physically discontinuous, logically one thread.

*(Thematic name, RATIFIED 2026-09-09 (§9.3) — it sits inside the existing
Tapestry vocabulary of weave / weft / loom, and names the one thing that
vocabulary lacked: a physically-discontinuous, logically-single thread.)*

```c
struct dma_block {
    paddr_t      pa;      // block base, page-aligned
    struct page *pages;   // the alloc_pages chunk, for free_pages
    unsigned     order;   // buddy order, for free_pages
};

struct KObj_DMA {
    u64               magic;
    struct dma_block *blk;      // nblk entries; blk[0] for a plain object
    u32               nblk;     // 1 for plain DMA; N for a skein
    size_t            size;     // requested bytes (page-aligned)
    int               ref;
    bool              weave;
    bool              gpu_bo;
};
```

### 3.2 Plain DMA is UNCHANGED — deliberately

Only the **weave** subtype becomes a skein. `SYS_DMA_CREATE` (virtqueues,
descriptor tables, ring memory) keeps single-block semantics, because:

- those buffers are small (`KOBJ_DMA_MAX_SIZE` is 1 MiB, order <= 8) so
  contiguity is cheap and fragmentation is a non-issue;
- a virtqueue descriptor table **must** be contiguous — the device walks it by
  address, with no length list to consult.

This keeps virtio-net, virtio-blk and every ring allocation entirely out of the
blast radius. The change is confined to the one object class whose consumer
(virtio-gpu `ATTACH_BACKING`) accepts a list.

### 3.3 Block granularity: 2 MiB

`SKEIN_BLOCK = 2 MiB` (order 9, 512 pages), with N = ceil(size / SKEIN_BLOCK).

| | today | skein |
|---|---|---|
| 48.8 MiB request | one 64 MiB block | 25 x 2 MiB = 50 MiB |
| waste | 15.2 MiB (24%) | 1.25 MiB (2.5%) |
| largest contiguous run needed | 64 MiB | **2 MiB** |

Order 9 is abundant where order 14 is not — that is the whole fix. The tail
block is over-allocated to a full 2 MiB rather than rounded to its own order;
the extra complexity is not worth 1.5 MiB, but it is a trivially reversible
choice.

### 3.4 The transport already fits

`REQ_REGION_LEN` is `0x500` = 1280 bytes. Minus a 24-byte header and the
8-byte `{resource_id, nr_entries}`, that leaves **78 mem entries** — 156 MiB at
2 MiB granularity, comfortably above the 64 MiB envelope (32 entries).
**No transport or ring change is required.**

Scaling note for later ambition: a 5120x2880 triple-buffered weave is 176 MiB,
which exceeds BOTH the 64 MiB envelope and the 78-entry capacity. Going there
needs an envelope raise plus either 4 MiB blocks or a larger REQ region — out
of scope here, recorded so nobody re-derives it.

### 3.5 The ABI change, and the fail-closed rule

`SYS_DMA_MAP` currently **returns the PA** ("so the driver can embed it in
device-visible descriptors", `syscall.c:831`). A skein has no single PA.

**A skein's `SYS_DMA_MAP` must REFUSE to return a PA — not approximate one.**
Returning `blk[0].pa` would be silently wrong for any caller that assumes the
whole buffer follows it, and would corrupt rather than fail. The map still
succeeds (the VA mapping is the point); the PA return becomes a distinguished
"not representable" value for `nblk > 1`.

A new syscall carries the list:

```
SYS_DMA_SEGMENTS(handle, user_buf, max_entries) -> count (or -1)
    user_buf: array of { u64 pa; u64 len; }
```

Bounded copy-out, mirroring the native buffer guards (the standing
"a new copy-out arm must mirror its native twin's buffer guard" rule).
`count > max_entries` is refused rather than truncated, so a caller can never
attach a partial backing and believe it whole.

**This is a syscall interface change**, which CLAUDE.md's escalation list calls
out explicitly — hence signoff before code.

### 3.6 Mapping stays VA-contiguous

The compositor draws into a flat buffer, so the guest VA mapping must remain
one contiguous run regardless of physical scatter. This is already easy:
`burrow_map`'s PTE install is a per-page loop (`burrow.c:547`, `:676`, `:1258`
all iterate `page_count`), so page *i* resolves to block `i / BLOCK_PAGES`,
offset `i % BLOCK_PAGES`. No new mapping machinery.

### 3.7 tapestryd

`attach_backing` takes a slice instead of a pair:

```rust
pub fn attach_backing(&mut self, resource_id: u32, segs: &[(u64, u32)])
```

writing `nr_entries = segs.len()` and the entry array, with a static assert
that `24 + 8 + segs.len()*16 <= REQ_REGION_LEN`. The caller obtains `segs` from
`SYS_DMA_SEGMENTS` after `t_dma_map`.

---

## 4. Invariants

| # | Effect |
|---|---|
| **I-40** (no torn scanout / weave share integrity) | Substance unchanged: every page still stays backed and mapping-membership-immutable from first client map to retire. What changes is that the membership set is a LIST of blocks rather than one span, so `tapestry_present.tla`'s notion of "the weave's pages" needs to read as a set built from N blocks. The present/retire ordering is untouched. |
| **I-45** (GPU authority bounded by context) | The device now receives N addresses where it received 1. The obligation is new and must be explicit: **every entry sent in ATTACH_BACKING belongs to that weave's skein** — no other object's page may appear in the list, and the sum of lengths must equal the resource size. That is the invariant a prosecutor should attack first. |
| **I-7** (BURROW pages live until last handle closed AND last mapping unmapped) | Unchanged — refcounts live on the object, not per block. The free path frees N blocks instead of 1. |
| **I-32** (resource floor) | Improved: the page count charged is the same modulo tail rounding, and the tail waste falls from 24% to 2.5%. No new axis. |
| **I-12** (W^X) | Untouched — DMA pages are never executable; the prot guard is unchanged. |
| **I-37** (Weft cross-Proc share) | `burrow_share_into` must share the whole skein; the share admission gate keys on the `weave` bit, which is unchanged. |

---

## 5. What this does NOT do

- It does not touch plain `SYS_DMA_CREATE` (§3.2).
- It does not add a reserved arena, CMA, or page migration.
- It does not raise `KOBJ_DMA_WEAVE_MAX_SIZE` (still 64 MiB) — this makes the
  existing envelope *reachable*, which today it is not.
- It does not fix anon burrows' identical rounding (§1.2); it leaves a
  mechanism that could (§8).

---

## 6. Test plan — AS RUN

- **The failing case is the regression test**: boot at 2560x1664 and reach
  `scanout direct` + `console up`. PASSES at the tip (`exit=0`, 0 weave
  failures, `scanout direct 0 slot 0 (2560x1664)`, `console up 256x75 cells`);
  the full suite is 1470 PASS / 0 FAIL at that geometry.
- **Sabotage, both directions — and the first attempt measured the wrong
  thing.** Forcing `nblk = 1` while leaving the resolver's `SKEIN_BLOCK` stride
  at 2 MiB does NOT reproduce pre-skein behaviour: it makes
  `kobj_dma_pa_at` refuse every offset past the first block, so tapestryd
  SEGVs at `addr=0x2600000` and the run proves only that the resolver is live.
  The honest sabotage is `SKEIN_BLOCK` = 64 MiB, which yields nblk == 1 AND a
  matching stride — exact pre-skein semantics. **That run PASSES**, which is
  what established section 1.0's correction. A sabotage has to reproduce the
  OLD behaviour, not merely break the new one; breaking it proves the code
  runs, never that it was needed.
- **Unit-level discrimination**: under the `nblk = 1` sabotage, 4 of the 6
  `skein.*` kernel tests fail and exactly the 2 that assert NON-skein
  behaviour (`small_weave_stays_one_block`, `scope_is_weave_only`) still pass.
  A sabotage that passes everything, or fails everything, would have been the
  finding.
- **A fragmentation test**: fragment the buddy deliberately, then mint a weave
  — it must still succeed where a single-span allocation would not.
- **In-guest segment probe**: `sum(len) == size`, every `pa` page-aligned,
  blocks pairwise disjoint, `count <= max_entries` refused not truncated.
- **The existing gates unchanged**: ls-halcyon (both levers), ls-gfx-compose,
  the SMP gate. The weave path is on the I-40/I-45 audit surfaces, so this is
  an audit-bearing chunk and gets a prosecutor round on close.

---

## 7. The novel angle, recorded not built

If Thylacine ever does need the reservation family (a device with neither
scatter-gather nor an IOMMU), the Thylacine-shaped form is **the contiguous
arena as a conferred capability**: a slice of a boot-reserved arena granted
through the existing I-34 allowance, alongside MMIO windows, IRQ INTIDs, the
DMA per-buffer cap and PCI functions. A driver's contiguous budget becomes
part of its conferred hardware authority rather than a global scramble — which
is Genode's platform-driver model (§2.2) expressed in vocabulary we already
have, and narrows rather than widens authority.

Recorded as a NOVEL.md candidate; **v1.0 does not build it**, because
scatter-gather makes it unnecessary for every device we have.

---

## 8. Follow-on (not this chunk)

The `struct dma_block` list is the same shape anon burrows need to retire
their documented rounding deferral (§1.2). Doing it there is a separate chunk
with its own audit; noted so the mechanism is designed with that reuse in mind
rather than retrofitted.

---

## 9. The ballot, RATIFIED 2026-09-09

All four returned as recommended. Recorded with the consequence each carries,
so the implementation is bound by decisions rather than by preferences.

1. **ABI: a new `SYS_DMA_SEGMENTS`, with `SYS_DMA_MAP` fail-closed on a
   skein.** `SYS_DMA_MAP` keeps its exact contract for every current caller
   (virtio-net, virtio-blk, every ring), and returns a distinguished
   not-representable value rather than `blk[0].pa` when `nblk > 1`. The
   segments call REFUSES rather than truncates when the caller's buffer is too
   small. *Consequence: no existing caller is touched, and no caller can
   attach a partial backing and believe it whole.*
2. **`SKEIN_BLOCK` = 2 MiB.** 25 entries and 2.5% tail waste for the
   operator's 48.8 MiB display; the largest contiguous run needed falls from
   64 MiB to 2 MiB; 25 of the 78 available transport entries. *Consequence:
   a full 64 MiB weave uses 32 entries, leaving real headroom.*
3. **The name is `skein`.** `struct dma_block`, `KObj_DMA.blk` / `.nblk`,
   `SKEIN_BLOCK`. *Consequence: the weaving vocabulary (weave / weft / loom)
   gains the one term that names a physically-discontinuous, logically-single
   thread.*
4. **Scope: weave only.** Plain `SYS_DMA_CREATE` and the anon-burrow rounding
   (§8) are both out. *Consequence: the smallest surface to prosecute on an
   I-40/I-45 trigger, and the anon deferral stays a deferral — recorded, not
   silently inherited.*
