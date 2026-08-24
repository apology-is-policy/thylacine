---
id: sub-tapestryd
type: sub
title: "tapestryd — the compositor: the weave lifecycle, the present engine, and the retire ordering"
parent: moc-userspace
code: [usr/tapestryd/src/server.rs, usr/tapestryd/src/gpu.rs, usr/tapestryd/src/pane.rs, usr/tapestryd/src/input.rs, usr/tapestryd/src/main.rs, usr/tapestryd/src/chords.rs, usr/tapestryd/src/keymap.rs]
audit: hard
guarded-by: [inv-i40, inv-i5, inv-i34, inv-i1, inv-i45, inv-i9]
validated-by: [spec-tapestry-present, prose, gate-smp]
locks: []
hazards: [haz-driver-panic-dos]
abis: []
design: ["docs/TAPESTRY.md", "docs/AURORA-CONFIG.md"]
created: 2026-08-02
updated: 2026-08-24
---
## Purpose

The compositor: it owns both graphics-path PCI functions, serves
`/dev/tapestry`, and holds the server half of [[inv-i40]]. Clients hand
it pixels through a shared page (a *weave*) and a 32-byte present
descriptor; it transfers, flushes, and either scans a client's resource
out directly or composes several into its own screen buffer.

The warden binds it to `virtio-pci:16` (GPU) **and** `virtio-pci:18`
(keyboard) through the manifest's `gather` mode — one grant, one Proc,
an I-34 allowance narrowed to exactly those functions. Both ride PCI
because the six populated virtio-mmio slots share one page whose
lifetime belongs to stratumd, so a second persistent MMIO claimant is
structurally impossible.

Since the Warp arc it is **also the GPU seam**: when the device offers
`VIRTIO_GPU_F_VIRGL`, tapestryd serves a second tree, `/srv/warp`,
through which a client creates a virgl context, mints GPU buffer
objects, and submits 3D command streams. That half holds [[inv-i45]] —
whose **guest-exposure axis** is what this dossier describes; its host
and v3d axes are reserved and unbuilt respectively, so cite the axis
rather than the bare number.

## Contract

**The tree** is `/dev/tapestry`: a global `ctl`, `surface/` (with `new`
as the mint file, then per-surface `ctl`/`weave`/`present`/`event`/
`geometry`), `layout`, and `pane/<id>/`. Surface qids carry bit 40, pane
qids bit 41 — the same template ptyfs and netd use.

