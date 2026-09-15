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
updated: 2026-09-06
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
a second reweave returns `E_AGAIN`. A resize-ack that arrives DURING the
drain is likewise refused `E_AGAIN` and its deferral latched
(`Surface.ack_deferred`); when the drain completes at that first post-fence
present, `release_displaced_gen` **re-offers** the standing configure under a
fresh serial (A-F2, [[chg-2026-09-06-harc-audit-close-r1]]) — because a client
that only drains-and-acks-the-newest would otherwise never re-ack the refused
offer and never learn its new size, a recovery no client implemented and latent
since G-6b.

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

### The server host3d-ring path (V-3b-1c-2a)

The 1c-1 engine minted rings internally (the probe). V-3b-1c-2a wires it into the
`/srv/warp` SERVER so a HOST3D ring is a first-class ring flavor under a client's
warp ctx — the tapestryd half of the client-claimable ring (the weft-share client
CLAIM + the `warp-prove` cross-Proc leg are 1c-2b). Four pieces, all in
`server.rs` bar a one-line `gpu.rs` wrapper and a `main.rs` call:

**The per-client venus device-ctx.** `WarpCtx` gains `venus_ctx: Option<u32>`,
**lazily** created on the first host3d ring mint (`wctx_venus_ensure` →
`gpu.ctx_create_venus`, capset-4) and destroyed with the warp ctx in
`wctx_finish` (condemn-slot-on-refuse, before the `dev_ctx` destroy — else a
reused slot could re-mint the venus id into a live host ctx). A client that mints
only V-3a guest-blob rings pays no venus ctx. Section 0.6 ratifies one venus ctx
per client (V-3b-2's forward reuses it, no rework). The id is a **dedicated band**
`WARP_VENUS_CTX_BASE (0x200) + slot`, pinned disjoint by a `const _` gap assert +
a `conv_attempt` `debug_assert`. The band matters because the recon's first
choice — `COMPOSITOR_CTX + 1 + slot` — **aliases `CONV_PROBE_CTX_BASE`
(`COMPOSITOR_CTX + 1`)**, the conv-probe throwaway ids: an enumerate-mirrors catch
found by grepping every `ctx_create*` id before writing, not after a collision.
Temporally the conv throwaways are destroyed before any client mints, so the
alias was latent; the disjoint band + the assert make it structural.

**The HOST3D ring flavor — ADD, not replace.** `ring/new` accepts
`"<bytes> <ridx> host3d"` (a bare `"<bytes> <ridx>"` stays the coherent guest-blob
ring; an unknown third token is rejected). `wring_mint` gains `host3d: bool` and,
after the SHARED validation + the I-32 backing-budget + the ridx checks (so a
host3d ring is bounded EXACTLY like a guest-blob ring), branches to
`wring_install_host3d` — mint via the 1c-1 `mint_host3d_ring` under the venus ctx,
install `WarpRing { dma_fd: -1, host3d: Some(hr), .. }`. A venus-ctx or engine
failure fails the mint CLEAN (the engine unwinds its own partial state; a venus
ctx created-then-mint-fails is cleaned at `wctx_finish`).

**Teardown routes to the engine.** `WarpRing` gains `host3d: Option<HostRing>`;
`wring_teardown` moves the non-`Copy` token into `drop_host3d_ring` and `return`s
BEFORE the guest-blob `res_unref` / `dma_fd` path, so a host3d ring's resource is
unref'd exactly once (by the engine) — the type system forbids a second drop, and
the early `return` forbids a double-unref. `dma_fd: -1` is safe at every reader:
`wring_weft_ensure` returns `None` for it (host3d weft-share is 1c-2b's
`WEFT_BIND_HOSTMEM`, not the guest-blob `t_weft_share`), and the guest-blob detach
is past the early `return`. `wring_teardown` is the SOLE ring-free path
(`wctx_finish`'s loop), so no free bypasses the host3d arm.

**The kick fail-closed guard** — the self-audit catch. Because the flavor is
client-reachable via `ring/new` at 1c-2a (not only the self-test), a client can
create a host3d ring AND `kick` it; `wring_kick` reads the V-3a `WARP_RING_OFF_*`
control header, which a host3d ring does NOT carry (its memory is Venus's format).
Un-guarded, a kick would write V-3a control words into a Venus page — bounded by
the round-2 drain cap and contained to the client's own ring (no UAF, no
cross-ctx), so not a soundness hole, but wrong. `wring_kick` now rejects a host3d
ring `E_OPNOTSUPP`; the real host3d kick becomes `gpu.submit_3d(dev_ctx, ridx, cs)`
at V-3b-2. (The `map` path already fails cleanly via `wring_weft_ensure`; the
`fence` read blocks with no completions, which is the fence contract, not a bug.)

**The boot self-test** (`warp_host3d_selftest`, called from `serve()` before
READY, self-skipping like the gpu probes) mints a warp ctx under a synthetic conn
(`u64::MAX` — unreachable by the `wrapping_add(1)`-from-0 conn counter, and torn
down before the accept loop), mints a host3d ring, round-trips a sentinel at the
ring VA, and finishes the ctx — exercising the venus-ctx create/destroy, the ring
install, and `wring_teardown`'s host3d arm end to end with NO client. One line:
`warp host3d-ring venus-ctx=<id> MAPPED+ROUNDTRIP teardown OK` (only on a
successful round-trip; a mismatch → `FAIL`, a 2D/no-blob/no-venus device →
`skipped`; 1c-2b inserts a `refcount=1` field before `teardown OK`, below). It asserts the SERVER WIRING, not host distinctness — 1c-1's
`hostmem_ring_probe` already proves the physical host-backing. The `venus-verdict`
gate gains a server-path leg (the `venus-ctx=` line present on the test boot,
absent + a `skipped` line on the 2D control); `test-venus-verdict.sh` proves the
discrimination without a boot (28/28), including a `FAIL` sabotage.

Audit (holotype-reviewer Fable 5 max, family diversity): 0 P0 / 1 P1 / 1 P2 / 3
P3, all fixed; GL-verified on thyla-pi KVM/V3D. **F1 [P1] was the author's miss**:
the venus destroy sat below the `wctx_finish` leak-arm return, and the vindication
recovery un-poisons the slot destroying only dev_ctx — a wedged-then-recovered
slot leaked its venus ctx AND permanently lost host3d (EEXIST on re-mint). The
self-audit reasoned the leak arm was "consistent with dev_ctx" and stopped at the
boundary of the changed function; the bug lived one call away in the vindication
path it never opened — the whole-system-stewardship failure mode, closed by a
context-independent reviewer. Fix: destroy venus in the leak arm too (quiesced by
construction here), skip the vindicate stamp on a refused destroy. F2 [P2] the
recycled-hostmem zero above. Two gate-instrument lessons this rung: the no-boot
28/28 tests the VERDICT, not the CAPTURE — the real venus boot found
`boot-probe.sh`'s `tapestryd: gpu`-only filter dropping the `warp` self-test line
(broadened to `gpu|warp host3d-ring`); and F4, "teardown OK" was keyed on the
sentinel alone until it was made to read the poisoned flag — an assertion is not
an observation.

### The client-claimable ring and observe-and-reap retire (V-3b-1c-2b)

1c-2a left two things dead: a client could `ring/new` a host3d ring but not
**claim** it (the weft-share fell through), and its teardown lifetime was unsolved.
1c-2b closes both. The claim is the kernel's [[sub-kernel-weft]] F1 arm
(`WEFT_BIND_HOSTMEM`); `wring_weft_ensure` now `t_weft_share`s the host3d ring's
hostmem burrow instead of returning `None`, so a peer Proc can map the ring's GPA.

The hard half is the retire, and the problem is structural: a host3d ring's host
bytes are a QEMU subregion tapestryd owns, which lives **outside** the kernel's
#847 dual count — so the kernel cannot keep tapestryd from freeing an offset a
client still maps, the way it keeps a shared Burrow's pages alive. The prior art
settles what tapestryd *can't* do: Fuchsia's VMO and Genode's dataspace keep
device-memory lifetime in the kernel precisely so no userspace free races the
refcount; Thylacine can't, because `unmap_blob` is a controlq op only tapestryd
issues. So the next-best is to make tapestryd **observe** the count before it
frees — `image.c`'s cache-eviction check (`handle==1 && mapping==0`) lifted to
userspace across a new read-only syscall.

`retire_host3d_ring` reads `t_hostmem_refcount` ([[sub-kernel-syscall-dispatch]]'s
`SYS_HOSTMEM_REFCOUNT`, which returns [[sub-kernel-burrow]]'s `burrow_total_refs` —
`handle + mapping` under `v->lock`) **after** the caller has disarmed the weft
share, and reclaims the offset (`drop_host3d_ring`) only at count `== 1`: the only
reference is tapestryd's own map, no client map AND no client holding the
transferred claim pin. At count `> 1` it **parks** the ring on `Gpu.hostmem_parked`
with its VA still mapped; `reap_hostmem_parked` reclaims parked rings whose count
has dropped back to 1, run at **mint** (reclaim-before-alloc, so offset pressure
drives reclaim exactly when a new mint needs the space) rather than the completion
pump (mint runs in the serve-loop request context, where the controlq teardown
`drop_host3d_ring` issues is established-safe; the pump is not). `drop_host3d_ring`
is now reachable *only* through retire/reap, never directly on a client-shared
ring.

**Two audit rounds, both dirty, both on the reap predicate — the surface's whole
difficulty is that predicate.** Round-1: the reap first keyed on `mapping_count==1`
alone, which misses a client that has **claimed** but not yet mapped —
`weft_share_claim` transfers the registration pin (a *handle* ref) before
`burrow_share_into` bumps `mapping_count` in the same `SYS_WEFT_MAP`, so a
mapping-only read frees the offset under a pending map (a cross-client alias). The
citation named `image.c`'s gate but dropped its handle half — *the* half that
excludes the in-flight mapper. Fix: return the **sum**, which the pin lifts to `>=2`
during that window. The generalizable lesson: **a lifecycle predicate ported across
actors** (the kernel cache holds a HANDLE; tapestryd holds a MAPPING) **must
re-derive which refcount half carries the safety**, not transliterate the visible
one. Round-2 caught that the sum was itself two lock-free loads (closed kernel-side
by `burrow_total_refs` reading both under `v->lock` — see [[sub-kernel-burrow]]).
Round-3 came back clean (4 P3, all prose). The reap loop is bounded (per-pass cap;
the parked list strictly shrinks; a mint under pressure loops reap+alloc while a
pass reclaims), and a crashed client's ring is reclaimed at the next mint because
Proc death drops the mapping.

GL-verified on thyla-pi KVM/V3D: the self-test line now reads `warp host3d-ring
venus-ctx=<id> MAPPED+ROUNDTRIP refcount=1 teardown OK` — the `refcount=1` is
`burrow_total_refs` observed at runtime, `teardown OK` is `retire_host3d_ring`
taking its `==1 → drop` path. The client-claim cross-Proc reproduction
(`warp-prove ring-host3d`) is a tracked follow-on; the SUBMIT_CMD forward is
V-3b-2.

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
- **The placement-claim mint is owner-gated (H-4b-2, CLOSED).** H-4b-1 minted
  on emptiness alone (an interim "any peer" rule); H-4b-2 narrowed it to the
  leaf's OWNER -- Renderer anywhere / a session iff `owner_principal == p` /
  a Client never -- per HALCYON.md 13.7's "a session-owned empty leaf's
  claim". The placement-DoS residue this bullet warned of is closed: a
  foreign same-display peer can no longer re-mint under the restore tool's
  tokens (it does not own the leaf). See "The Session actor" note below.

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
leg; `ls-gfx-panes` (33 legs) covers the G-6 pane tree and,
since H-4b-1, the placement claim (one leaf minted twice, focus moved off
it: the stale token falls back to focus placement and never steers into
the leaf, the live token lands in it, `claim=nothex` is E_INVAL raw).
Since H-4b-2 the battery is a `Session(michael)` actor, so the whole 33/33
run also proves the Session path REGRESSION-safe (the claim leg still
mints -- the battery owns the leaf it split -- and the pane-tree negative
still refuses `michael -> SYSTEM` with E_PERM); the POSITIVE cross-process
mutual-authority witness is owed at H-4b-3's restore tool;
`ls-gfx-mode` covers the display-mode verb and its authority gate. The
`test-mode` cargo feature strips the determinism surface to `E_OPNOTSUPP`
for production, and both variants compile. The battery and its expect
scenario (`usr/tapestry-battery/src`, `tools/interactive/ls-gfx-panes.exp`)
are UNOWNED -- their reference lives in `docs/reference/139-tapestryd.md`
(the gate paragraph); the coverage sweep is
[[seam-tapestry-battery-unowned]].

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

## The placement claim -- one-shot, minted on read, spent by create (2026-09-02, H-4b-1)

HALCYON.md 13.7 makes PLACEMENT a capability: `host()` auto-splits the
FOCUSED leaf, so a layout restore -- which must land each spawned child in
the leaf the saved tree names -- cannot be built on the focus path. H-4b-1
lands that primitive alone; the authority model is untouched (the Session
actor is H-4b-2, the restore tool H-4b-3).

**The token lives on the pane, not the surface.** `Pane.claim_token:
Option<u128>` (`pane.rs`) is set only on an EMPTY leaf and cleared the
instant the leaf is hosted (`host`, `host_into`) or freed (`close`: the root
collapses to `None`, every other pane is dropped), so a claim can never
steer a surface into a leaf that already holds one -- the occupancy rule is
structural, not a branch that could be skipped.

**Mint = the offset-0 read of `pane/<id>/claim`** (`PFK_CLAIM`, the ninth
pane file; handled in `h_read` BEFORE the immutable `pane_read`, because
minting mutates). 16 CSPRNG bytes (`libthyla_rs::rand::fill_bytes`) become a
`u128`, `Layout::mint_claim` stores it (refusing a non-empty leaf itself),
and the read returns `{:032x}\n`. The text is pinned to the fid by
`read_text_snapped`, so a client reading to EOF spends ONE mint; a seeked
first read mints nothing and reads EOF; a later offset serves the pin
whatever the leaf holds by then (the emptiness gate applies at mint only).
A container is E_NOENT (no tile), an occupied leaf E_PERM (its placement is
taken), a CSPRNG failure E_IO. **Last mint wins**: a second read re-tokens
the leaf and the first token is stale.

**Spend = `create W H claim=<32 hex>`** (the surface ctl's create arm).
Syntax first: exactly 32 hex digits, and `claim=` beside any role but
content is E_INVAL for every peer -- judged BEFORE the renderer gate; past
the gate the typed `Host` enum (`Content { claim } | Chrome { bind } | Menu
| Status`, which replaced the three loose create parameters) cannot carry a
claim on a never-hosted class. At the host step, after the weave allocation
(a failed alloc spends nothing), `Layout::consume_claim` finds the EMPTY
leaf whose live token matches, clears it (one-shot), and `host_into` hosts
there -- never splitting, never moving focus (the restore tool arranges the
whole skeleton, then sets focus once). **A token naming no live empty leaf
-- stale, spent, or never minted -- FALLS BACK to `host()`**, the focus
placement, exactly as 13.7 says: the child passes an opaque cookie and must
never fail to create because its placement hint went stale under it. The
fallback is `say!`-ed, so a restore whose claims all miss shows in the log
instead of silently landing on focus. (An earlier draft failed loud with
E_INVAL; that contradicted 13.7 and would have made a ported program die on
a stale hint -- corrected before landing.)

**Who may mint, at this stage: any peer.** The mint gates on emptiness alone
-- today's empty-leaf rule (an all-empty subtree is anyone's; anyone may
`close` an empty), and a claim is strictly weaker than the close every peer
already holds over the same leaf: it can never take a tile that holds a
surface. The scripture's tightening -- the leaf must be SESSION-OWNED --
lands with `owner_principal` recorded at split (H-4b-2), when the mint
narrows to the reader's own empties. Until then the residue is a placement
DoS by a same-display peer (re-minting under the tool's tokens, or closing
its empties -- the latter possible today already), never an authority
breach.

**Witness**: the battery's placement-claim leg (ls-gfx-panes) -- one leaf
minted twice, focus moved off it, the stale token falls back (its surface
lands, never in that leaf, which stays `surface none`), the live token lands
in it; `claim=nothex` is E_INVAL raw. libtapestry gained
`Surface::open_claim[_on]` (`Mint::Claim(u128)` -> ` claim={:032x}`).

## The Session actor -- session-wide mutual pane authority (2026-09-02, H-4b-2)

The authority KEY changed. Before H-4b-2 every non-renderer peer was one
`Client(stripes)` actor, keyed on the kernel per-PROCESS tag, so two
processes of ONE user were mutually walled. That makes layout RESTORE
impossible: the restore tool and the programs it spawns are different
processes of the same user, and the tool arranges tiles the spawned programs
then occupy. H-4b-2 adds a third identity `Actor::Session(u32)` keyed on the
kernel's durable per-Proc PRINCIPAL.

**The 3-way `actor()`** (per authority write): `peer_is_renderer` ->
`Renderer` (the environment, acts anywhere); else `peer_principal` in
{SYSTEM, NONE, INVALID} -> `Client(peer_stripes)` (system daemons, the
unauthenticated, and unknown peers stay per-PROCESS, mutually walled --
deliberate: the boot chain must not collapse into one session); else
`Session(peer_principal)`. So only a real user principal earns the ratified
same-principal mutual authority (a program running as you may
close/refocus/rename/claim your OTHER tiles) -- strictly weaker than the
same-owner process kill I-26 already grants; the console (SYSTEM) and other
users (another principal) stay protected. `Conn.peer_principal` is cached ONCE
at `Conn::new`, unlike renderer-status (which `peer_is_renderer` re-reads per
write because the SAK can revoke it): a running Proc's principal is immutable.

**Ownership carries the principal.** `Surface.owner_principal` is set at mint
from the minting conn. An EMPTY leaf carries `Pane.owner_principal` too,
stamped at SPLIT from the splitting actor via `actor_owner_principal`
(Renderer and Client -> 0 = the environment, unclaimable by any user session
and untouched by the reap; Session(p) -> p). BOTH empty children of a split
are stamped, not just the new one (Fix A, `f6e306ae`): 13.7's plural "empty
leaves record an owner_principal at split", and the layout-restore precondition
-- a session building a whole tree from the environment root (owner 0) must own
EVERY leaf it builds, or it could claim only one child of each split. Bounded to
empty leaves (an occupied original keeps its surface's ownership -- the field is
inert there); an empty leaf is already anyone's to mutate (13.6), so
re-stamping one confers nothing beyond the close every peer already holds, and
it never touches a leaf hosting a surface. The three authority checks gained
`Session(p)` arms keyed on `owner_principal` (`actor_owns_subtree`: every
hosted surface == p, vacuous-true on an all-empty subtree, same as Client;
`actor_hosts`: the leaf's surface == p). `actor_names` now also lets a session
NAME an EMPTY leaf it owns -- the restore tool tags a leaf at claim time,
before its spawned child hosts a surface (acme's tag-before-`win`); an empty
leaf hosts no surface so `actor_hosts` is false, and an empty leaf's owner is
its recorded `owner_principal`, not a hosted surface.

**The claim mint is now owner-gated** (this closes the H-4b-1 seam). The
offset-0 read of `pane/<id>/claim` mints only on a leaf the reader OWNS:
Renderer anywhere / `Session(p)` iff `pane_owner_principal == p` / a Client
never (owns no empty leaf). E_PERM otherwise -- the same code as an occupied
leaf, both "this placement is not available to you". A claim stays strictly
weaker than the close every peer holds: it can never take a tile that holds a
surface.

### The creator reservation -- H-4d closes the claim race (2026-09-05, H-4d-1)

Owner-gating the claim still left a SAME-principal race: under a session the
compositor is the user's rio and fills every empty leaf it owns, while the
restore tool -- the *same* principal -- is mid-build splitting a skeleton. Last
mint wins, and no mark made AFTER the split (a tag, a claim) can close it,
because the compositor's reconcile runs per `TEV_LAYOUT`, per split. H-4d-1 marks
the leaf AT the split: `Pane.creator_conn`, stamped on BOTH empties by a ctl
split (a chord split stamps none), released at that conn's `retire_conn`. The
claim mint then answers **E_AGAIN** -- not E_PERM -- to any OTHER conn of the
same principal while the creator lives ("the leaf IS its principal's, just not
yet"); the Renderer (the environment) is never held off. **The claim-LESS create
respects the reservation too, keyed on the PROCESS** ([[chg-2026-09-06-harc-audit-close-r1]]
A-F3): `Pane.creator_peer` sits beside `creator_conn`, and `host_for(n, conn,
peer)` treats a focused empty leaf another live process reserved as occupied, so
`host()`'s focused-leaf fallback cannot take a restore tool's tagged leaf out
from under it. Keyed on the peer, not the conn, a program that splits on one conn
and hosts on another (the battery's control conn + its per-surface conns) still
fills its own leaf; the first ls-gfx-panes run caught the conn-keyed version in
the tabbed leg. The release fans ONE
`TEV_LAYOUT` to the declared session so the retry is prompt (no geometry change
at a release). rio's rule: a window a program creates is that program's, not the
menu's -- so a restore tool's skeleton is never filled by its own session
compositor mid-build. The paired decision -- a session's tagged empty leaf
becomes a terminal tile whose tag IS its command line (`kaua-term cols rows
<argv>`), the compositor being the user's rio since KT-1 -- lives on the halcyon
side (docs/reference/150/151); the tapestryd half is the reservation + the menu
authority checks' `Session(p)` arms admitting the declared session.

**The reap.** `reap_session_empties(principal)` closes a departed session's
empty scaffolding when the principal's LAST live conn is gone. `retire_conn`
(at teardown) already closed the dying conn's OCCUPIED leaves (`retire` closes
each retired surface's hosting leaf), so the reap only touches leftover empty
leaves stamped with that principal -- never another principal's tiles, never
the environment's (owner 0). The root never leaves; a reaped root-leaf is
handed back to the environment. Main's TWO conn-removal sites do the last-conn
check: `conns.remove(i)` first (so `conns` holds only the survivors), then
`conns.iter().all(|o| o.peer_principal() != gone)` -> reap. The guard rejects
the sentinels, so a system/renderer/unknown death no-ops.

REGRESSION-SAFE by construction: a single-process user is unchanged (stripes
and principal both uniquely identify it). The NEW mutual authority is first
exercised by H-4b-3's restore tool (the real second same-principal peer),
where the positive cross-process witness lands; the batched holotype over
H-4b-1..3 is at that arc close.

## The owner file + the H-4b arc audit close (2026-09-02, H-4b-3b)

H-4b-3b landed the restore TOOL (`halcyon layout restore`, in the halcyon
crate -- not this surface) and added one tapestryd file for its SAVE side:
`pane/<id>/owner` (`PFK_OWNER`, read-only, in the RO pane-file set). It reports
the tile's kernel principal -- a hosted leaf's surface `owner_principal`, or an
empty leaf's recorded `owner_principal` (0 = the environment); a container is
E_NOENT. Ungated (a principal id is no secret; the `layout` dump is already
ungated). The save tool reads it to mark each tile it must NOT respawn (the
console = SYSTEM, another user); the tool's classifier is fail-CLOSED (owner 0
and an unreadable owner are BOTH treated as the environment's).

The batched holotype over H-4b-1..3 CLOSED here: **0 P0 / 0 P1 / 0 P2 / 2 P3,
NOT dirty** (Opus fallback -- Fable was out of credits; a finished fallback
round is closed; the prosecutor re-derived every load-bearing claim from code,
and a parallel self-audit found 0). F2 [P3] (a fail-OPEN arm in the halcyon
tool's env classifier -- owner 0 treated as respawnable) was FIXED on the
halcyon side (the pure `owner_is_env`). F1 [P3] is a tapestryd-adjacent SEAM
worth recording here: **empty-leaf `owner_principal` gates the claim MINT but
NOT the structural verbs** (`close`/`split`/`move`/`mode`) -- `actor_owns_subtree`
is vacuously true on an all-empty subtree (the ratified 13.6 "an all-empty
subtree is anyone's"), so a co-resident `Session(other)` or a `Client` can
`close` an in-flight restore's empty scaffolding before it is filled. HARMLESS
at v1.0 (one session; trusted system daemons; a hostile same-user program is
`Session(self)` which owns its own empties); no escalation, no hosted-tile
degradation, no crash. The fix (block a subtree containing a FOREIGN-owned
empty leaf) refines the ratified 13.6 rule and lands with the multi-seat seam.

## Backgrounded-leaf tiling, structural transparency, and the hosting-fan defect (2026-09-05, KT-1.5d-3 F2)

**Context the dossier lacked.** Since KT-1.5d-1b a SESSION (a logged-in
principal's per-user halcyond, `Actor::Session`) taking the display
BACKGROUNDS the console renderer's leaf (aurora): no FRAME, no composite, no
CONFIGURE, no scanout. d-1b decided that from VISIBILITY (`vis -> bg_now`)
AFTER `recompute`, so the backgrounded leaf still consumed a tiling column --
a multi-tile session tiled into N+1 columns (ls-gfx-session measured two
session tiles summing ~836 of 1280 px).

**F2 (i), tiling exclusion.** `reconcile` decides backgrounding from the TREE
before `recompute` (`hosted_leaves`, owner-based, visibility-independent)
into `Pane.backgrounded` (`apply_backgrounded` clears stale flags each pass);
`layout_pane`'s Split arm zero-rects a backgrounded leaf -- KEPT visible so
the post-recompute vis/bg/scanout accounting is untouched -- and the
foreground siblings divide the full rect (all-bg container guard). A
session-less display yields the empty set: the console path is byte-identical.
Two tiles now sum 1264/1280.

**F2 (ii), structural transparency (operator-ratified).** A backgrounded leaf
is transparent to a session's structural ops: `actor_owns_subtree(Session)`
skips it -- GUARDED on >= 1 owned non-backgrounded surface, else a session
could close an aurora-only subtree; an all-empty subtree stays vacuously
owned -- and `tab_cycle` / `visible_strips` / the `layout_pane` Tab arm
(`Next::Tab`, resolved outside the `&mut` borrow) skip it: never a segment,
a cycle stop, or the shown child. THE TWO-FLAGS TRAP: the skip keys on the
LEAF tree flag (`is_bg_leaf`, via `subtree_hosted`), never on
`Surface.backgrounded`, which is visibility-derived and CLEARS the moment a
tab hides the leaf (that re-exposure E_PERM'd `tab next` until re-keyed).

**F2b, the pre-existing hosting-fan defect (found 2026-09-05).** A reconcile
is structural iff `calc_geom_sig` changed, and the signature folded only each
visible leaf's `(id, content rect)` -- WHERE, never WHAT the leaf shows. A
hosting into an ALREADY-split empty leaf (an explicit `split` then a spawn;
the H-4b `host_into` claim placement; `host()`'s focused-empty-leaf arm)
changes no rect, so the pass ran NON-structural (the focus-only chrome
branch), the CONFIGURE fan never fired, and the new surface had no standing
offer (`offered == None`): it never learned its pane size and its first
`resize` ack answered E_INVAL. Masked since G-6 because every gate hosted via
`host()`'s SPLIT arm (a new rect) and the production clients pre-size to the
pane. FIX (state-based): the signature folds the hosted surface INCARNATION
`(slot, gen)` (sentinel for empty), so hosting / unhosting / a same-slot
rehost are structural on every placement path with no per-caller poison
(the `frame_tick` `geom_sig.wrapping_add(1)` is the action-based shape this
avoids). Test-mode logs every resize-ack rejection with its discriminant and
the deciding state -- the client sees one Rwrite error; only that line
separates stale / no-offer / echo-mismatch / draining.

**Prosecution.** Any reconcile-consumed input the signature still omits (a
leaf's role/status/tab-visibility that changes what the pass must do); the
incarnation's `gen` read racing a retire inside one pass; the sentinel
`u64::MAX` colliding with a real `(slot, gen)` (slot 0xffffffff, unreachable);
the transparency guard's vacuous-empty edge; the Split arm's all-bg
fallback; `host()`'s shape-keyed split flipping SplitH once aurora is
excluded (the flat `[aurora, A, B]` root that exposed the ownership model).

**Tests.** `ls-gfx-panes` is the regression test for F2b (the battery's
explicit pre-split + B's resize leg fail 3/3 without the fold); its move leg
now orders panes by the layout text's child order, because a zero-rect leaf
cannot be ordered by rect. `ls-gfx-session` carries the F2 geometry leg (two
session tiles must sum >= 85% of the display width; witness captured THEN
spawn, in sequence -- expect matches by arm ORDER). The battery + scenarios
remain [[seam-tapestry-battery-unowned]].

## The declared seat: the display handoff as an act, the takeover rule, and the KT-1 audit's compositor fixes (2026-09-05, rounds 1-2)

**The trigger was wrong (round 1, C-F6).** d-1b backgrounded the console
renderer whenever a leaf's `owner_principal` was a real user -- and
`principal_is_session` is true for EVERY logged-in user, so any user program
that drew a window from the console shell (DOSBox, tapestry-demo, the
battery) put the console to sleep: a default-boot behaviour change shipped
under a "byte-identical console path" claim. The handoff is now an ACT of the
session compositor: `session on` on its own ctl conn before its first surface
hosts (`global_ctl`, Session-principal-gated; the renderer and every sentinel
peer are E_PERM). `Comp.session_conns` holds at most one `(conn, principal)`;
`has_session_tree` is true iff the declared conn hosts a leaf
(`hosted_leaves`, visibility-independent), and only then does `bg_tiling`
collect the sentinel-principal leaves. A user window never backgrounds
anything; a session-less display is the empty set, byte-identical.

**The seat is held while it hosts (round 2, C2-F1 + B2-F5; round 3, F6).**
Round 1 made the slot first-come with no takeover, and halcyond died on a
refusal -- so any Session conn that had written `session on` (a same-user
program; an orphan of a previous user, which the getty loop never kills) held
the seat until it died, and login's "exit is logout" turned every later
graphical login into the C-F12 re-prompt loop. Now `session on` takes the
seat over from ANY holder that hosts nothing (an idle declaration holds no
display; a crashed compositor's conn is retired -- and un-declared -- as soon
as its EOF is serviced, so a restart never needs more than halcyond's retry
budget), and a holder hosting leaves keeps it against every newcomer, the
user's own programs included (round 2 let a same-principal newcomer take a
LIVE holder over; round 3 F6 showed that steals the user's own compositor's
seat -- its mint cap drops, it hears no `TEV_LAYOUT`, and it never
re-declares -- so the exception is gone), answering E_BUSY with a log line
naming the holder. The client half: halcyond retries a refused declaration
through `DECLARE_TRIES` (25 ms apart) and then runs UNDECLARED -- its tiles
tile beside the console like any user window, the surface cap is the
ordinary one, and `session up ... (undeclared)` says so -- never exiting into
the login loop; and it re-issues `session on` once its first surface hosts,
taking THAT verdict as `declared`, so an idle re-claimer in the declare ->
first-mint window cannot leave it running undeclared while mislabelled. The declaration
clears in `retire_conn` AFTER the retire loop (C2-F2): a crash with N tiles
keeps the console backgrounded through the N-1 intermediate reconciles and
lands one transition to `Direct(console)`; the last retire already sees a
declared conn hosting nothing, so the end state is identical to `session
off`'s.

**`TEV_LAYOUT` (kind 10; round 2, B2-F6).** A structural pass whose change
touches no hosted surface -- a split of an EMPTY leaf -- fans no CONFIGURE
(nothing hosted changed size) and no FOCUS (no surface gained it), so the
conn that claims empties learned of them only at an unrelated event. The
structural branch now pushes `TEV_LAYOUT` (`value` = the layout epoch) to ONE
surface of the declared conn (`session_notify_surface`, the lowest slot it
owns; one event per change, not one per tile); it COALESCES in `push_event`
like CONFIGURE and FOCUS (the newest epoch subsumes every unread one -- it is
pushed at every structural pass whoever caused it, so a foreign client's
window churn against a momentarily non-draining session must never fill the
queue; round 3 F2); a wedged push retires that surface like any other fan
target. Session-less displays emit none.

**The compositor's other round-1 fixes.** `close` runs `actor_owns_subtree_all`
-- a Session owns a subtree for the DESTRUCTIVE verb only if every hosted
surface in it is its own, so the transparent (backgrounded, SYSTEM) console
leaf blocks a `close` of any container holding it (C-F2, a P1: the console
renderer received TEV_CLOSE and exited); `split`/`focus`/`zoom`/`tab`/`move`
keep the guarded transparency. `foreground_leaf_count` (visible AND not
backgrounded) drives the inset decision and the Direct predicate additionally
requires the lone foreground leaf's CONTENT rect to equal the display rect
(C-F3/C-F4: Direct was chosen against an inset layout because the zero-rect
leaf counted; a session going 2 -> 1 kept a ring). `is_bg_subtree` (a leaf:
its flag; a container: non-empty and all children bg) replaces the one-level
`is_bg_leaf` at every child filter (C-F8). `Surface.backgrounded` is DERIVED
from the tree flag each pass (`bg_now` = the surfaces of `bg_tiling` leaves;
C-F10 -- the two-flags trap the F2 section names is dissolved). A declared
conn mints up to `MAX_SURFACES_PER_RENDERER`, the pool grew by `2 *
MAX_PANES` so the bound is exact (C-F5: the 4-surface cap dead-keyboarded the
fifth tile). `exec_chord` stamps a chord-split leaf with the focused leaf's
owner only when that is a real Session principal, else 0 (C-F9);
`surface_target` is None for an empty content rect (C-F13).

**Prosecution.** The declaration's authority (Session-principal only; `off`
clears only the caller's entry; a repeat `on` is idempotent); the takeover
rule's two arms vs a holder mid-teardown (its surfaces retiring while the
newcomer declares -- `conn_hosts` reads the live tree); the intermediate
retire passes with the conn still declared (a wedge-retire re-entering
`reconcile`); `TEV_LAYOUT`'s target when the declared conn's lowest slot is a
surface being retired in the same pass; `actor_owns_subtree_all`'s vacuous
all-empty subtree (the H-4b F1 deviation, unchanged); the Direct conjunct vs
the status carve; `session_conns` never exceeding one entry on any path.

**Tests.** ls-gfx-panes: the undeclared control (the console leaf keeps a real
column while a user client hosts two surfaces) and the declared control (the
battery's own conn writes `session on`: the same leaf reads the ZERO rect, a
`close` on the container holding it answers E_PERM with the leaf undisturbed,
`session off` restores its column) -- the one-variable pair. ls-gfx-session:
`session declared by conn N` precedes the first present; caps-probe; the zoom
survival, lone-tile and 1264/1280 geometry legs. Unconstructed: the takeover
arms (a second conn of one principal; a foreign holder), a crash with >= 2
tiles, a Tab container of only backgrounded leaves.

## The PRESENTABLE -- a display object the server never maps (2026-08-26, Warp-WSI W-3c-1)

The vkQuake/WSI arc's first new object class on the warp seam: a venus
swapchain image registered for the compositor to scan out, NOT for the guest
to draw into. `ctx/<id>/img/{new,<handle>/{info,ctl}}` -- `new` takes
`<handle> <w> <h> <format> <stride> <mem_id>`, validates the display shape
against the compositor's accept set (stage 0: B8G8R8A8 only), and registers a
`WarpImg`; `info` reports the accepted shape + `bound` + `mem`; `ctl` takes
`destroy` only. `create_presentable` (gpu.rs) mints it as a HOST3D blob --
AMENDED by measurement at W-3c-1 round 2: virglrenderer REFUSES
`USE_SHAREABLE` on a HOST3D blob, so the mint is `USE_MAPPABLE` and the
guest-invisibility comes from the server NEVER MAPPING it, not from a share
flag. `set_scanout_blob` (gpu.rs, the verdict wrapper over the W-3a raw-resp
probe fn -- one wire implementation, the #230 by-meaning rule) binds it.

**The class is defined as much by what it LACKS.** No guest mapping, hence no
weft share, no hostmem offset, no reclaim park, no #847 dual count. Its I-7
lifetime hazard runs the OTHER way from a mapped BO's: the DISPLAY holds the
reference, so a retire must not race the scanout. `wimg_teardown` is therefore
ORDERED -- unbind (via the existing `gl_evict_res`, reused not
re-implemented) BEFORE `resource_unref`. That order is `specs/tapestry_present.tla`'s
W-3b presentable-class `PUnbound` conjunct as code, whose `buggy_punbind_skipped`
cfg proves the omission violates `NoTornPresentable`. The unbind issues
UNCONDITIONALLY, never gated on the per-object `bound` flag: `gl_evict_res`
self-guards on the authoritative `Comp.bound_res`, so a redundant no-op is
free while trusting a second copy of the fact costs the display (the
#230/#847 by-meaning discipline).

`MAX_WARP_IMGS_PER_CTX` = 16, folded into `ctx_backing_total` so the I-32
holistic cap covers the class; `WARP_IMG` = `1 << 45`, a new qid tag under the
existing 8-way disjointness `assert!` (WARP_CTX/BO/RING/MEM/IMG + SURF + PANE
+ FLAG). `warp_img_selftest` proves it with four arms -- `shape=` (three
refusals one variable away, the accept-set discriminator), `mint=`, `bind=`,
`unbind=` (the ORDERING witness: destroy WHILE BOUND, observe `bound_res` back
to 0 -- the modeled bug's ABSENCE, not a generic teardown success). The
AUDIT-TRIGGERS row the W-3a closer deferred to this chunk was added here.

## The generalized present source -- Bo | Img (2026-08-31, Warp-WSI W-3c-2)

W-3c-2 gave present-to two source families. Context (JOURNAL run 6): the
run-5 "a presentable is not blittable" measurement was of the blob_id=0
STAND-IN class (an SHM fd, categorically untypeable by vrend's
`pipe_resource_set_type`, which takes DMABUF only -- a blit on it raises
ILLEGAL_RESOURCE -> `ctx->in_error`, the measured `compose=noreadback`). The
REAL class (blob_id names a VkDeviceMemory) has virglrenderer's designed
cross-context path; the composed arm is resequenced to W-3d and NO scripture
was narrowed.

The mechanism: `enum PresentSrc { Bo(u32), Img(u32) }`; `WarpCtx.present_to`
becomes `Option<(slot, gen, PresentSrc)>`. BOTH families are PUB-keyed -- the
verb resolves an img HANDLE to its pub id at consent time, so a freed handle's
later tenant can never inherit a consent (pub ids monotonic, never reused --
the pin `gen` gives the surface half). `enum AdoptSrc { Bo, Img{stride} }` +
`GlAdopt.kind`; `gl_adoption` gains the img arm (geometry vs the CURRENT
surface incarnation -- the display-MODE half of the accept set, discharged
per-use where the bind is chosen). `Comp::direct_bind_adopted(g, w, h)` is ONE
copy of the family dispatch (SET_SCANOUT for Bo, SET_SCANOUT_BLOB at the
declared shape for Img -- the spec's `PPresentBind`; a post-bind full flush
per #57 on both).

**Every composed-machinery consumer is HARD-GATED to `AdoptSrc::Bo`, and the
gate is MEMORY SAFETY, not sequencing.** `comp_rb_pump` (its readback DMA
writes into `g.va`; an img adoption's va is 0), `comp_readback_retired`'s
`same_adoption` (plus a KIND PIN: img and bo pub sequences are independent, so
a bare pub compare could false-match across families after the consent
changed), and the composed present arm. `comp_import_bo` /
`comp_release_bo` / `comp_replay_deferred_imports` are Bo-only (an img consent
imports nothing until the W-3d compose arm -- no compositor-side
representation to witness yet). `wimg_destroy` gains the consent-clear arm
(clear `present_to` + `res_stale` + `gl_retarget` BEFORE the take+teardown). A
composed-mode surface whose consent names an img is LOUD once per ctx
(`note_img_composed_deferred`) and the pane shows its own 2D weave until W-3d.

**Seam (owed, load-bearing).** `wimg_teardown`'s `PDrained` conjunct: the
Direct adoption creates NO pinflight member (the standing binding is tracked
by `Comp.bound_res`, completed inside one dispatch). THE W-3d COMPOSE ARM IS
THE FIRST PINFLIGHT PRODUCER AND MUST LAND THE DRAIN IN `wimg_teardown` IN THE
SAME COMMIT (`tapestry_present_buggy_pdrain_skipped.cfg` is the counterexample;
a green suite between the two proves nothing). Constraint carried for the
compose arm: vkr's `mem->exported` is ONE-SHOT (a memory exports once), so one
blob mint per VkDeviceMemory -- the registration must adopt, or the map and
present paths coordinate on a single mint.

## The W-4 present windows: the text pin, the double-paint census, the latency instrument (2026-08-31, Warp-4)

Mechanisms server.rs grew across the W-4 comparison arc (which INVERTED the GL
vs VK verdict: GL 44.8 / VK-linear 47.6 / VK-blit 51.3 fps).

- **`text_snaps: Vec<(fid, gen, bytes)>` -- a per-fid generation pin for the
  REGENERATING text files** (P_CTL / P_LAYOUT / W_CTL). An offset-0 read
  snapshots the composed text on the Conn; later offsets serve the pin, so one
  open reads one generation (the r7-F6 splice fix -- a naive re-compose per
  read tore a multi-read consumer). Cleared at `fid_clunk`, `teardown`, AND
  `drop_all_fids` (the r8-F1 sibling-omission find: the third clear site the
  first two implied).
- **The double-paint fix (the PokeBind/PokeFlush census).**
  `direct_bind_adopted` flushes INTERNALLY on success, so `img_poke_complete`'s
  rotated-poke arm must NOT flush again. The `Cost` census splits it:
  `PokeBind` = the WHOLE rotated paint (set_scanout + internal flush),
  `PokeFlush` = same-image re-pokes ONLY. Under a rotating swapchain the steady
  state is ALL PokeBind. `img_poke_complete` calls `release_displaced_gen`
  (the retired generation's resources) then charges the right census.
- **The latency instrument.** `poke_hist_bind[8]` / `poke_hist_flush[8]`,
  buckets `<2 <5 <8 <11 <14 <20 <30 >=30` ms, read as two tctl rows -- the
  instrument that proved the ~10 ms host pacing quantization (0/3796 steps
  under 8 ms, run 5). A measured number, with its lane named.
- **`warp_stall_watch`** -- one warp-watch line per live ctx carrying the
  identity fields `conn=` / `surf=` (surf = the surface whose `gl_src` names
  the ctx, `-` if none; an orphan reports `surf=-`), plus `pass`/`fparked`/
  `rparked`/`inflight`/`sig`/`rep`/`again`/timeline/`poisoned`. The mint say
  carries `conn=selftest` for the u64::MAX self-test conn.
- **The hostmem budget split.** `ctx_guest_backing` / `ctx_hostmem_backing`
  are separate axes (the round-7 F4 correction: hostmem has its own
  `WARP_CTX_HOSTMEM_MAX` bound, not the guest cap).

## The menu -- the one ephemeral surface the compositor grabs and tears down (2026-09-02, H-3c THE GATE + its audit close)

The obj verb menu: a `Role::Menu` surface (`create W H role=menu`, no bind,
NEVER hosted, never focusable). The create and the `menu ` verbs are the
renderer's OR -- since H-4d -- the declared session compositor's, gated
`session_declared && conn_hosts` ([[chg-2026-09-06-harc-audit-close-r1]] A-F5):
the user's rio summons the menu over its own tiles, but an idle declarer that
hosts nothing is refused (else it could float a menu, take the grab, and force
Composed with no tile of its own). `Comp.menu:
Option<MenuState { n, gen, rect }>` is the ONE placed menu. Gated global verbs:
`menu place <surface-id> <x> <y>` (authority -> syntax -> a non-menu surface
E_NOENT -> owned by the caller's PROCESS via `owner_peer == peer_stripes`
E_PERM; clamp; replace; forces Composed; redraw CONFIGURE) and `menu dismiss`.

**THE GRAB.** `key_event` (Esc press = compositor dismiss + swallow the
release/repeats), `ptr_route` (MOVE/SCROLL menu-relative), `ptr_btn` (press
outside = click-away: dismiss, press AND release swallowed; the no-menu arm =
click-to-focus on a focusable unfocused hosted leaf, press passed through),
`chord_key` (dismiss before `chord_action`). The swallow bookkeeping is the
audit-close restructure: `Comp.menu_swallow_btn` is GONE, replaced by
`key_owner: [u64; KEYCODE_SPAN]` and `btn_owner: [u64; BTNCODE_SPAN]` (packed
`slot+1 | gen<<16`; `OWNER_SWALLOWED` = 0xffff) -- a press records its target
(`owner_pack`), a release/repeat follows it iff the gen matches, drops if
retired, live-routes only when unrecorded. `chord_down` widened to
`[u64; KEYCODE_SPAN/64]` (was `[u64;4]` & 0xff -- aliased codes >= 256).
`ptr_btn`'s click-away marks `OWNER_SWALLOWED` AFTER `menu_dismiss` (the old
order let retire's arm clear the record -> the release leaked).

**COMPOSITOR-OWNED DISMISS** = `retire`'s menu arm (unplace first,
`menu N dismissed (<reason>)`, `menu_heal` at the tail), reached by EVERY path
including ctl `destroy` / `retire_conn` / WEDGE. `menu_heal` targets the
intersection: `paint_borders(false)` + strip intersections pushed, tag-bar
headers + empty-leaf BG_COLOR filled (`placement_rect` = the crop),
same-size CONFIGURE to intersecting hosted + `visible_chrome` surfaces.
`menu_reassert` composes each `shown_slot` over any screen write under the
menu (`screen_push` before upload, `screen_flush_rect`/`_full` after);
`reconcile`'s structural repaint runs `prefill_from_shown()` after
`paint_chrome()` (every visible hosted surface's `shown_slot` composed;
GL adoptions and held slots skipped), and its Off/Direct `want` arms gained
`&& self.menu.is_none()`. The `ctl` read reports `menu none | n x y w h`.
Closed list: `memory/audit_h3c_closed_list.md` (0/1/0/5 + 5 self-found, DIRTY).

## fid_clunk: the minted-never-created surface reaped on its ctl clunk (2026-09-02, H-3c-2 audit close)

`fid_clunk(&mut self, comp, fid)` (was fid-only; `h_clunk` threads `comp`).
After the fid + its held replies drop, if the fid was a surface CTL fid
(`is_surf && surf_fk == FK_CTL`) whose surface is owned by THIS conn at THIS
gen and still `SurfState::Minted` (created server-side by the `surface/new`
mint but never `create`d), and NO OTHER fid of this conn names the same path,
the surface is retired (`comp.retire(n)`, say `surface N minted, never
created, its ctl clunked: retired`). The client-side twin is libtapestry's
`fail_created` (it says `destroy` on a post-mint failure); this is the
compositor's backstop for a client that clunks without it. The three text-pin
clears (`fid_clunk` retain, `teardown`, `drop_all_fids`) live alongside.

## The status bar -- the display carves a strip (2026-09-02, H-3d)

`Role::Status` (a surface role only). `Surface.is_status`; `Comp.status:
Option<StatusState { n, gen }>`. `create(.., is_status)` refuses (E_INVAL,
BEFORE the weave allocation) when a bar exists or `w != disp_w || h !=
status_h || disp_h <= status_h`; on success registers `Comp.status`, says
`status bar N created (WxH); the display carves H`, `reconcile()`. The ctl
parse: `role=status`, no bind, renderer-gated.

`status_rect() -> Option<Rect>` = `{0, dh - status_h, dw, status_h}` while a
bar is registered and the display is taller than the unit; `surface_target`'s
status arm (gen-pinned) resolves to it; `visible_chrome` / `compose_geometry`
classify `is_status` as chrome. **`reconcile`: `layout_h = dh - status_h`
while a bar exists is passed to `recompute` ONLY** -- every other height use
stays the DISPLAY's; shadowing `dh` bound the scanout at 1280x780 (the found
bug). The Off/Direct `want` arms require `status.is_none()`. `paint_borders`
fills the strip `status_bg` (Daylight token 6) at structural repaints;
`retire`'s status arm clears `Comp.status` before the reconcile (say `status
bar N retired; the display returns`). The global `statusbar` file
(`P_STATUSBAR` = 6; walk/readdir/read) reports "x y w h", zeros with no bar.
The halcyond-side chrome (`status.rs`/`statusset.rs`, OSC 7 + the cmd mark)
and libutopia's `cwd_report`/`mark_cmd` are not vault-owned here.

## The #56 patchwork latch re-keyed on slot rotation (2026-09-05, the fullscreen-zoom fix)

The operator's Cmd+F report (aux 0048), run to ground at `adt-zoom-r1`
([[fnd-zoom-r1-f1]], P1). The #56 patchwork latch keyed on damage COVERAGE --
`if !rects_cover_full(&rects, w, h) { s.patchwork = true }` (one-way) -- as a
PROXY for the property it exists for: a client that ROTATES weave slots leaves
a slot stale outside its damage, so scaling one slot composes half-stale
frames. Aurora satisfies both. DOSBox-X (an SDL single-slot client: slot 0 IS
its framebuffer, complete by construction) presents partial rects as a matter
of course (the menu bar, the four overscan borders, changed scanline bands),
satisfied only the proxy, and was latched an accumulator -> `placement_rect`
CROPPED it at the content origin instead of letterboxing: native at the pane's
top-left in its tile, native at the display corner on black when zoomed. The
class is [[haz-latch-keyed-on-proxy]] -- a latch keyed on a proxy fires on every
class the proxy covers and the property does not, permanently (the latch is
one-way), and the doc even named the exemption ("the SDL class never latches")
the predicate never checked.

The fix keys the latch on the PROPERTY, observed directly: `Surface.slots_
presented` is a bitmask of the slots ever presented (surviving reweaves, never
cleared), and the latch trips only on partial damage AND `slots_presented`
naming two or more (aurora latches at exactly the same present as before -- its
second present is always a second slot; a single-slot client never trips). It
says `surface N patchwork latched (...)` once when it does. Because a
letterboxed compose then serves partial presents, each partial present redraws
only its damage's PROJECTION through the scale rather than the whole scaled
rect per rect (a 70 Hz cursor blink, or DOSBox's four overscan rects, would
otherwise rescale a display-sized rect per rect): `ComposeOp.clip` carries the
projection (`libhalcyon::place::scaled_clip`, host-tested against
`nearest_src`, the exact mapping `compose_cpu` samples by, so a clipped compose
is pixel-identical to a whole one with no seam); `compose_cpu` composes and
pushes only `op.clip.intersect(op.dst)`, while the GPU path keeps its whole-op
blit. `letterbox` moved to `libhalcyon::place` so the battery's sample points
derive from the compositor's own function ([[sub-libhalcyon]]); the client-side
single-slot declaration is `Surface::set_single_slot` ([[sub-libtapestry]]).
FIT vs FILL was not a fork -- aspect-fit is the existing letterbox policy
(640x417 -> 1227x800, 26 px pillars).

**Prosecution.** The bitmask surviving a reweave (a resized surface keeps its
rotation history, so a rotating client re-latches correctly after a resize);
the one-way latch vs a client that presents one slot then rotates (it latches
at the first second-slot partial present, not before); the clip's pixel-identity
to a whole compose (the `nearest_src` host test); the GPU path unchanged (whole
blit). **Tests.** `ls-gfx-panes` `singleslot`: a single-slot client
(`set_single_slot` + thyla_tap's discipline) zoomed shows the compositor's own
`letterbox 640x400 -> 1280x800 @(0,0)` line (the latch line a FAIL arm), the
partial present's pixel through the 2x scale, the untouched frame's pixel at
three-quarters (black under the bug); then the one-variable control (rotation
on -> the second partial present latches at `slot 1 of slots 0b11`). The
real-DOSBox re-run (aux's `dx-fullscreen-repro.exp`, the fixture on aux-3) is
owed to aux after the merge. Landed `f25781ad` ([[chg-2026-09-05-fullscreen-zoom]]); the prosecution notes
rode AUDIT-TRIGGERS row 42 to the next tapestryd round -- the H-arc round-1 audit
below, which discharged the deferral (A-F1 + A-F4).

## The H-arc audit close, round 1 (2026-09-06)

The batched H-arc round-1 audit ([[adt-harc-r1]]: three Fable 5.1 prosecutors in
parallel over the zoom fix + H-4c + H-4d-1 + H-4d-2a/2/3, `839a966f`; 0 P0 / 2 P1
/ 0 P2 / 11 P3, clean by count, every finding fixed at the close) landed six
compositor findings on this surface. The zoom section above anticipated this
round (its "next tapestryd round" deferral); A-F1 + A-F4 discharge it.

**A-F1 [P1] -- the composed GPU arm serves the letterbox re-key's partial
presents** ([[fnd-harc-r1-a1]]). `Surface.res_stale[WEAVE_SLOTS]` marks a slot
whose host copy never received the full frame (a fresh generation, a hide, a
CPU-arm present, a GL adoption, a failed compose). Once the #56 re-key made the
letterbox arm serve a single-slot client's PARTIAL presents, the composed GPU arm
blitted a stale slot WHOLE by a scaled op -- compositing bytes no present carried
(the witness tokens, the pre-hide frame, undefined texture). The fix mirrors the
direct arm: a stale slot's first transfer expands to the FULL surface
(`vec![(0,0,w,h)]`), not the damage rects; and the slot un-stales after ANY
successful GPU transfer whatever the coverage -- the second half, because keying
the un-stale on `full` would re-mark every partial present and fire the expansion
on every subsequent one. A failed compose re-marks stale (the host copy then
holds only that present's partial damage).

**A-F2 [P1] -- the draining resize-ack re-offer** ([[fnd-harc-r1-a2]], the
mechanism recorded in "The generation fence" above). Pre-existing since G-6b: a
resize-ack arriving while a reweave still drains is refused `E_AGAIN`, and a
client that only drains-and-acks-the-newest never re-acked it, so the surface
never learned its new size. `Surface.ack_deferred` latches the refusal;
`release_displaced_gen` re-offers the standing configure under a fresh serial when
the drain completes (test-mode `resize-ack N re-offer WxH after the drain`; a
wedged re-offer retires the surface).

**A-F3 [P3] -- the creator reservation keys on the PROCESS** (recorded at "The
creator reservation" above). `Pane.creator_peer` beside `creator_conn`;
`host_for(n, conn, peer)` treats a focused empty leaf another live process
reserved as occupied for a claim-LESS create, so `host()`'s focused-leaf fallback
does not take a restore tool's tagged leaf out from under it.

**A-F4 [P3] -- floor the bars at the latch flip.** When the #56 latch flips
(letterbox -> crop) under a Composed display with no structural pass to repaint
the pane, the first frame's scaled projection outside the native rect would
persist until the next structural repaint. `floor_bars_around(n)` fills the four
bands around the surface's current placement with `BG_COLOR` and flushes (a no-op
off Composed, when hidden, or with no screen buffer).

**A-F5 [P3] -- the menu seat requires hosting** (recorded at "The menu" above).
`role=menu` and the `menu ` verbs are the renderer's OR the declared session
compositor's, gated `session_declared && conn_hosts`: an idle declarer that hosts
nothing is refused, so it cannot float a menu, take the grab, and force Composed
with no tile of its own.

**A-F6 [P3] -- the test coverage.** `ls-gfx-panes` gained scenario 2a (the
draining re-offer: an ack mid-drain -> `E_AGAIN` -> the server re-offers after the
drain) and a partial-FIRST single-slot client E (its first present is partial, so
the slot is stale -- the witness for A-F1's full-surface expansion). The battery +
scenarios stay [[seam-tapestry-battery-unowned]].

OWED (from the peer close): the GPU-path witness for A-F1 on the GL host, and
aux's real-DOSBox-X re-run. Folded from [[chg-2026-09-06-harc-audit-close-r1]]
(the KT-1 inheritance: the peer's `no-dossier-change` deferred the vault prose to
this track; the UI + beacon-relay half landed in [[chg-2026-09-06-harc-r1-fold-ui]]).

## The delivered close chord -- Super+Q asks before it acts (2026-09-15, I-7b)

`ChordAction::Close` is no longer performed here. `exec_chord` DELIVERS it to
the registered rail's owner (`deliver_chord(3, id)`, the [[sub-libtapestry]]
`TEV_CHORD` kind) carrying the FOCUSED PANE's id in `value` -- only the
environment knows whether that tile has a job running, and only it owns the
HALCYON-INSTRUMENT section 14.5 dialog that asks. The owner then closes by verb
under its own authority, WITHOUT section 6.5's final-tile protection, which
Super+Q deliberately does not carry: it is the structural act, and the only
reading under which a pane holding a retained tile can be removed at all.

The id rides in `value` rather than being re-derived by the owner because the
owner's view of focus comes from the `layout` file it may have read a wake ago;
the compositor's is authoritative at the instant the chord fires.

**`deliver_chord` now REPORTS whether the owner actually has the chord**, and
the close arm is the only caller that reads the answer. With no rail -- the
legacy profile, or a seat whose rail is not up -- or onto a rail whose event
queue was too full to take it (which retires the rail), the compositor closes
the pane itself, exactly as it did before I-7b, so the chord can never degrade
into a no-op. The picker (1) and help (2) chords have no such fallback BY
DESIGN: both live only in the environment, so a false there is said and
dropped, unchanged.

Ground truth: `usr/tapestryd/src/server.rs` (`exec_chord`'s `Close` arm and
`deliver_chord`); the code vocabulary is pinned in the `TEV_CHORD` contract
comment in `usr/lib/libtapestry/src/lib.rs`. Witnessed in-guest by
`ls-halcyon-session-instrument`'s I-7b legs -- `tapestryd: chord close -> rail
owner`, then the confirmation dialog, then a Cancel that keeps both the tile
and its job.


## The pointer path's witnesses -- a lost press could not be told from a swallowed one (2026-09-15, the I-6 hunt)

`ptr_btn` had one say on its way in (`ptr btn ... -> chrome`, and only for a
chrome or rail target) and none at all on the arms that decline to act. The
`code != BTN_LEFT || drag.is_some()` swallow returned silently; a `track_at`
MISS fell through to the general routing silently; and an event that never
reached the compositor is silent by definition. `hover_update` compounded it:
its witness was gated on `now`, so a pointer LEAVING a track said nothing
either, and the motion after a press was as quiet as the press.

The consequence is a documentation-worthy property of this service rather than
a mere gap: **three different faults produced a byte-identical log.** Four
successive readings of the I-6 divider stall (the compositor emitting a hover
say and then no `drag start` for the press that followed) were each refuted by
ground truth without any of them being separable from the others -- not for
want of reasoning, but because no arm of the press path could report. A
hypothesis set that cannot be discriminated by the instrument is not a
reasoning problem.

**The witnesses now in the tree** are `#[cfg(feature = "test-mode")]` only; no
production arm changed. `tapestryd: ptr btn code C P at X,Y drag D menu M
track T` sits at the TOP of `ptr_btn`, before any routing decision: absent, the
event never reached the compositor; present, the fault is past that line and
`drag`/`menu`/`track` name the arm that took it. `divider press swallowed
(btn C drag D)` speaks for the formerly silent swallow. `divider hover left at
X,Y` speaks for the crossing off a track.

**The eventq low-water mark** is read at the top of `InputDev::drain`, BEFORE
the recycle and BEFORE the nothing-new early return, because the device can
only deliver into descriptors this driver has published and it has not yet
consumed: `avail_idx - cur_used` is exactly what the transport had to work with
while the serve loop was away, and `QUEUE_SIZE` is 16. At zero it had none, and
an event it could not place did not arrive -- a guest-side fact that holds
whatever the transport chooses to do with such an event.

**The slow-pass say** belongs with it because of a property of this loop worth
stating plainly: the input devices are POLL-MODE and are NOT in the pollfd set,
so nothing about an arriving event wakes the serve loop. The drain interval IS
the pass period -- bounded by the frame tick, so <= ~67 ms at `IDLE_HZ` -- and a
HELD pass is therefore the only condition under which a 16-deep eventq can run
out of descriptors between drains. `serve pass took N ms` names such a pass.

**Calibration (the control that makes a future firing mean something).** On a
healthy run -- `ls-halcyon-instrument` PASS, 42 legs, 89 s -- `ptr btn code`
fired 32 times while `input eventq LOW`, `serve pass took` and `gpu command
never retired` fired ZERO times. Those three are nowhere near their thresholds
in normal operation, so a future occurrence is signal rather than noise. The
healthy shape of the leg that has been failing reads:

    divider hover pane 2 track 0 at 994,404
    ptr btn code 272 1 at 994,404 drag 0 menu 0 track 1
    divider drag start pane 2 track 0 at 994,404
    divider drag end pane 2 track 0 esc -> 687:580

Reading the next occurrence: no `ptr btn` line means the event never arrived,
and the eventq/slow-pass says then state whether a held pass starved the ring;
a `ptr btn` line carrying `track 0` means the press arrived and `track_at`
disagreed with the hover that had just fired; `drag 1` means a stale drag
swallowed it.

Ground truth: `usr/tapestryd/src/server.rs` (`ptr_btn`, `hover_update`),
`usr/tapestryd/src/input.rs` (`drain`), `usr/tapestryd/src/main.rs` (the serve
loop's pass clock). The defect these were built for is OPEN and intermittent;
the green run above is a verdict, not a diagnosis.

## The live workspaces -- one root each, and the traversals that had to be re-judged (2026-09-15, W-1a)

`Layout` grew `workspaces: Vec<Workspace>` (a live root plus a remembered
focus each) and `active`. The notable choice is what did NOT happen: `root`
was not kept as a stored field synced to `workspaces[active].root`. It became
an ACCESSOR and the field was deleted, because the one failure a switch must
not have is leaving a stale root behind, and a value that is never copied
cannot go stale. Deleting the field also handed the blast radius to the
compiler, which is why this is worth recording: a grep for `.root` had
reported thirty-odd sites, but it MISSED line-broken calls (rustfmt splits
`self.panes` from `.iter()`) and mis-attributed three `Conn.root` hits on an
unrelated type. The compiler found exactly 34 in the lib and exactly ONE in
the server.

**What actually had to change, and what came free.** `recompute`'s first pass
already marks every pane hidden and zero-rects it before walking from the
root, so an inactive workspace goes dark with no new code -- and because that
pass also clears `dividers`, `track_at` cannot match an inactive track either.
The real work was the d-1b dormancy predicate: `apply_backgrounded` now also
stamps every inactive root's subtree. It is stamped THERE rather than by the
caller in `reconcile` because only the tree knows its own roots; a
caller-supplied set would be a second copy able to drift.

**The traversals that are safe only by construction.** Most whole-pool scans
filter on `visible`, which pass 1 clears for inactive roots -- `visible_strips`,
`visible_leaf_count`, `foreground_leaf_count`, `visible_hosted`, and
`neighbor_dir` via `live_ids`. That is a real invariant and should be stated
rather than rediscovered: *an inactive root's panes are invisible and
zero-rect after every recompute.* The scans that do NOT filter on visibility
are the ones that needed judging one at a time: `hosted_leaves` (feeds the
d-1b session test, correctly spanning workspaces), `find_hosting` /
`surface_at` / `find_claim` (a surface lives in exactly one leaf, so global is
right), `live_ids` (the 9P `pane/` listing and `ctl`'s `panes` count stay
GLOBAL on purpose -- those are resource and addressability facts, while the
`layout` dump is the active root's; do not later "reconcile" the two), and
`slot_of_id`, which is where the bug was.

**Two defects this found.** The zoom resolves by id through the global
`slot_of_id`, and `recompute` never checked the target belonged to the active
root -- so a zoom made in one workspace would still match after a switch and
fill the display with another workspace's pane. Guarded by `in_active_root`,
and the switch clears the zoom as well, since `zoomed_id` is one field and a
carried-over id would put a number in the `layout` header that the carve
refuses to honour. The worse one: `close_inner`'s root arm freed the WHOLE
pane pool, commented "the subtree was the whole tree" -- true with one root,
and with nine it annihilates every other workspace and leaves `workspaces`
pointing at freed slots. It now frees only that root's descendants, reading
the children BEFORE the kind is replaced. The regression test is
sabotage-measured in both directions.

**The channel and the bound.** The `layout` header carries `workspaces N
active K`, ONE-BASED to match the pane ids beside it and the `01`..`09` the
rail paints; the per-pane rows stay the active root's, so the H-4b file-walk
is unchanged. `MAX_WORKSPACES` = 9 PARTITIONS the existing `MAX_PANES` = 32
pool -- workspaces add no resource ceiling, which is the honest I-32 story.

Ground truth: `usr/tapestryd/src/pane.rs`, `usr/tapestryd/src/chords.rs` (the
eighteen chords; `action_of` PARSES `workspace-N` / `move-to-N` rather than
listing eighteen arms, so the render and parse directions cannot drift), and
the two `exec_chord` arms in `server.rs`. Host-tested at 48 (41 before).
The `workspace N` ctl verb and the battery leg landed in W-1b, below.

## The workspace verb -- two homes, and why the second one is right (2026-09-15, W-1b then W-2b)

W-1a gave the tree live roots but only one driver: `Super+N` on the
compositor's own key path, intercepted ABOVE the event stream. A 9P client
cannot inject that, so the acceptance battery had no way to exercise a switch.
The verb exists to be that driver. **It was built twice, and the second
placement is the one to read.**

**W-1b put it on `ctl`, reasoning from the design's phrase "the seat class,
like `scale`".** Two defects followed, both found by the gate rather than by
the compiler or the host suite (`server.rs` carries no inline tests). First,
`global_ctl`'s cfg-3 apply-authority gate is DEFAULT-DENY with one
hand-written conjunct per exemption, and the new verb had none -- so it was
reachable by the RENDERER ALONE and the declared session compositor could not
switch at all. Second, once a conjunct was added the leg still failed
identically: every conjunct is `conn_id`-scoped, and the battery holds TWO
sessions (its own driver session plus libtapestry's per-client ring), so
declaring on one and acting on the other is correctly refused. Both failures
presented as the same flat `rc -1`, because the harness's raw write path
collapses every errno.

**The lesson that survives from W-1b, because it generalizes:** in that
handler authority comes BEFORE syntax, so reading a verb arm's body proves
nothing about whether the verb is gated. `scale`'s arm carries only
`layout_verb_budget()` -- not because it is ungated, but because its authority
was decided thirty lines above it.

**W-2b moved it to the `layout` file, authorized by PRINCIPAL**
(operator-ratified after an architecture review). `actor_may_switch` admits
`Renderer`, or a `Session(p)` where p owns a hosted surface anywhere in the
tree; `Actor::Client` is refused. Four things drive that, and they are worth
keeping because the surface-level analogy to `scale` is genuinely seductive:

1. **The reader and the writer must be the same file.** The `workspaces N
   active K` header is rendered on `layout`, and halcyond parses it from
   there. A verb on `ctl` changing state that only `layout` reports is the
   split a 9P-heritage system exists to avoid.
2. **`zoom` is the controlling precedent.** It has a workspace switch's exact
   blast radius -- one leaf fills the display, every other tile vanishes --
   and it is authorized on `layout` by owning ONE tile.
3. **The session model already grants strictly more.** A same-principal
   program may `close` the user's tiles: destructive and irreversible.
   Refusing it a reversible view switch is non-monotonic in blast radius.
4. **The `scale`/`theme` seat gate does not transfer.** Those are seat-scoped
   because two painters -- the compositor's chrome and the session's content
   -- must agree on one rendering contract, so exactly one party may decide
   it. There is no second painter for which workspace is shown: the tree is
   the compositor's alone and every client re-reads `layout`.

Principal scoping also dissolves W-1b's second defect by construction: a
client with several conns has one principal, so declaration and act can no
longer disagree.

**The bound and the dispositions.** ONE-BASED at the door (`0` or
`> MAX_WORKSPACES` is `E_INVAL`); switching to the active workspace is
idempotent, not an error; and a SKIPPED number or an exhausted pane table is
the TREE's refusal, returned as `E_INVAL` rather than a silent success that
would leave the caller believing it had switched. Syntax first, then authority
-- this file's convention, and the inverse of `ctl`'s.

**The witness is tile PRESENCE, not a pixel.** The `layout` per-pane rows are
the active root's, so a live tile must LEAVE those rows while another
workspace is up and come back when its own returns. The battery asserts the
header at each step (`1 1` -> `2 2` -> `1 1`), the dormant tile's absence in
between, the refusal of a skipped number, and the vanish of the empty
workspace on the return. Gated: `ls-gfx-panes` PASS 48 s, one attempt.

**What the E2E CANNOT witness, stated so it is not mistaken for coverage:**
the authority axis. Refusal needs either a non-session principal or a session
hosting nothing, and the battery is michael and hosts a tile -- it can
construct neither. W-1b's "an undeclared client is refused" control was
DELETED rather than left to pass for the wrong reason.

Ground truth: `layout_cmd`'s `workspace` arm and `actor_may_switch` in
`usr/tapestryd/src/server.rs`; the leg in `usr/tapestry-battery/src/main.rs`;
the expect arms in `tools/interactive/ls-gfx-panes.exp`; and `halcyon
workspace <n>` in `usr/halcyon/src/{lib,main}.rs`, which needed no new channel
because the tool's own `/srv/tapestry` conn is already `Session(principal)`.

## The workspace round: one P0 and the guard that was only ever in the carve (2026-09-15, HALCYON-WORKSPACES round 1)

The first adversarial round over the whole W arc closed **DIRTY -- 1 P0 / 3 P1
/ 2 P2 / 2 P3**. Every finding below is fixed and sabotage-measured in both
directions (each fix reverted alone, its test run, the file restored and
md5-verified). The round ran on the **Opus-5 fallback tier**: Fable 5.1 died on
its first call to credit exhaustion, and a round is never skipped for want of
Fable.

**The one sentence that explains four of the findings.** `slot_of_id` is
global BY DESIGN -- pane ids address panes in every workspace, and W-1a chose
that deliberately so `pane/` readdir and `live_ids` remain resource and
addressability facts. The consequence nobody drew at the time: **every verb
that resolves an id must decide for itself whether a foreign-workspace target
is legal.** Before this round `in_active_root` had exactly TWO non-test call
sites, and both were carves. Not one verb used it. W-1a F2 had fixed what got
DRAWN and left what could be REACHED.

- **F1 [P0] -- `close()` on an inactive root was a no-op that still reported
  the surfaces released.** `close_inner`'s root arm tested `slot ==
  self.root()`, the ACTIVE root, so an inactive workspace's root fell through
  to the parentless early return -- after `collect_surfaces` had already
  filled the out-parameter. `retire` discards the return value (`let _ =
  self.layout.close(leaf)`) and `mint` takes the first free surface slot, so
  the leaf went on naming an index that was handed to the next client: another
  principal's surface, composed and given the keyboard inside the first
  principal's pane. `subtree_hosted` also stayed non-empty forever, so the
  workspace could never be reaped. This is the EXACT INVERSE of the W-1a
  defect, where the same arm was too BROAD and freed the whole pool -- both
  from `self.root()` silently meaning "active". The arm now keys on
  `workspace_of_root(slot)`, and an inactive root's collapse updates THAT
  workspace's remembered focus.
- **F2 [P1] -- the focus side-effects crossed workspaces.** `focus`,
  `zoom_toggle` (which calls `focus`) and `split` (which assigns
  `self.focused` directly) all took a global slot. Beyond keys routed to an
  invisible tile, `host_for` places the next surface at `self.focused`, so the
  NEXT CLIENT was hosted into the dormant workspace. `Layout::focus` is now a
  refusing chokepoint, with the setter guarded too. Note `split` moves focus
  in **two** places -- same-mode parents FLATTEN, different-mode NEST -- and
  guarding one is not a property of the function; that is the W-1a F2 shape
  recurring inside the fix for W-1a F2.
- **F3 [P1] -- `Workspace.focused` stored a SLOT.** `alloc` hands out the
  first FREE slot, so a pane created in another workspace could land on the
  remembered slot and pass the `is_leaf` restore guard as a genuinely live
  leaf. The field now stores a pane ID -- monotonic, never reused, so a dead
  remembered focus resolves to nothing, which is exactly the fallback wanted.
- **F4 [P1] -- `move_focused_to_workspace` detached before its last
  allocation**, so an exhausted pane table orphaned the leaf: parentless, in
  no tree, still hosting its surface, un-reapable, and still addressable by
  id. `move_dir` already had the ordering right. Every pane the move needs is
  now allocated before any mutation, with rollback.
- **F7 [P3] -- `reap_session_empties`'s root arm** had the same
  `self.root()`-means-active confusion; it uses `is_workspace_root` now.
- **S5 [P2] -- the vanish rule freed RESERVED leaves.** It tested only for
  hosted surfaces, so a workspace holding nothing but a half-built restore
  skeleton -- empty leaves stamped with `creator_conn` by H-4d precisely so
  the session's own compositor cannot fill them mid-build -- read as empty and
  was destroyed. `subtree_reserved` now guards it.
- **S2 [P2] -- `tab` mutated with no authority check on one path.** The check
  sat INSIDE `if let Some(anc) = tab_ancestor(focused)`, so a focused leaf
  with no tab ancestor skipped authority entirely and still reached
  `unzoom()`. It was the only arm of `layout_cmd` reaching a mutation with no
  authority predicate on any path, and `Actor::Client(0)` -- denied by every
  other predicate in the file -- could cancel another principal's zoom. With
  no ancestor there is nothing to cycle, so the arm now returns Ok(()) first.

**Coverage gap, stated rather than implied**: `server.rs` has no test module,
so S2 and F7 carry NO inline test and are witnessed by reasoning and the
interactive gates only.

**Still open**: F8 [P3] `split_fits` / `min_size` walk from the ACTIVE root, so
a hypothetical leaf in another workspace is never encountered and the minima
check passes vacuously -- a dormant split is unbounded. Bounded in consequence
by the dormancy net. And S4 [P2], an operator design fork: the vanish rule
RENUMBERS surviving workspaces, because a workspace's identity is its vector
index.

## Workspace numbers became identities (2026-09-15, S4)

Operator-ratified after round 1 surfaced it. Scripture landed first
(`5eae48f5`), then this.

**What was wrong.** A workspace's identity was its position in
`Layout.workspaces`. `reap_empty_workspaces` removes by index, so dropping an
empty MIDDLE workspace shifted every higher one down: the user's tiles stayed
alive but Super+3 no longer reached them -- it made a fresh empty workspace
instead. Both cited precedents refuse that. i3 treats workspace numbers as
NAMES rather than positions; tmux keeps stable numbers with gaps
(`renumber-windows` is opt-in and off by default).

**As built.** `Workspace` carries `number: u8` (1..=`MAX_WORKSPACES`) and the
vector is kept SORTED ASCENDING by it; the set is sparse, so 1, 3, 4 is an
ordinary state. `active` stays an internal INDEX -- positions are the right
thing for the carve and the painters, numbers are the right thing for identity
and labels -- with `active_number()` exposing the identity and
`workspace_numbers()` the list.

`ensure_workspace(n) -> Option<usize>` is the ONE find-or-create, and it exists
as one function because the subtle part is local to it: a sorted set means a
create INSERTS rather than pushes, and **an insert at or below `active` shifts
the active index**, which must move with it or the seat silently changes
workspace under the user. `switch_workspace` and `move_focused_to_workspace`
both route through it and both read `self.active` AFTER the call for that
reason. The move judges its empty-tile refusal BEFORE ensuring the target --
but that closed only ONE of the two doors, and this paragraph asserted the
whole claim until round 2 measured it (see the round-2 section below, F4).

**The retired rule, and why it was wrong twice over.** "Only the next free
number may be made" existed to keep a DENSE vector hole-free -- a property of
the representation, not of the design -- and it was attributed to i3, which
creates workspace 5 on Super+5 whether or not 2, 3 and 4 exist. So the ratified
choice made the switch SIMPLER: `n` goes straight through from the chord and
from the `layout` verb, with no index conversion and no skip check. The bound
is still `MAX_WORKSPACES` = 9, which the digit row enforces on its own, and
`ensure_workspace` states the count bound explicitly rather than leaving it as
an inference from uniqueness.

**The header is a format change on the ratified channel.** `workspaces N
active K` became `workspaces <ascending,csv,of,numbers> active <number>`. A
count cannot label a gapped set: a bar told "3" cannot know whether that means
1,2,3 or 1,3,4. The count is the list's length, so nothing is lost, and an
older reader fails CLOSED (the list does not parse as an integer).

**Coverage note.** `creating_a_lower_number_keeps_the_seat_where_it_was` is the
only test that reaches the index-shift line: every other workspace test creates
in ascending order, so without it that line is unexercised and a sabotage there
does not fire. `server.rs` still has no test module, so the `layout_cmd` arm
and the chord arms are witnessed by the battery and the gates only.

## The workspace round 2: a round-1 fix that disabled the vanish rule (2026-09-15, HALCYON-WORKSPACES round 2)

Round 1 closed DIRTY (a P0 returned), so the project's re-audit rule owed a
round aimed at THE FIXES. It found **1 P0 / 1 P1 / 1 P2 / 4 P3**, and the P0
was created by a round-1 fix -- which is the outcome that rule exists to
catch. Tier: OPUS fallback (Fable 5.1 credit-exhausted), so family diversity
was forfeit and context independence was the whole of what the round bought.

**F2 (P0) -- S5 made the vanish rule unreachable for any workspace a session
had split in.** Round 1's S5 taught `reap_empty_workspaces` to respect a
placement reservation by adding `&& !self.subtree_reserved(r)`, and
`subtree_reserved` keys on `creator_conn != 0`. Nothing cleared that field
when the leaf it reserved was FILLED, or when the root it sat on COLLAPSED --
`host_for`, `host_into` and `close_inner`'s root arm each cleared
`claim_token` and left it standing. The rail's SPLIT H stamps the splitting
conn (H-4d), which for a session is halcyond's own conn, alive as long as the
session. So: split a workspace, fill both tiles, close them, switch away --
and the workspace never vanishes again, for the rest of the session. Ratified
scripture silently stopped firing, and up to 9 roots stayed pinned out of
`MAX_PANES` = 32.

Fixed by ending the reservation where its purpose is served: cleared in
`host_for`'s fill arm, in `host_into`, and in `close_inner`'s root-collapse
reset (which already resets kind, status, claim, weight, dividers and
separator -- the stamp simply was not on that list).

**Why the battery could not see it.** The gate never issues a `split` VERB,
so its roots keep `creator_conn == 0` and vanish normally. The chord split
path does not stamp `creator_conn` either. Only the verb path -- the rail
button, `halcyon layout restore`, the tile menu -- arms it.

**A measurement worth keeping.** The end-to-end vanish test covers the host
clear and the collapse clear TOGETHER: reverting either ONE alone left it
green, because along that path the other still lifts the reservation. A
property with no per-site witness is shape, not bound, so each site now has
its own test (`filling_a_reserved_leaf_spends_its_reservation` and
`a_collapsed_root_drops_its_reservation`), and both fire.

**F1 (P1) -- `move_dir` grafted a pane out of a dormant workspace.** Round 1
closed the cross-workspace class at the FOCUS chokepoint; `move_dir` is a
STRUCTURAL verb taking a caller-supplied slot, and its root-wrap branch reads
`self.root()` -- the ACTIVE root -- unconditionally. A `move` verb naming a
dormant pane (reachable: `slot_of_id` is global by design, and the ownership
check passes for a principal that owns the subtree) detached it, wrapped the
ACTIVE root in a fresh container beside it, and re-seated the active
workspace onto that container. Now guarded by `in_active_root`, matching
`focus`'s precedent; teaching the wrap to re-seat the pane's OWN workspace
root would be a new feature, not a fix.

**F3 (P2) -- the move freed a RESERVED skeleton root.** `reap_empty_workspaces`
was taught to respect a reservation; `move_focused_to_workspace` was not. It
judged "placeholder" on emptiness alone, so an arriving tile ran
`free_subtree` over a restore tool's reserved skeleton and the tool's later
`create claim=` found nothing. The conjunct is now `&& !subtree_reserved`.

**F4 (P3) -- and the correction to the S4 section above.** A refused move
could still strand a freshly-minted empty workspace: the empty-tile check is
judged before the ensure, but the two ALLOCATION refusals sit after it. Fixed
by REORDERING rather than unwinding -- a newly-created workspace's root is
always an empty placeholder, so `pre_container` can only fail for a workspace
that already existed, which makes "ensure created it, then the leaf alloc
failed" the single strand window; hoisting that alloc above the ensure closes
it by construction. The stranded workspace was self-healing (the next reap
drops it), which is why it is P3 -- but the commit body and this dossier both
asserted an invariant the code did not hold, and that is the part worth
recording.

**Coverage.** tapestryd lib 65 (was 59): six new tests, every fix
sabotage-measured in isolation with `pane.rs` restored byte-identical.
`server.rs` still has no test module, so `layout_cmd`'s workspace arm, the
chord arms and `reap_session_empties` remain witnessed by the gates alone.

## The workspace round 3: the streak broke, and a witness that could not witness (2026-09-15, round 3)

Round 2 closed DIRTY, so a third round was owed on ITS fixes. Result:
**0 P0 / 0 P1 / 1 P2 / 4 P3** -- the two-round run of "the previous round's fix
is the next round's P0" is **broken**; no P0 could be constructed against
`move_focused_to_workspace`'s reordering or `move_dir`'s new refusal. The round
also WITHDREW three of its own draft findings on re-derivation, including one
that had asserted an arithmetic difference between `len.max(1) as u8` and
`(len as u8).max(1)` and then found both yield the same value.

**F1 [P2] -- the P0's third clear site had no witness, and the close said it
did.** Round 2 cleared `creator_conn` at THREE sites (`host_for`, `host_into`,
`close_inner`) and wrote TWO isolating tests, while its own commit body read
"each site got its own isolating test and BOTH now fire". The word "both" for
three sites was the tell, and nobody re-read it. CONFIRMED BY MEASUREMENT
before fixing: reverting the `host_for` clear left all 65 tests green. It
matters because `host_for` is the GENERAL production fill path -- the
claim-less create and a claimed create whose `host_into` failed both land
there. A third witness now exists and fires.

**A VALUE'S WITNESS MUST BE ABSOLUTE.** Every bound assertion in `pane.rs` was
phrased `MAX_WORKSPACES as u8 + 1`, i.e. RELATIVE to the constant -- so
lowering the constant moves the goalpost with it and no test can notice.
Measured: setting the shared bound to 8 left the entire tapestryd suite green
while halcyond's went red. `the_ratified_bound_is_nine_workspaces` now pins the
value absolutely, which is simultaneously the only proof this crate reads the
SHARED definition rather than a private copy that merely agrees today.

**F5 [P3]** -- the move allocated its replacement leaf before validating `n`,
so an out-of-range number allocated a pane, rolled it back, and burned a
monotonic id for a move that was never legal. Range check hoisted above the
alloc. Its witness had to compare two identically-built layouts' probe ids,
because the refusal ALREADY returned false before the fix: the burned id is the
only observable, so asserting the refusal would have been a check that cannot
fail.

**F2 [P3], recorded rather than hidden** -- the round-2 root-collapse clear has
a cost. Any peer may close an EMPTY leaf (`subtree_surfaces` is empty, so the
ownership walk's `.all()` is vacuously true, which the server does
deliberately), so a peer closing a reserved skeleton root now lets that
workspace VANISH where it used to persist. Accepted: the pre-existing
`claim_token = None` on the same arm already destroyed the tool's placement,
and a stale claim degrades to focus placement rather than failing. Noted in the
arm's comment.

**Coverage.** tapestryd lib 68 (was 65). Every round-3 fix sabotage-measured in
isolation, `pane.rs` and `libhalcyon/layout.rs` restored byte-identical.

## F8: the minima judged the ACTIVE root in both directions (2026-09-15)

Tracked from the workspace round 1, carried through rounds 2 and 3, fixed
here. `split_fits`, `min_size`, `min_fits` and `fits_after` all walked from
`self.root()` -- the ACTIVE root -- so a mutation in a DORMANT workspace was
measured against a tree it does not live in. `min_size_hyp` never encountered
the hypothetical leaf, the walk returned the active tree's ordinary minima,
and the check passed VACUOUSLY: a dormant split was unbounded.

**Reachable by verb, not hypothetical.** Most callers pass `self.focused`,
which `Layout::focus` chokepoints to the active root, so they were never the
problem. Two caller-supplied paths were: the `split` verb (`server.rs`, gated
on ownership rather than on the active root) and the `mode` verb through
`fits_after(|l| l.set_mode(slot, mode))` -- and `set_mode` carries no
active-root guard of its own. Round 2's F1 guard had already closed the third,
`move_dir`.

**The fix is per-workspace, and F8 is on record as NOT a one-liner for a
reason.** Widening the check to "every root must fit" is the tempting
one-liner and is wrong in the other direction: a dormant workspace already past
its minima -- a layout restored onto a smaller display -- would freeze the
workspace the user is actually looking at. So 5.2's "already past its minima
stays mutable" rule is preserved PER ROOT, and only a workspace that FIT before
and does not after is a refusal.

As built: a new `top_of(slot)` returns the parentless ancestor (no such helper
existed anywhere in the crate -- measured); `split_fits` walks from
`self.top_of(slot)`; `min_fits` became `min_fits_root(root)`; and `fits_after`
compares each workspace's fit before and after, keyed by NUMBER rather than
index, because a mutation may create or vanish a workspace and S4 made the
number the identity while the index shifts under an insert.

**The measurement that makes the inverse control a guard.** Three sabotages,
all fired: restoring `self.root()` in `split_fits`; restoring the
active-root-only `fits_after`; and -- the one that matters --
IMPLEMENTING THE WRONG FIX, the every-root widening, which fails
`a_dormant_overflow_does_not_freeze_the_active_workspace`. Without that third
sabotage the inverse control would be decoration; with it, the test
discriminates the over-correction and not merely the absence of a fix.

tapestryd lib 71 (was 68). `pane.rs` restored byte-identical after each.