**The client protocol**, in the order a client uses it: open
`surface/new` (the mint rebinds the fid onto the new surface's ctl),
write `create W H`, `Tweft` the weave fid for a share id, `SYS_WEFT_MAP`
it, then write 32-byte present descriptors and read the event stream.

**The present descriptor** is version-pinned, names a slot and a damage
rect, and carries additional rects inline when `rect_count ≥ 2` — the
payload already lives in the client's registered buffer, so a separate
slice reference would be redundant indirection.

**The isolation contract (F2):** a surface resolves **only** for the
connection that minted it. Every client attaches its own session — open=
connect on `/srv/tapestry` mints a fresh SrvConn and dev9p session per
opener — so connection *is* client session. Procs that deliberately
*share* a session (fd inheritance, or ops through the shared boot mount)
share its surfaces: the Plan 9 shared-mount semantic, capability-coherent
because the session **is** the capability.

**The pane and layout tree is deliberately connection-global.** F2 gates
surfaces; it never gates the shared tree, because the tree is the window
manager's, not any one client's.

### The Warp tree — `/srv/warp`

Served only when virgl is negotiated. `ctl`, `caps` (the `GET_CAPSET`
blob), and `ctx/` with `new` as the mint file; a minted ctx exposes
`ctl`, `fence`, and `bo/` (again `new` as mint, then per-BO
`ctl`/`map`/`info`). Warp qids carry bit 42, with bit 39 marking a ctx
node and bit 38 a BO node — the same tag-bit template as the surface
tree, one level deeper.

A client's path is: open `ctx/new` → write its capset and ring count →
open `bo/new` and write a geometry → `Tweft` the BO's `map` fid and
`SYS_WEFT_MAP` it → write command streams to `ctl` → read `fence` for
completions. `present-to <surface> <bo>` (Warp-4) is how a GL client
gets its result on screen: the ctx *consents* to displaying one of its
BOs on one surface.

That consent is **pinned to `(slot, gen)`**, not to a slot. A surface
slot is reusable, and a consent naming only the slot would re-arm itself
against whatever surface next occupies it — a client displaying into a
stranger's window by outliving its own.

Bounds: 8 contexts globally, 1024 BOs per context, 8 in-flight fences
per context (`FENCED_SLOTS / 2`), 64 MiB of backing per context.

## Mechanism

The weave lifecycle, which is the spec's state machine as built
([[spec-tapestry-present]]):

| spec | code | what happens |
|---|---|---|
| `WeaveFirst` | `create W H` → `alloc_weave` | `t_dma_create_weave` (the kernel-minted share-admissible subtype) + map + **zero** + `RESOURCE_CREATE_2D` + whole-weave `ATTACH_BACKING` |
| `Reweave` | `resize W H <serial>` → `resize_ack` | mint the new generation, displace the old |
| `Map` | kernel-side | the client's `SYS_WEFT_MAP` claims the share **consume-once**; tapestryd never observes it |
| `Submit`/`Complete` | `present` | validate → `TRANSFER_TO_HOST_2D` → `RESOURCE_FLUSH` → `Rwrite`, all inside one dispatch |
| `RetireDisplaced` | `present`'s tail | the first post-fence present drops the displaced generation |
| `Destroy`/`ServerRelease` | `retire` / `release_gen` | the five-step teardown below |

`armed` becomes real **lazily**, at the first `Tweft` (`weft_ensure` —
the netd precedent): the share registers once and the stored id echoes
thereafter. The spec's Map guard is indifferent to *when* registration
happens; it cares only that retire disarms it.

The weave is **zeroed at allocation** — a DMA chunk must never leak a
prior occupant's bytes into a client mapping. It is triple-buffered: one
weave carries three page-aligned slots, and a present names the slot.

**The retire ordering** — `retire(n)` and its per-generation twin
`release_gen`, five steps whose order is the whole point:

1. **Quiesce.** Empty by construction (see Concurrency).
2. **`SYS_WEFT_UNSHARE`** — registry removal *before* page free, so a
   claim racing the retire finds nothing and fails closed. This
   discharges `NoStaleMap`.
3. **Scanout release** — forced explicitly, because two reconcile arms
   can leave scanout still naming the surface.
4. **`DETACH_BACKING` + `RESOURCE_UNREF`** — the resource dies before
   its backing.
5. **Unmap + close the weave** — `serverRef` drops. The pages survive
   until the client's mapping ref drops too, or until the kernel reaper
   force-reclaims an orphaned mapping after the compositor dies.

`retire` also clears a `last_focus` naming the slot (a stale one would
suppress the focus-gained event for a *future* surface minted there) and
closes the hosting pane before reconciling.

**The generation fence.** `resize_ack`'s `Rwrite` completes only *after*
the new generation is allocated, and the connection's frame stream is
FIFO — so every present sent after reading that ack validates and blits
against the new geometry. No per-present serial tagging is needed; the
reply ordering carries it. The new generation is minted **first**, so an
allocation failure leaves the current one untouched and the offer
standing for a retry. The displaced generation drains *passively* —
never read again, its last content still displayed, so tearing-freedom
holds — and retires at the first post-fence present. At most one drains;
a second reweave returns `E_AGAIN`.

### The fenced lane

The 3D half cannot be synchronous — a GL job takes as long as it takes —
so Warp-2d added a second lane beside the synchronous ring rather than
converting it. The audited two-page ring is **byte-identical**; fenced
chains get their own DMA region.

**The fence mechanism is borrowed, not built.** Setting `hdr.flags` bit
0 makes the device withhold that command's response until its fence
signals. So the used-buffer notification *is* the fence completion:
there is no fence machinery here, only a label on something the
virtqueue already does.

Slot `i` owns a fixed descriptor pair `(2+2i, 3+2i)`, a request buffer,
and a response header. Completion is attributed by **used-entry id** —
the head descriptor names the slot — never inferred from a ring cursor,
because up to `1 + FENCED_SLOTS` chains are outstanding at once and the
single-in-flight cursor check that the synchronous lane used cannot
survive that.

Three postures govern failure, and they are deliberately different:

| posture | trigger | effect |
|---|---|---|
| `dead` (latched) | any submit/ring failure, or a used entry naming a chain never published | every later submit fails fast — after one, no cursor can be trusted, and a freshly-zeroed response buffer reads as `resp_type=0x0` |
| abandoned | a chain unretired for 30 s | *that slot's bookkeeping* is reclaimed; the engine is **not** declared dead |
| refused (`E_AGAIN`) | every slot in flight | the client retries |

**A slow device is not a dead one**, and the distinction is the whole
design: tapestryd *is* the console, so a false dead-latch costs the
user their machine. That is also why a full lane refuses rather than
blocks — the serve loop must stay live.

Abandonment never frees anything the device might still touch. The
descriptor pair is **poisoned** (retired from the pool, not returned),
the owning context is marked, and every later retire under that context
**leaks rather than frees**. If the device does eventually retire the
chain, that late retire is proof it has finished: the slot un-poisons
and a *vindication* travels back to the seam, which frees the parked
backings. Leaked bytes and leaked *count* are both charged, and both
reset only at that live un-poison — an uncharge is honest only when
paired with the drop that actually frees.

The seam mirrors this. A context whose destroy arrives with fences in
flight is marked `retiring` — **instantly unresolvable to every client**
— and a per-pass pump finishes it once quiesced. Termination is the
driver's: an unretired fence is abandoned within the bound, which
decrements the counter, so the count always reaches zero. A wedged
context does not free its slot either, because `dev_ctx = slot + 1`:
handing the slot on would hand a live device context id to the next
client, and a stale stream would execute against a stranger's context.

### The coherent ring lane

Warp-6 V-3a adds a third client transport beside the synchronous ring and
the fenced lane: a **coherent shared-memory ring**, one per `ring_idx`
(0–63; Venus allocates one per VkQueue). It is a weft-shared GUEST blob —
`t_dma_create_gpu_bo(GUEST)` + `t_weft_share`, the client claims it with
`SYS_WEFT_MAP` — so both sides read and write the same pages with no
per-op syscall. No new kernel primitive: it rides the BO + weft + blob
machinery the earlier Warp arcs built.

The blob opens with a control header at offset 0, four SeqCst words:

| off | word | writer | meaning |
|---|---|---|---|
| `0x00` | `head` | **guest** | producer index |
| `0x08` | `tail` | host | consumer index |
| `0x10` | `idle` | host | 1 = pump parked; the guest kicks iff `idle==1` |
| `0x18` | `seq`  | host | monotone completed-seq feedback |

The CS ring proper lives past `WARP_RING_HDR` (`0x40`). A ring is capped
at `WARP_RING_MAX` (1 MiB) and refused if zero, unaligned, over-max, at a
`ring_idx >= 64`, or duplicating a live index (`wring_mint`). Mint zeroes
`head/tail/seq` and parks `idle=1`. The Tweft share is lazy — minted at
the first `Tweft` (the `weft_ensure` precedent) and disarmed at teardown
BEFORE any backing free, so a client's live mapping survives via the #847
dual count (`wring_teardown`). Rings issue no device fences at V-3a (no
submit lands, the device never DMAs a ring blob), so no wedge posture
applies to their retire.

**The qid tag.** A ring node is tagged `WARP_RING = 1 << 43` in the qid
`path`. This bit took two wrong picks first — `1 << 37` sat *inside* the
30-bit id field (bits 8..37), so `warp_id` read the tag back as an id;
`1 << 40` aliased `SURF_FLAG` — and the fix is now guarded by a
`const _: () = assert!` proving all six Warp tags (`WARP_BO`=38,
`WARP_CTX`=39, `SURF_FLAG`=40, `PANE_FLAG`=41, `WARP_FLAG`=42,
`WARP_RING`=43) pairwise disjoint and clear of both the id field and the
file-kind field. Two of the three encoding bugs were invisible to `make
test` and surfaced only on virgl, because a 2D device SKIPs `ctx/new`.

**The doorbell (`wring_kick`, [[inv-i9]]).** Writing `ring/<ridx>/kick`
sets `idle=0` and drains `[tail, head)`, echo-acknowledging each slot
(V-3a lands no device submit — the drain is a pure echo; V-3b's Venus
path replaces it with `gpu.submit_3d`). Each drain advances `tail`, and
`wring_complete` bumps `completed_seq` into the `seq` word. Two guards
make the loop sound:

- **The re-scan** is the register-then-observe half of [[inv-i9]]. After
  the loop finds nothing new it publishes `idle=1`, then *re-reads* head;
  if the guest advanced head in that idle-publish window (having elided
  its own kick because it observed `idle==0`), the re-scan catches it,
  re-clears `idle`, and continues. No advance is lost between the guest's
  head-store and the host's park.

- **The drain cap** (`WARP_RING_MAX_DRAIN_PER_KICK` = 4096) bounds one
  kick's passes. `head` is client-writable shared memory and tapestryd is
  single-threaded, so a multi-threaded client can advance head on one
  thread faster than the serve thread drains and pin it forever — a
  box-wide DoS, and tapestryd *is* the console. At the cap the kick
  publishes `idle=1` and breaks; both the direct drain and the re-scan
  `continue` re-enter the same gate, so it bounds every path. Found at
  round 2 as a live [P1]: round 1 had deferred it on the premise "the
  guest is blocked on the kick RPC so head is fixed", which names the
  wrong actor — the *kick caller* blocks, but the client's *other threads*
  own the head mapping.

**The contract the cap adds** ([[inv-i9]] guest obligation, round 3).
Breaking at the cap skips the post-drain re-scan, so for any advance still
pending (`head > tail`) at the cap the host drops its half of register-
then-observe. A ring client that blocks on `ring/<ridx>/fence` therefore
MUST re-check `idle` after its last head advance and re-kick if
`idle==1`; the host does NOT rescue a capped-out advance (fence
read/poll deliver on `completed_seq`, frozen at the cap). LATENT at
V-3a — the only client is the single-threaded prover, whose drain-to-
stable loop honors it, and a malicious client only strands *itself*. The
robust host-side rescue (a follow-up drain the serve loop runs after
servicing other conns) is OWED at V-3b, where the Venus ring is
doc-conformant and pipelined and this echo drain is gone anyway; it needs
a self-reschedule the V-3a serve loop lacks (tracked in the V-3b ring-kick
rescue design note + `docs/WARP-V3-DESIGN.md` section 4).

**Feedback + fence.** `wring_complete` publishes `completed_seq` into the
blob's `seq` word — the guest's zero-syscall poll fast-path — and the
blocking `ring/<ridx>/fence` reader learns the same value via
`poll_ring_fences` (coalesced at `reported_seq`).

### The HOST3D map-blob substrate (V-3b-1a)

Warp-6 V-3b (Model B) needs a ring that *virglrenderer* consumes, and that
demands a **host-allocated** blob (the coherent ring above is `blob_mem=GUEST`,
the opposite backing). Three `gpu.rs` methods build it — the substrate on which a
later rung mints Venus's real command ring:

| method | command | request | response |
|---|---|---|---|
| `create_host3d_blob(res, ctx, flags, len)` | `RESOURCE_CREATE_BLOB` `0x010c` | HDR+32 | `OK_NODATA` |
| `map_blob(res, offset) -> map_info` | `RESOURCE_MAP_BLOB` `0x0208` | HDR+16 | `OK_MAP_INFO` `0x1106` |
| `unmap_blob(res)` | `RESOURCE_UNMAP_BLOB` `0x0209` | HDR+8 | `OK_NODATA` |

`create_host3d_blob` sets `blob_mem=HOST3D` (`0x0002`), `blob_id=0`, and
`nr_entries=0` — the host allocates the storage, so there is **no guest
`mem_entry`** and the request is HDR+32 (the GUEST `resource_create_blob` is
HDR+48: same fixed fields at the same offsets, plus one 16-byte `mem_entry`). The
`ctx_id` rides the header; QEMU passes it straight to virglrenderer. `map_blob`
does `memory_region_add_subregion(&hostmem, offset, mr)` host-side, so the blob's
bytes surface at `hostmem_base + offset` — the PA a guest then maps via the V-2
`SYS_BURROW_FROM_HOSTMEM` — and returns the `virtio_gpu_resp_map_info` cache
word. That word is read at `RESP + HDR` (`+24`), which `submit_and_wait`'s
header-only zero (`0..24`) does not cover, so `map_blob` **pre-zeroes `RESP+24`
before submit** (the `get_capset_info` residue rule): a short-writing device then
reads as cache `0`, never a prior response's bytes.

**The venus-context requirement ([[inv-i45]], proven on GL 2026-08-24).** A
`HOST3D` `blob_id=0` `USE_MAPPABLE` blob is the **vkr (venus renderer) shm path**
(`vkr_context.c`: `blob_id==0 && blob_flags==USE_MAPPABLE`), and virglrenderer
serves it ONLY under a **capset-4 (venus) context**. A create under a virgl
context or device-global is refused (`RESP_ERR_UNSPEC` / `EINVAL`). So the ring
is minted under a venus context tapestryd owns — the [[inv-i45]] scope is the
host-side resource context, and tapestryd still forwards raw venus command bytes
without parsing them.

**The init self-test (`host3d_probe`).** Runs pre-Server, after `blob_probe`, so
no client resource or context is live. It skips *out loud* — a positive
`host3d-map skipped (...)` line, never silence — when `F_RESOURCE_BLOB` is
unoffered, when `shm_region(1)` is absent (no hostmem BAR), or when CONTEXT_INIT
was not negotiated. Two arms settle the context question: **Arm A** (a venus
capset-4 context, then torn down) creates + MAPs the blob — the positive, and the
Model B substrate; **Arm B** (device-global, `ctx_id 0`, a distinct offset) is
the **negative control** whose refusal proves the venus-ctx requirement is real,
not incidental to Arm A. Each arm's order is create → map → unmap → unref, and
the resource is unref'd before Arm A's ctx is destroyed. Because a HOST3D blob
has no guest backing, there is no `Dma` to unmap under a live host reference (the
`blob_probe` SF1 hazard does not arise here); a refused unref leaks only a
never-reused host resource id, bounded to one context's worth.

**Host prerequisite.** virglrenderer's venus renderer forks the
`virgl_render_server` binary (`/usr/libexec/virgl_render_server`) to service
HOST3D shm resource ops. Debian's process-mode `libvirglrenderer1` ships no such
binary, and without it `get_blob` returns a bare `EINVAL` with no fork-fail log —
the silent-failure signature that made the first proof a multi-boot hunt. thyla-pi
was provisioned with it (built from virglrenderer 1.1.0 source, installed
additively — it does not touch `libvirglrenderer.so`). Any fresh venus GL host
needs the same.

### The hostmem guest-map (V-3b-1b)

V-3b-1a's `map_blob` places a HOST3D blob in the hostmem BAR host-side; V-3b-1b
guest-maps it so tapestryd (and later, via weft, the client) can reach those
bytes. `HostmemAllocator` is a page-aligned **bump** allocator over
`shm_region(1).length`, handing out non-overlapping byte offsets relative to the
region window base — the SAME frame `map_blob(res, O)` uses, so a blob mapped at
offset `O` is guest-reached at `O`. `hostmem_map_probe` allocates an offset,
creates + maps a HOST3D blob under a venus ctx, then calls
`PciDev::burrow_from_hostmem(1, O, len, cache)` — `cache` the **host-dictated**
attribute (`map_info_to_cache(map_info)`, CACHED on KVM; never a guessed WC, the
GPU-DESIGN 6.2 honored-exactly rule the V-3b-1b F1 fix established) — the
[[inv-i45]] hardware authority is the held KObj_PCI claim; the kernel resolves
`bar.pa + window + O` and maps RW into tapestryd's burrow-attach window, and
round-trips a `u32` sentinel through the returned VA. The sentinel is a **same-address, same-core**
write-then-read — ARM coherency round-trips it with no barrier — so a MISMATCH
means the VA does not alias the BAR; it proves guest ACCESS only, not
host-visibility (virglrenderer polling the ring is V-3b-1c/2, deliberately not
claimed). Cleanup mirrors the probe discipline: `unmap_blob` exactly once per
mapped path (skipped on a map/burrow refusal that left nothing mapped),
`resource_unref` unconditional; the mapped guest VA leaks until proc exit
(bounded, one page at init — a dedicated unmap is the V-3b-1c-1 ring teardown,
below). The allocator was bump-only at V-3b-1b; V-3b-1c-1 makes it persistent and
reclaiming (the engine section below). The `SYS_BURROW_FROM_HOSTMEM` client binding itself
(`t_burrow_from_hostmem` + `T_CACHE_*` + `PciDev::burrow_from_hostmem`) landed in
libthyla-rs — V-2 built the syscall but left the wrapper to V-3.

### The persistent ring engine (V-3b-1c-1)

V-3b-1b's guest-map was a one-shot probe. V-3b-1c-1 makes it a reusable ENGINE —
the substrate the client-claimable Model B ring (V-3b-1c-2) and the venus-stream
forward (V-3b-2) build on. `HostmemAllocator` is hoisted into a persistent
`Gpu.hostmem: Option<HostmemAllocator>` (sized once from `shm_region(1)`) and gains
a **first-fit free-list**: `drop_host3d_ring` reclaims a retired ring's offset, so
a persistent daemon minting/retiring rings across client sessions does not exhaust
the 256 MiB region (bump-only would). No coalescing at v1.0 — ring blobs are
uniform-ish (page-rounded, `<= WARP_RING_MAX`), so same-size frees exact-match
without splitting and the list stays flat.

The lifecycle is a reusable pair. `mint_host3d_ring(res, ctx, len) -> HostRing`
composes alloc-offset → `create_host3d_blob` (under a venus ctx) → `map_blob` →
`burrow_from_hostmem` (host-dictated cache), with **full error-path unwinding** at
each of the three failure points (offset → resource → subregion), so no half-minted
ring survives; a `u32::try_from(size)` guard fails a `len` that page-rounds past a
u32 rather than truncating the wire size to a 0-byte create. `drop_host3d_ring`
is the inverse (detach → unmap → unref → reclaim), and **logs** a device refusal
because a swallowed one surfaces later as a bogus `reuse=false`, indicting the
free-list for a teardown fault.

**`HostRing` is deliberately NOT `Copy`, and `drop_host3d_ring` takes it BY VALUE**
— the holotype F1 catch. A `Copy` handle + a by-ref drop + an unvalidated `free`
compose into a silent double-free: two copies each drop the same ring, `free`
pushes the offset twice, and two later mints hand ONE hostmem offset to two
clients' rings — cross-client aliasing, no log line, live the day 1c-2 adds a
second retire path (a death reaper AND a close verb, the shape tapestryd already
has for BOs). The move-only handle makes the double-drop a compile error; a `free`
oob/overlap guard (rejects an extent past the bump watermark or overlapping a freed
one) is the belt to that suspenders. The reusable lesson: a resource handle that
is `Copy` is a double-free waiting for a second caller.

**The engine proof** (`hostmem_ring_probe`, init-time, single-threaded): mint TWO
rings under one venus ctx (the allocator must hand DISTINCT offsets, `0x0`/`0x1000`),
write an **offset-derived** sentinel through each guest VA, then **re-read BOTH** —
if a host/kernel defect aliased the two backings onto one PA, one write clobbers the
other and the re-read mismatches, so the probe witnesses PHYSICAL distinctness, not
merely distinct allocator offsets (holotype F2 — the 1b probe proved only the
latter). Tear both down, then re-mint and assert the freed offset is REUSED (the
free-list). One verdict line, emitted only on the four-way conjunction:
`hostmem-ring MAPPED+ROUNDTRIP x2 (off_a=.. off_b=.. cache=CACHED) teardown+remint-reuse OK`;
else `hostmem-ring FAIL (...)`. The `venus-verdict` gate anchors on the `x2`
success line (a FAIL line — any property false — is rejected), and
`test-venus-verdict.sh` proves the discrimination without a boot, including a
`reuse=false` FAIL-line leg so a lifecycle regression cannot ride an absent-token
check. GL-VERIFIED on thyla-pi KVM/V3D 2026-08-24. What is NOT here: the ring is not
yet a client-claimable `/srv/warp` file — the weft-share of the hostmem burrow
(`WEFT_BIND_HOSTMEM`), the per-client venus device-ctx, and the `warp-prove`
cross-Proc leg are V-3b-1c-2.

## Data structures

`Surface`: the current `Weave` (handle, VA, size, optional share id),
its resource id, `w`/`h`/`slot_stride`, an optional `old_weave` for the
draining generation, `owner_conn` + `gen`, a bounded event queue, and
the state (`Minted` → `Woven` → `Live`).

Bounds — F9: 8 surfaces globally, **4 per connection**, dimensions ≤ the
display (the weave is tapestryd's own DMA allocation, so the client's
page budget does not bound it), 8 connections, 32 fids, a 128-entry
per-surface event queue, 64 rects per present.

`Comp` holds the surfaces, the pane `Layout`, the `Gpu`, the scanout
mode (`Off` / `Direct(n)` / `Composed`), and the bump-allocated weave VA
window.

The `tevent` record is 24 bytes, version-pinned; pointer MOVE packs
surface-**relative** coordinates, never absolute screen ones.

`WarpCtx`: owner connection, public and device context ids, the declared
capset and ring count, the fence bookkeeping, the two failure flags
(`fence_poisoned` / `stream_rejected`), the leak accounting
(`leaked_bytes` + `leaked_count`), the health probe, the `present_to`
consent, and a heap row of 1024 BO slots allocated at mint.

`WarpBo`: a kernel-minted GPU-BO DMA chunk attached as the backing of a
device-global 3D resource and shared to the client by `Tweft`. The
share is minted lazily and **disarmed at retire before any backing is
freed** — the same ordering as the weave.

Two counters deserve care. `fence_signaled` is a **dense per-context
completion count**, deliberately not the device-global fence id: they
usually move together, which is what makes treating them as one number
space so easy and so wrong. And the fenced-write ledger
(`rx` / `minted` / `again` / `err`) exists so a client's own count of
successful writes can be reconciled against the server's at quiescence —
`rx - minted - again - err > 0` names an answered-without-dispatch path
that no single counter would reveal.

## Concurrency

Single-threaded, like its siblings. The interesting property is not a
lock but a *shape*:

**Every present is handled synchronously** — validate, transfer, flush,
reply, all inside one 9P dispatch — because `gpu.rs` submits a
two-descriptor chain and waits. So the in-flight present set is
**provably empty at every retire decision point**, and the spec's
`ServerRelease` guard (`intransfer = 0`) holds by construction rather
than by a drain.

**This is the single most important thing to know before touching this
subsystem.** A pipelined controlq — the obvious performance lift — does
not make the guard *false*; it makes it **unimplemented**, silently,
with the model still green. Any move that way must land a real drain
first.

**That lift has since happened, and the guard survived — by exclusion,
not by a drain.** Warp-2d pipelined the controlq for fence-bearing 3D
chains, which is exactly the move above. It is sound here because
**presents were left out of it**: a present still submits and waits
inside one dispatch, so the in-flight *present* set is still provably
empty at every retire decision point. The pipelined present path remains
unbuilt.

So the property to check before touching this is now narrower and
easier to break. It was once "the controlq is synchronous", which one
glance at `gpu.rs` confirmed. It is now "**presents** are synchronous",
while the file plainly contains a non-waiting submit path — and a future
edit that routes presents through the fenced lane for the obvious
throughput reason would look like using existing machinery rather than
like removing a guard. The two lanes are one type and one call away from
each other. Anything moving a present onto the fenced lane still owes
the real drain.

Deferred event reads use the ptyfs shape: park a `PendingRead`, deliver
from `poll_events` at the loop top, with four cancel sites (conn death,
clunk, `Tversion`, `Tflush`).

## Invariants enforced

[[inv-i40]] — the retire ordering above (`NoStaleMap`, `NoTornScanout`),
the reweave fence (`ReweaveOrdered`, `DisplayedBacked`), and the
completion-not-submit recycle gate (`RecycleGate`).

[[inv-i5]] and [[inv-i34]] — the gather grant confers exactly the
matched nodes' own bdfs and INTIDs; the allowance is never fabricated
per axis.

[[inv-i1]] — the F2 owner + generation gate at every surface-qid
consumer.

[[inv-i45]] — the Warp seam: a client's GPU authority is bounded by the
context it minted, and no device state or guest backing is freed while
the device may still be using it. Enforced by the slot poisoning, the
leak posture, the `(slot, gen)` consent pin, and the deferred retire —
all described above. **The wikilink dangles on purpose**: I-45 is named
by the audit-trigger row, by `GPU-DESIGN.md`, and by the source itself,
and it is absent from `ARCHITECTURE.md` §28. See Caveats.

The Warp seam's health signal has **two independent failure axes** and
reading one through the other has already caused a defect:

- **`fence_poisoned`** — a chain of this context never retired. Says
  nothing about whether commands *ran*.
- **`stream_rejected`** — the host renderer latched this context's
  command stream off. Submissions are still accepted and their fences
  still retire *normally*; simply nothing executes. Sticky, mirroring
  `glGetGraphicsResetStatus` and `VK_ERROR_DEVICE_LOST` — the remedy is
  recreate, never retry.

A context can be perfectly healthy on the fence axis while executing
nothing at all, which is why a fence-based liveness check cannot detect
it. The detector is a health probe: two 1×1 resources the client can
never name, one holding a fixed value and one the target of a copy.
They are kept **out of** the per-context BO array deliberately — every
client-facing resolve walks that array, so membership would be exactly
the reachability that lets a client forge a healthy verdict, or
manufacture a rejection against a context that is fine.

The probe is rate-limited to **one per context per compositor tick**.
It costs three synchronous device round trips on the dispatch thread and
a client triggers it, so an ungated version is a fresh denial-of-service
lever against the console — the fenced lane has admission control for
precisely this reason and the synchronous slot bypasses all of it. One
per tick is the cadence the design intends anyway, so the bound costs
the intended use nothing and caps the whole machine at 8 probes a frame
regardless of what clients do.

**The untrusted edge** is `present`, and it validates the version word,
`rect_count ≤ 64`, the **exact** payload length for the declared count,
the slot index, and **every rect before any pixel work**, in `u64`
arithmetic so `x + w > surface_w` cannot wrap. Validate-all-then-act is
the same discipline as ptyfs's ctl grammar, for the same reason.

**The global-ctl authority gate is default-deny**: `is_ungated_ctl` is a
denylist of exactly the determinism verbs, so every *future* global verb
is gated by construction. An allowlist would silently ungate a verb
added without touching the gate line. That inversion is the design and
must survive any edit.

## Error paths

Any `gpu` submit failure latches `dead` — fail fast, never the
zeroed-response cascade (below). Present errors surface as `EIO`;
resize-ack "not now" verdicts as `EAGAIN` (stale serial, or a prior
reweave still draining), which the client answers by draining events or
presenting a frame and re-acking.

`alloc_weave` rolls back fully on every failure path — the DMA close,
the unmap, and the resource unref each unwind what preceded them.

**The never-drop set (R2-F4).** `FRAME` coalesces globally — at most one
queued per surface, its tick refreshed in place (a back-of-queue-only
check let interleaved KEY/FRAME streams accumulate, the G-3 F3 fix).
Relative pointer motion coalesces by **summation**, not replacement,
because replacing deltas loses motion. `CONFIGURE` coalesces wholesale —
only the latest serial matters. On overflow with a non-droppable event
pending, one coalescible entry is evicted; if there is nothing
coalescible the client is dead or stalled and the surface **wedges**
(force-retire and close). It never blocks and never drops a control
event for a live client.

## Performance

Direct scanout is the zero-copy fullscreen path: the client's own
resource is scanned out and a present is transfer + flush on it.
Composed mode blits the damage into tapestryd's screen buffer at the
pane's content rect, which copies — so the screen resource references no
client weave, which is exactly what makes the post-fence retire safe.
Triple buffering keeps the client a frame ahead. Damage is per-rect;
`rect_count == 0` means full-surface.

The synchronous command engine is the cost side of the quiesce
construction: one IRQ wait per GPU command.

## Prosecution

- The **retire order** is the invariant: unshare before any backing
  free, resource before backing, scanout off before the resource dies. A
  reordering reopens `NoStaleMap` or `NoTornScanout`.
- **Every** surface-qid consumer must carry the owner + generation gate
  — walk, readdir, open, read, write, `Tweft`. A missed gate is a
  cross-client screen scrape.
- Present validation stays validate-all-before-any-pixel-work, in `u64`.
- The gather grant: every conferred bdf and INTID must be some *matched*
  node's own.
- The global-ctl gate stays a **denylist**.
- **The completion authority is the used ring, never the ISR bit**
  (#31). VIRTIO orders the device's `used.idx` write before its
  notification, but a wake proves only that *some* notification-ish
  event arrived: irqfwd collapses INTx edges, and a level re-fire or
  config event can latch a stale pending event — routine under a live
  display backend. The pre-fix shape (break on the first `ISR_QUEUE`
  wake, read `used.idx` once, fail if behind) turned that benign timing
  into a *permanent* engine desync: `seq` diverged from the device's
  avail consumption, and every later command re-published a consumed
  avail index and read its own zeroed response buffer as `resp_type =
  0`. The wait must never break on the ISR alone; the dead latch must
  stay one-way.

## Seams

- **`h_walk` accepts `P9_NOFID` as a newfid and silently rebinds a
  newfid that is already in use.** Its two siblings reject both — netd
  because they *are* its `net-4d` F2 fix, ptyfs by inheritance. Bounded
  and self-inflicted here (tapestryd's fids carry no refcount, so a
  clobbered binding leaks nothing), but it is the guard that carries
  ptyfs's `HupAtMostOnce` argument. Task #47.
- The weave-mapping VA window is bump-allocated and freed VAs are not
  reused — bounded by the surface caps per generation against a 47-bit
  space. A free list is a v1.x seam.
- A session peer can close or steal focus from another client's pane.
  The v1.0 trust boundary is the per-territory `/srv`: `/srv/tapestry`
  lives in the driver's territory and only the trusted boot chain
  connects. A per-client ACL is the Halcyon-era seam.
- A tapestryd crash resets its virtio devices — scanout blanks until a
  restart re-inits ([[haz-driver-panic-dos]]).

## Caveats

- **`h_version` replies `9P2000.L` to any proposal** and sets
  `version_done` unconditionally, where ptyfs replies `unknown` for an
  unsupported version. Inert — the only client proposes `9P2000.L` —
  and the same shape as the `h_walk` seam above: a template guard that
  did not come across.
- `parse_dec` accepts leading zeros, so `surface/007` and `surface/7`
  name the same surface; readdir emits only the canonical form. (ptyfs's
  twin rejects leading zeros for exactly this reason.)
- A pane qid carries only the low 24 bits of the pane id while the
  `layout` file parses the full `u32`. They agree for the first 2^24
  allocations; past that the paths diverge into a miss, never a
  cross-pane alias. Bits 8..40 are free below the pane flag, so widening
  is cheap if it ever matters.
- The hold flag (deferred device-visible push, for deterministic tests)
  is stripped to `E_OPNOTSUPP` in production builds. Pixel work still
  happens in-dispatch even when held, so tearing-freedom is unaffected
  by which build is running.
- **[[inv-i45]] names three axes and only one is enforced.** The
  guest-exposure half is what this dossier describes; the host half is
  *reserved, not enforced* — virglrenderer does the bounding and is
  documented trusted — and the v3d half is unbuilt. Cite the axis, not
  the bare number. (Until main's `5da054e4` there was no §28 row at all
  and the only definition was headed "(proposed)" while calling the same
  bound enforced 360 lines earlier, which is how the ambiguity arose;
  `tools/check-invariants.py` now fails the build if that registry drifts
  again.)
- **The fenced lane's test hold ships in production.** The crate's
  default features include `test-mode`, no build passes
  `--no-default-features`, and `/srv/warp`'s `ctl` is mode 0666 — so the
  verb that stalls a context's fence completions is reachable by any
  client. The design answer was not to gate the caller but to make the
  power proportionate: a client may hold only **its own** context's
  fences, which it could already achieve by simply not reading them. An
  earlier revision held globally, which was an unprivileged box-wide
  denial of service. Worth knowing that identity deliberately cannot
  separate the prover from an attacker here — the in-guest test battery
  is an ordinary unprivileged client *by design*.
- The abandonment bound is 30 s of wall clock, which means a genuinely
  hung host renderer strands a context's backings for that long before
  anything is reclaimed, and leaks them permanently if the device never
  retires. That is an accepted risk recorded in `GPU-DESIGN.md` §9.2,
  not an oversight — the alternative is declaring a slow device dead,
  and the console is what dies with it.

## Provenance

The Tapestry G-arc: the kernel weave share (G-2), the compositor and the
orphaned-weave reaper (G-3), the console renderer role (G-4), the pane
tree + resize generations + the interaction layer (G-6), and the display
config surface (cfg-3). Swept into the vault by
[[chg-2026-08-02-server-sweeps]], which mints [[inv-i40]] and
[[spec-tapestry-present]].

## Tests

In-guest: the per-boot pattern gate drives the full path with a liveness
double-dump; `ls-gfx` drives QMP-typed input through the whole loop
asserted on the serial tee; `ls-gfx-live` covers the VNC live-display
leg; `ls-gfx-panes` (22 legs) covers the G-6 pane tree; `ls-gfx-mode`
covers the display-mode verb and its authority gate. The `test-mode`
cargo feature strips the determinism surface to `E_OPNOTSUPP` for
production, and both variants compile.

The V-3a coherent ring is proven on the GL host by `warp-prove ring`
(`tools/warp-host.sh ring`), virgl-only — a 2D device SKIPs `ctx/new`, so
`make test` proves only non-regression. Its legs: the round-trip (map +
doorbell + feedback + fence); the F2 rejections (zero / unaligned /
over-max / `ridx>=64` / duplicate index); the [[inv-i45]] ownership gate
(a second conn's LIVE ring, one variable away from the negative — a
regression that ignored `owner_conn` is caught); the [[inv-i9]] re-scan
discrimination (delivered by default / lost under the `ring-noscan` buggy
lever / recovered when re-enabled); and the round-2 F1 drain-cap bound (a
512 KiB ring, `ring-inject 1 5000` past the 4096 cap, one kick bounded
then all drained across re-kicks with no work lost). `usr/warp-prove/src`
is UNOWNED — its reference lives in `docs/reference/149-warp.md`; the
coverage sweep is [[seam-warp-prove-unowned]].

## Referenced by

[[spec-tapestry-present]] · [[inv-i40]] · [[sub-ptyfs]] ·
[[sub-kernel-weft]] · [[moc-userspace]].

## The create-time door has NO lower bound on a client-declared backing (2026-08-18)

Deliberate, and it was briefly otherwise. The C-6b audit close added a brace to
`wbo_create` refusing a `B8G8R8A8_UNORM` declaration whose backing could not
hold its base level. The follow-up round removed it: its premise is
contradicted by this project's own Mesa winsys, in a comment at the line that
picks the size (`usr/ports/mesa/patches/0006-*.patch:1511`) --

> The seam refuses unaligned or zero backings; the driver's staging-path
> textures legitimately ask for size 1.

Mesa declares one byte on two paths that keep the true width/height (staging,
`alloc_size = 1`; MSAA, `total_size = 0`) and the winsys rounds it to a page.
So "512x512 declared, 4096 offered" is byte-for-byte BOTH the read-overrun
attack shape and an ordinary staged or multisampled texture. **The declaration
carries no information that separates them** -- only the reader can, by whether
it is about to read the backing.

The lower bound therefore lives at the READ gate, `gl_adoption`: exact
(`b.size >= b.w * b.h * 4`, and adoption already pins `b.w == s.w && b.h ==
s.h`, so it is the reader's geometry), re-evaluated at retire through
`same_adoption`, and on the only path that reads a BO backing with foreign
geometry. A host-only resource never adopts, so it never reaches the read.

**Do not re-add a create-time floor.** The failure it caused was invisible to
every gate: the staging arm depended on a virglrenderer capset bit nothing in
this tree measures, and the MSAA arm refused every multisampled BGRA target
above 32x32 outright. A gate proves what the system DOES; an over-refusal
shows up only as something a client can no longer do.

## `import_skip_said` -- the one-shot half of the import rate limit

`comp_import_bo`'s `!composable` arm carries BOTH a per-tick rate limit
(`WarpCtx.import_tick`) and a per-ctx one-shot latch (`import_skip_said`),
because the tick limit alone still permits `clock_hz` 60 x `MAX_WARP_CTXS` 8 =
~480 synchronous console lines a second from ordinary unprivileged clients --
the same magnitude, in the same file, that `verify_diag_arms` exists to answer.
`comp_attach_refused` carries the rate; the latch carries the report.

## The device feature offer is reported, not discarded (2026-08-18, Warp-6 V-0)

`Gpu::init` reads both feature dwords during negotiation, uses **one bit** of
the low one (`VIRTIO_GPU_F_VIRGL`), and discarded the rest. The value was in a
register and then gone.

That mattered the moment Warp-6 opened. "Does this host offer
`VIRTIO_GPU_F_CONTEXT_INIT`?" is the question that decides whether a **Venus
context is reachable at all**, and it had no answer short of writing a new
build.

It is now one line per boot:

```
tapestryd: gpu features lo=0x3000001b (virgl=1 edid=1 uuid=0 blob=1 ctxinit=1) hi=0x00000101
```

Measured, and **identical on thyla-pi (KVM/V3D) and thyla-gl (TCG/lavapipe)**:

| device | `lo` | virgl | edid | uuid | blob | ctxinit |
|---|---|---|---|---|---|---|
| `virtio-gpu-pci` (default 2D dev device) | `0x30000002` | 0 | 1 | 0 | 0 | 0 |
| `virtio-gpu-gl-pci` | `0x30000013` | 1 | 1 | 0 | 0 | **1** |
| `+venus=on,blob=on,hostmem=256M` | `0x3000001b` | 1 | 1 | 0 | **1** | **1** |

`0x30000000` is `VIRTIO_RING_F_INDIRECT_DESC` + `VIRTIO_RING_F_EVENT_IDX`;
`hi=0x00000101` is `VIRTIO_F_VERSION_1` + `VIRTIO_F_RING_RESET`.

**The trap this closes.** `ctx_create` writes `context_init = 0` under the
comment *"F_CONTEXT_INIT not negotiated"*, and the device honours that field
**only** when the feature is negotiated -- which this driver never offers back.
So selecting a capset by writing it into `context_init` today would write into
an **ignored** field, collect `RESP_OK_NODATA`, produce an implicitly-virgl
context, and report success. Any future capset-selection work is a
**feature-bit** change first; the field-only version is a false pass.

**The constraint it exposes.** `VIRTIO_GPU_F_RESOURCE_BLOB` appears only with
`blob=on`, and the default dev device offers neither it nor virgl, so blob work
cannot be exercised on the local dev loop at all -- the same shape as #166's
inert-hostmem-under-HVF constraint.

The line is emitted before the `virgl` gate, so it reports on **every** boot
including 2D ones, and it prints before any hang in later init -- which is what
made it useful on a host where tapestryd later gave up.
