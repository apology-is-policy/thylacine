# WARP-V3-DESIGN.md -- Venus over the coherent ring

Implementation design for **Warp-6 V-3** (`vn_renderer_thylacine` + the coherent
shmem ring). Elaborates `GPU-DESIGN.md` sections 2.3, 2.4, 6.2, and the section-12
V-3 ladder entry into an implementable design. Binding for the V-3 sub-chunks;
where it and `GPU-DESIGN.md` disagree, GPU-DESIGN wins and this doc is corrected.

Status: **V-3a SHIPPED (`f12d7317`) -- but see section 0: it is NOT Venus's ring.
V-3b architecture RESOLVED to Model B (operator vote 2026-08-20), detailed in
section 0.** Tip context: V-2 (`SYS_BURROW_FROM_HOSTMEM=107`) shipped at
`7973f8dc`; V-3a at `f12d7317`.

---

## 0. V-3b RESOLVED ARCHITECTURE + the V-3a premise correction (2026-08-20)

**This section supersedes the section-2/3/4 premise that "the V-3a coherent
ring is Venus's ring." A design-pass spike (source-cited against Mesa 25.0.7 +
virglrenderer main) proved that premise WRONG, and the operator ratified
Model B. Read this before the older sketch below.**

### 0.1 The spike finding (source-cited)
1. **Unpatched Venus creates its command ring UNCONDITIONALLY** and cannot run
   ringless: `vn_instance.c:320` calls `vn_instance_init_ring` with no gate and
   aborts instance creation on its failure; every real Vulkan command threads
   through the ring; only 4 ring-bookkeeping commands (`vkCreateRingMESA` /
   `Destroy` / `Notify` / `SubmitVirtqueueSeqno`) ever use the SUBMIT_CMD path.
2. **virglrenderer executes venus commands from SUBMIT_CMD fully** -- its ring
   dispatch is a copy of the context dispatch (`vkr_ring.c:103`), so SUBMIT_CMD
   is a first-class path (it is how the ring is bootstrapped), not a gate.
3. **Venus's ring MUST be host-allocated shmem** (`HOST3D` + `blob_id=0` +
   `MAPPABLE` -> `FD_SHM`): virglrenderer FATALLY rejects any non-`FD_SHM` ring
   (`vkr_transport.c:201-203`) and derefs it as a contiguous host VA; Venus's
   driver hard-codes `HOST3D` and REFUSES guest memory
   (`vn_renderer_virtgpu.c:1457` -- host process isolation cannot deref guest
   sglists). Both guest and virglrenderer map the SAME host pages.

### 0.2 The correction: the V-3a ring is NOT Venus's ring
The shipped V-3a ring (`f12d7317`) is a `blob_mem=GUEST`, **tapestryd-consumed**
ring (head=producer/tail=consumer; `wring_kick` drains). Venus's ring is
`HOST3D`, **virglrenderer-consumed**, head=consumer/tail=producer (the opposite
convention). So V-3a cannot be Venus's ring -- wrong backing AND opposite
convention. **V-3a is not wasted**: it is a valid coherent-ring primitive for a
NATIVE (non-Venus) GPU client, and its `/srv/warp` ring ABI surface
(`ring/new|map|kick|fence`) + blob machinery are partly reusable. But its
tapestryd-consumer core is off the Venus path.

### 0.3 The resolved architecture: Model B (operator vote 2026-08-20)
virglrenderer polls Venus's HOST3D ring; tapestryd owns the device + the
resources but stays venus-agnostic:
- **shmem_create** -> tapestryd mints the ring as a `HOST3D` blob
  (`RESOURCE_CREATE_BLOB(blob_id=0, HOST3D, USE_MAPPABLE)` -> host anon-shmem),
  maps it into the guest via the **V-2 hostmem path** (`SYS_BURROW_FROM_HOSTMEM`,
  the host-shmem-fd -> guest-VA mapping), and registers it so virglrenderer maps
  the same host pages by `res_id`.
- **ops.submit** -> the venus command stream (incl. `vkCreateRingMESA` carrying
  the ring `res_id`, `vkNotifyRingMESA` idle-kicks, fences) is forwarded via
  `gpu.submit_3d` (the controlq SUBMIT_CMD) to virglrenderer. tapestryd forwards
  RAW command bytes -- it never parses venus. Low-traffic (the ring carries the
  bulk); the V-3a ring MAY serve as this transport, or a byte submit.
- virglrenderer maps the HOST3D ring (via `vkCreateRingMESA`) and POLLS it; the
  guest writes commands + advances tail; kicks (`vkNotifyRingMESA`) only when
  virglrenderer sets the ring IDLE status.
- **ops.wait** -> `t_poll` on the `ring/<ridx>/fence` fd(s) with the Vulkan ns
  timeout -> `timeout_ms` (the fence file is the host->guest wakeup, delivered
  by `poll_ring_fences` on retire); `wait_any` = poll multiple fds. No busy-spin.
- **get_info** -> the `caps` file (the retained Venus capset blob).

### 0.4 What V-3b builds (the deltas)
- **tapestryd**: the `HOST3D`-ring-blob mint + guest-map + register path
  (virglrenderer's `vkr_context_create_resource_from_shm` is the host-side
  reference); the SUBMIT_CMD forward of the venus stream on the controlq; the
  reply-shmem registration (`FD_SHM`). The OWED host-side rescue (round-3 F1)
  applies to the fenced-submit drain. Audit-bearing (the section-25.4 Warp row).
- **Mesa** (`vn_renderer_thylacine.c`, ~1.2 kLOC, patch 0010+ under
  `src/virtio/vulkan/`): the 19-fn `vn_renderer` (16 mandatory), mapping above;
  built with `-Dvulkan-drivers=virtio` on `thyla-keep`, a Vulkan ICD artifact +
  a Vulkan prove-gate on thyla-pi (real V3D). Mesa base pin: `mesa-26.1.6`.
- V-3c (capset authority, I-45) + V-3d (E2E) stay separate.

### 0.5 Open questions RESOLVED by the spike
- **Ring blob type**: `HOST3D` (host-allocated shmem), NOT `blob_mem=GUEST`.
- **coherent=0 (ringless)**: not available unpatched; Model A rejected (it would
  need a Venus source patch, violating "zero patches to the Venus driver").
- **Who consumes the ring**: virglrenderer (Model B), not tapestryd.

Full spike verdict + the 3-way analysis lived in the design-pass scratchpad
(`scratchpad/v3b/SPIKE-VERDICT.md`); the settled sub-designs (the host-side
rescue, `ops.wait`, the build) in the same dir. Sections 1-5 below are the
PRE-correction design; where they say the V-3a guest ring is Venus's ring, this
section 0 governs.

### 0.6 V-3b-1a as-built: the HOST3D + MAP_BLOB substrate, proven on GL (2026-08-24)
The first Model B rung -- the tapestryd primitive that mints and maps a HOST3D
blob -- is built and proven on real GL (thyla-pi KVM/V3D, virgl 1.1.0, QEMU
10.0.11). It lands three methods in `usr/tapestryd/src/gpu.rs`:
- `create_host3d_blob(res, ctx, flags, len)` -- `RESOURCE_CREATE_BLOB` with
  `blob_mem=HOST3D`, `blob_id=0`, `nr_entries=0` (host-allocated, no guest
  `mem_entry`, so HDR+32 not the GUEST HDR+48), the `ctx_id` in the header.
- `map_blob(res, offset) -> map_info` -- `RESOURCE_MAP_BLOB` (HDR+16); the host
  does `memory_region_add_subregion(&hostmem, offset, mr)`, so the blob's bytes
  appear at `hostmem_base + offset` (the PA the guest then maps via the V-2
  `SYS_BURROW_FROM_HOSTMEM`), and returns the `RESP_OK_MAP_INFO` cache word.
- `unmap_blob(res)` -- `RESOURCE_UNMAP_BLOB` (HDR+8), the teardown inverse.

**The empirical refinement to section 0.3 (the venus-context requirement).** A
`HOST3D` `blob_id=0` `USE_MAPPABLE` blob is the **vkr (venus renderer) shm
path** (`vkr_context.c`: `blob_id==0 && blob_flags==USE_MAPPABLE`), and it is
reachable ONLY through a **capset-4 (venus) context**. A create under a virgl
context or device-global (`ctx_id=0`) is refused (`RESP_ERR_UNSPEC` at the QEMU
layer / `EINVAL` from `virgl_renderer_resource_create_blob`). So section 0.3's
"tapestryd mints the ring as a HOST3D blob" is refined: **the mint runs under a
venus context tapestryd owns.** This does not fork Model B -- tapestryd stays
venus-agnostic (it forwards raw command bytes; the venus context is the host-side
resource *scope*, not command parsing), and it is the same context the guest's
venus stream runs in (one host-side venus ctx owns the ring). The `host3d_probe`
init self-test carries the proof: Arm A (venus ctx) MAPs; Arm B (device-global)
is the negative control whose refusal proves the requirement is real, not
incidental.

**The host prerequisite (provisioning note for any GL host).** virglrenderer's
venus renderer needs the `virgl_render_server` binary
(`/usr/libexec/virgl_render_server`) to service HOST3D shm resource ops -- the
library is built in *process mode* and forks it per context. Debian's
`libvirglrenderer1` ships **no such binary** (no package provides it), and
without it `get_blob` returns a bare `EINVAL` (no fork-fail log -- the absence of
a distinct error is what made this a multi-boot hunt). thyla-pi needed the binary
built from virglrenderer 1.1.0 source and installed to `/usr/libexec/`
(additive; it does not touch `libvirglrenderer.so`). Any fresh venus GL host must
be provisioned with it.

**Constant correction.** `RESP_OK_MAP_INFO = 0x1106` (this doc's and
IMPL-PLAN's earlier `0x1105` is `RESOURCE_UUID` -- an off-by-one; the shipped
code uses `0x1106`, verified against QEMU v10.0.2's verbatim enum).

Proven line: `tapestryd: gpu host3d-map venus-ctx MAPPED (map_info=0x1)`, with
the device-global arm refused. The gate is folded into the `venus` verb's
`venus-verdict` (a test-leg MAP + a device-global refusal + the control-leg
skip), discrimination-tested by `tools/test-venus-verdict.sh` without a boot.

### 0.7 V-3b-1b as-built: the guest-map (2026-08-24)
V-3b-1a proved the host places a HOST3D blob in the hostmem BAR; V-3b-1b
guest-maps it. Two pieces:
- **libthyla-rs**: the `SYS_BURROW_FROM_HOSTMEM` client binding V-2 left unbuilt
  (V-2 exercised the path in kernel unit tests only). `t_burrow_from_hostmem(handle,
  shmid, offset, length, cache_policy)` (a 5-arg `svc`) + the `T_CACHE_*`
  cache-policy constants (mirroring the kernel `enum t_cache_policy`) +
  `PciDev::burrow_from_hostmem(shmid, offset, length, cache)`, which wraps the FFI
  with the held KObj_PCI claim -- the authority (I-5-non-transferable). Returns
  the guest VA.
- **tapestryd**: `HostmemAllocator`, a page-aligned bump allocator over
  `shm_region(1).length` handing out non-overlapping offsets; and
  `hostmem_map_probe`, which allocates an offset, creates + maps a HOST3D blob
  there under a venus ctx, guest-maps the subrange via the wrapper, and
  round-trips a sentinel through the guest VA.

The offset is one frame throughout: `map_blob(res, O)` and
`burrow_from_hostmem(1, O, len, WC)` both take `O` relative to the region window
base (device PA = `shm_region.0 + O`). The sentinel (a same-address
write-then-read, no barrier -- ARM coherency round-trips it) proves the guest can
ACCESS the mapped BAR; host-visibility (virglrenderer polling the ring) is a
later rung (V-3b-1c/2), correctly not claimed here. The VA is mapped WC
(`T_CACHE_WC` -> Normal Non-Cacheable) so guest ring stores drain without a
cache flush. The allocator is bump-only at V-3b-1b; a free-list arrives with the
ring lifecycle (V-3b-1c, section 0.8). Proven on GL (thyla-pi KVM/V3D):
`tapestryd: gpu hostmem-map MAPPED+ROUNDTRIP`; the control leg (no F_RESOURCE_BLOB)
self-skips. Folded into the `venus` verb's `venus-verdict`.

### 0.8 V-3b-1c-1 as-built: the persistent hostmem ring engine (2026-08-24)

V-3b-1b's guest-map was a one-shot probe: a local allocator, one ring, torn down
in place. V-3b-1c-1 makes it a reusable, persistent ENGINE -- the substrate the
client-claimable Model B ring (V-3b-1c-2) and the venus-stream forward (V-3b-2)
build on. Three deltas, all in `usr/tapestryd/src/gpu.rs`:

- **The allocator is persistent + reclaiming.** `HostmemAllocator` is hoisted
  into a `Gpu.hostmem: Option<HostmemAllocator>` field, sized once from
  `shm_region(1)` at probe, and gains a first-fit **free-list**: a torn-down
  ring's offset is reclaimed, so a persistent daemon minting and retiring rings
  across client sessions does not exhaust the 256 MiB region (bump-only would).
  No coalescing at v1.0 -- ring blobs are uniform-ish (page-rounded, <=
  `WARP_RING_MAX`), so same-size frees exact-match without splitting; a Vec-grow
  failure leaks the extent (bump fallback) rather than aborting.
- **The lifecycle is a reusable pair.** `mint_host3d_ring(res_id, ctx_id, len)
  -> HostRing` composes the V-3b-1a/1b steps (alloc offset -> `create_host3d_blob`
  under a venus ctx -> `map_blob` -> `burrow_from_hostmem` at the host-dictated
  cache) with **full error unwinding** at every stage (offset -> resource ->
  subregion), so no half-minted ring is ever left behind. `drop_host3d_ring(&HostRing)`
  is the inverse (`t_burrow_detach` -> `unmap_blob` -> `resource_unref` ->
  reclaim the offset). `HostRing` carries exactly what teardown needs; the caller
  disarms any weft share first (I-7 #847, V-3b-1c-2's concern).
- **The probe proves the engine, not a single map.** `hostmem_ring_probe` mints
  TWO rings under one venus ctx (the allocator must hand DISTINCT offsets --
  `off_a=0x0`, `off_b=0x1000`), round-trips a sentinel through each guest VA,
  tears both down, then RE-MINTS and asserts the freed offset is REUSED (the
  free-list). One verdict line:
  `tapestryd: gpu hostmem-ring MAPPED+ROUNDTRIP x2 (off_a=.. off_b=.. cache=CACHED) teardown+remint-reuse OK`,
  emitted only when all four properties hold (else `hostmem-ring FAIL (...)`).

The `venus` verb's `venus-verdict` gate anchors on the `x2` success line (a FAIL
line -- any property false -- is rejected); `tools/test-venus-verdict.sh` proves
the discrimination without a boot, including a `reuse=false` FAIL-line leg so a
lifecycle regression cannot pass. What is deliberately NOT here: the ring is not
yet a client-claimable `/srv/warp` file, and no client Proc maps it -- that (the
weft-share of the hostmem burrow via `WEFT_BIND_HOSTMEM`, the per-client venus
device-ctx, the `warp-prove` cross-Proc leg) is V-3b-1c-2.

---

## 1. What V-3 is, and the seam it plugs into

Mesa's Venus (Vulkan) driver talks to a host renderer through **one designed
seam**: `struct vn_renderer` -- `shmem` / `bo` / `sync` + `submit` / `wait` /
`get_info` (GPU-DESIGN section 2.3). Two backends exist upstream:
`vn_renderer_virtgpu` (Linux ioctls) and `vn_renderer_vtest` (a Unix socket, no
kernel driver at all). **A Thylacine backend is a third: ~1-1.5 kLOC, zero
patches to the Venus driver above it.**

The backend owes the driver above it three things and no more:

- **coherent shmem** (`vn_renderer_shmem_create` / `_map`): a shared region the
  driver lays out its *own* command-stream ring inside (head / tail / status
  cachelines). **We do not define the ring format** -- Venus does, in our shmem.
  Our job is that the region is coherent: host writes to it become visible to a
  guest poll without an explicit syscall, and vice versa.
- **submit** (`vn_renderer_submit`): hand a batch of the driver's CS to the host
  renderer, carrying a `ring_idx` (u8, 0-63; Venus allocates one per `VkQueue`),
  with in-order completion *per ring_idx* and exactly one waitable per submit.
  GPU-DESIGN section 2.4: *"that is a Loom CQE."*
- **wait / sync**: a submission's completion is observable. Venus **simulates
  syncobjs in userspace** (`SIMULATE_SYNCOBJ 1`): `VkFence`/`VkSemaphore` are
  *feedback slots in shared memory* the host writes and the guest polls. **There
  is no host->guest interrupt for ring replies.** We owe no syncobj object
  model, no timeline-semaphore primitive, no dma-fence graph.

The virtqueue is used only for bootstrap, the doorbell kick, and fence delivery;
the doorbell is *skipped entirely* unless the host's ring thread has gone idle
(section 2.4). That idle-skip is the one place I-9 (no lost wakeup) has teeth --
see section 4.

## 2. Sub-chunk breakdown

| Sub-chunk | Scope | Build/validate locus | Kernel? |
|---|---|---|---|
| **V-3a** | the coherent shmem ring primitive: a server-minted, weft-shared, coherently-mapped guest blob + the submit doorbell (idle-skip) + the fence feedback-slot signal + **F2** (validate client pa/len). The `/srv/warp` ABI addition. | tapestryd (Rust, in-tree) + a `warp-prove ring` round-trip on the **GL host** (a virgl device). **No Venus DRIVER needed** -- but a virgl DEVICE is: the ring lives under a warp ctx, and `ctx/new` returns `E_OPNOTSUPP` on a 2D device (a local 2D run SKIPs cleanly; the mechanism is unexercised). Impl-time correction to the original "local, no builder" plan. | **No new syscall** (rides weft + the V-1 guest-blob path) |
| **V-3b** | `vn_renderer_thylacine.c` -- the ~1-1.5 kLOC Mesa Venus backend over the V-3a ring; the winsys `coherent=0->1` flip. | a new Mesa patch (0010+) under `src/virtio/vulkan/`; host cross-build on `thyla-keep`. | no |
| **V-3c** | the I-45 capset-authority check (`WarpCtx.capset` goes live; reject a never-enumerated capset) + the Venus `CTX_CREATE` wiring. | tapestryd `server.rs` + `gpu.rs`. | no |
| **V-3d** | E2E on thyla-pi (via `WARP_HOST=thyla-pi-cf`): a Vulkan smoke through the full stack; lands **F1's** terminal-severity measurement (the V-2 death-quiesce leg, finally exercised by a live client). | thyla-pi (KVM, real V3D + the Vulkan V3D ICD). | no |

V-3a is the foundation (V-3b builds on it) and the only sub-chunk needing no
external builder -- hence the entry point.

---

## 3. V-3a -- the coherent shmem ring

### 3.1 Where the ring physically lives

The ring is a **guest blob** (`blob_mem = GUEST`, `gpu.rs:113`): its storage IS
guest `mem_entry` pages -- no host allocation, no hostmem BAR. (V-2's hostmem BAR
mapping is a *different* rung, for host-allocated `HOST_VISIBLE VkDeviceMemory`
via `MAP_BLOB`; it does not back the command ring.)

In Thylacine terms the blob's backing is a **weft-shared burrow** minted by
tapestryd -- the existing V-1 guest-blob path -- and delivered to the client
through the weft map fid:

1. tapestryd mints the backing (a weft-share-admissible burrow: the section-6.1
   GPU-BO subtype `t_dma_create_gpu_bo`, or a plain DMA page as `blob_probe`
   uses) and registers its guest PAs with the virtio-gpu device via
   `RESOURCE_CREATE_BLOB` (`blob_mem=GUEST`, the `mem_entry` list).
2. the client opens the ring file under its ctx, and maps the backing with
   `t_weft_map(fd, 0) -> VA` (`SYS_WEFT_MAP` -- the same primitive Warp-3 BOs
   use, `warp_client.h` `warp_bo_map`).
3. the mapping is **`NORMAL_WB`** (cacheable, coherent). Coherence between the
   guest poll and host writes is the **virtio coherent-DMA model**: device
   writes to guest RAM are coherent with the guest's own view (QEMU's device
   model writes the same host pages that back guest RAM), and the guest uses
   **acquire/release barriers**, never cache maintenance, to order its reads of
   the status/fence cachelines against the doorbell. This is the standard
   virtio ring discipline; V-3a inherits it rather than inventing a Thylacine
   coherence attribute. (Contrast V-2's hostmem BAR, which needed the MAIR
   attr-index widening precisely because it is *not* ordinary guest RAM.)

**Consequence: V-3a adds no kernel syscall and no burrow type.** The ring is a
weft-shared WB burrow; the kernel bounds the client's map to the burrow (the
weft share-in path, the same bound V-2's `hostmem_resolve_subrange` enforces for
hostmem). Everything else is tapestryd + the winsys.

### 3.2 The `/srv/warp` ABI addition

The warp tree today (149-warp.md "The tree") carries per-ctx `bo/<id>/{ctl,map}`,
`submit`, and `fence`. V-3a adds a **ring** under the ctx:

```
ctx/
  <id>/
    ring/
      new         # write "<bytes> <ring_idx>" -> mint a coherent ring blob of
                  #   <bytes> (page-rounded, <= WARP_RING_MAX) for ring_idx
                  #   (0-63); read -> "<res_id>\n"
      <ridx>/
        map       # weft map fid: open + t_weft_map -> the ring VA (NORMAL_WB)
        kick      # write (empty) -> doorbell: process the ring iff the host
                  #   ring thread is idle (the idle-skip; section 4)
        fence     # read blocks until the next completion for this ring_idx;
                  #   returns the monotonic completed-seq (counted, not parsed,
                  #   like the existing ctx/<id>/fence)
```

The existing `ctx/<id>/submit` (one `t_write` = one atomic CS batch) remains the
**virgl** path (coherent=0, copy-in). The **ring** path is the Venus/coherent
path: the client writes CS into the mapped ring and kicks, rather than `t_write`
-ing the stream. Both share `warp_fenced_admit` (the per-ctx in-flight throttle,
`server.rs:7032`) so the #204/#210 backpressure model is unchanged.

### 3.3 F2 -- validate client pa/len (the deferred fork, now due)

Warp-3 hardwired **`coherent=0`** in the winsys -- "no blob mappings, the F2
fork" (149-warp.md:648) -- precisely to defer the question of a client mapping a
blob and naming a subrange of it. V-3a lands F2:

- **the client never supplies a raw PA.** It supplies a `(ring_idx, offset,
  len)` against a ring it was handed. tapestryd validates `offset + len <=
  minted_ring_bytes` (the geometry it minted in `ring/new`), and the kernel weft
  map independently bounds the mapping to the shared burrow -- the same
  two-layer bound V-2 uses (`hostmem_resolve_subrange` + the burrow map bound).
  A client-declared geometry that exceeds either is refused (`E_INVAL`), never
  clamped.
- **the ring backing is minted by tapestryd, weft-shared to exactly one client
  (the ctx owner), and bounded by that ctx's page budget (I-32).** The client
  cannot name another ctx's ring (I-45; ring files live under `ctx/<id>/`, and
  `wctx` already enforces one ctx per conn + owner-conn identity).

F2 is therefore "the client's declared ring geometry is validated against the
backing tapestryd minted, at both the tapestryd verb and the kernel weft map,
and refused-not-clamped on violation." No raw client pointer reaches the device.

### 3.4 Submit / completion mapping to the existing lane

- **submit** = the client writes its CS into the ring (guest-visible shmem) and
  writes `ring/<ridx>/kick`. tapestryd, on a kick, admits via
  `warp_fenced_admit` and hands the ring's current head..tail slice to the
  device on the fenced controlq (`gpu.submit_3d`), carrying `ring_idx`.
- **completion** = the device retires the fence; tapestryd advances the
  ring_idx's completed-seq and (a) writes the feedback slot in the ring blob
  (the guest polls it -- the zero-syscall fast path) AND (b) unblocks any reader
  parked on `ring/<ridx>/fence` (the syscall slow path, for a guest that chose
  to block rather than poll). Both are offered; Venus uses the poll.

### 3.5 The kick / idle-skip -- where I-9 has teeth

The doorbell is skipped unless the host ring thread has gone idle (section 2.4).
That is a classic lost-wakeup shape and must be a **register-then-observe**:

- the host ring thread, before parking, publishes `idle=1` **then re-scans the
  ring head** (observe-after-publish); if head advanced, it clears `idle=0` and
  keeps draining.
- the guest, after writing CS and advancing the ring head, reads `idle`
  (acquire); iff `idle==1` it writes `kick`. If it races and sees `idle==0` it
  skips the kick -- correctly, because a non-idle host is still draining and
  will see the advanced head.

The single reordering that loses a kick -- guest advances head, host reads stale
head, host publishes idle, guest reads stale idle=0 and skips -- is closed by
the acquire/release pairing on head and idle (the store-buffer register-then-
observe, the I-9 idiom the Weft readiness ring and `tsleep` already use). This
is the one V-3a mechanism that warrants explicit invariant prose in the impl
header + the audit; it is the coherent-ring analog of `weft_readiness.tla`'s
single-cache-line poke.

### 3.6 Invariants on the line

- **I-45** (GPU authority bounded by context): the ring is minted under a ctx,
  weft-shared to that ctx's owner conn only, and named only through
  `ctx/<id>/ring/`. One ctx per conn (`wctx_mint`) already holds. A ring cannot
  address another ctx's resources; the submit carries the ctx's `dev_ctx`.
- **I-9** (no lost wakeup): the kick idle-skip, section 3.5.
- **I-32** (resource floor): the ring backing is charged to the ctx owner's page
  budget at mint; `WARP_RING_MAX` bounds a single ring; the per-ctx BO
  count/byte caps (`WARP_CTX_BACKING_MAX`, #204) already bound the aggregate.
- **F1 tie-in** (V-3d): a ring is a weft-shared burrow, so a *client's* live ring
  mapping across a tapestryd death is exactly the V-2 F1 scenario -- but for a
  guest blob (guest RAM), not a hostmem BAR, so the death-quiesce MEM_SPACE
  concern does not apply; the burrow simply outlives via its refcount until the
  client unmaps. V-3d measures the real thing on hostmem (host3d blobs), where
  F1's partial-quiesce is exercised.

### 3.7 Tests (V-3a, the GL host, no Venus driver)

A `warp-prove ring` scenario on a virgl device -- the ring mechanism validated
without Mesa/Venus. **It runs on the GL host, not locally: minting the warp ctx
the ring lives under requires virgl** (`ctx/new` is virgl-gated; a 2D device
returns `E_OPNOTSUPP` and the prover prints `RING SKIP`). Driven by
`tools/warp/warp-ring.exp` via `tools/warp-host.sh ring`. Steps:

1. open `/srv/warp`, mint a ctx, `ring/new "<4096> 0"`, map the ring VA.
2. write a sentinel command word at head; read `idle`; `kick` iff idle.
3. tapestryd (a test-mode ring echo, behind the existing `warp-hold`/test lever
   family) advances tail, writes the feedback slot with a known seq.
4. assert the client observes the feedback slot (poll) AND a blocking
   `ring/<ridx>/fence` read returns the same seq.
5. **F2 rejection legs**: `ring/new` with `bytes > WARP_RING_MAX` -> refused;
   a map/geometry with `offset+len > minted` -> `E_INVAL`, refused-not-clamped;
   a second ctx naming the first ctx's ring -> refused (I-45).
6. **I-9 leg**: a delayed-idle-publish injection (test lever) that would lose a
   kick under a naive protocol; assert the completion still lands (the head
   re-scan catches it).

Audit-bearing (tapestryd is the section-25.4 Warp row; the ring is a new
client-writable shared transport into the GPU): a focused round after V-3a,
prosecuting I-45 / I-9 / I-32 / F2 on the ring.

---

## 4. V-3b/c/d -- sketch (elaborated when reached)

- **V-3b**: `vn_renderer_thylacine.c` implements `struct vn_renderer` over the
  V-3a ring: `shmem_create` -> `ring/new` + map; `submit` -> write CS + `kick`;
  `wait` -> poll the feedback slot; `get_info` -> the caps file. The winsys
  `coherent` flag flips to 1. New Mesa patch 0010+ under `src/virtio/vulkan/`;
  built on `thyla-keep` via `tools/clade-mesa-cross.sh` with
  `-Dvulkan-drivers=virtio`.
  - **OWED at V-3b (audit round-3 F1, [[design-v3b-ring-kick-rescue-owed]]):** the
    V-3a drain cap (`WARP_RING_MAX_DRAIN_PER_KICK`) breaks WITHOUT the post-drain
    re-scan, so it relies on a documented guest obligation (a client blocking on
    the fence MUST re-check idle + re-kick) that the pipelined Venus ring MUST
    honor. Build the robust HOST-SIDE rescue here -- a follow-up drain the serve
    loop runs after other conns, bounded per pass (DoS-safe) -- against V-3b's
    `gpu.submit_3d` drain (which replaces the V-3a echo), since it needs a
    self-reschedule the V-3a single-RPC serve loop lacks.
- **V-3c**: `WarpCtx.capset` (written unvalidated at `server.rs:8687`, read
  nowhere today) goes live; the ctl `capset <n>` verb **rejects a capset the
  device never enumerated** (the I-45 obligation that rides V-3, not V-6); the
  Venus `CTX_CREATE` path in `gpu.rs` carries `context_init=capset` under
  negotiated `F_CONTEXT_INIT`.
- **V-3d**: `WARP_HOST=thyla-pi-cf tools/warp-host.sh` a Vulkan smoke
  (`vulkaninfo` / a headless compute dispatch) through the full stack on real
  V3D; measure F1's terminal severity (an EL0 access to a quiesced hostmem BAR
  under a client mapping -- benign garbage vs box-fatal abort), the one number
  V-2's audit refused to guess.

---

## 5. Open questions (pin during impl, not forks for signoff)

- **RESOLVED during impl (the "local, no builder" premise):** the ring is
  addressed under a warp ctx (`ctx/<id>/ring/<ridx>`), and `ctx/new` is
  virgl-gated (`server.rs`, the SUBMIT gate's twin) -- so a warp ctx, hence a
  ring, cannot be minted on a 2D device. V-3a's mechanism test therefore runs
  on the GL host, not locally. The local 2D path is graceful (the prover
  prints `RING SKIP`; tapestryd does not hang -- `ctx/new` fails clean). A
  local "deviceless ctx" test lever was considered and REJECTED: it would
  exercise a configuration production can never reach (a 2D device rejects ctx
  creation), the unconstructed-state anti-pattern.

- **F2 enforcement split** -- kernel weft-map bound vs tapestryd geometry check:
  the design says both (mirroring V-2). Confirm the weft map already refuses an
  out-of-burrow offset for a guest-blob share the way `hostmem_resolve_subrange`
  does for hostmem; if not, that check is V-3a's, kernel-side, and the "no new
  syscall" claim narrows to "no new syscall, one weft-map bound tightened."
- **feedback-slot vs fence-file dual signal** -- section 3.4 offers both. If the
  blocking `ring/<ridx>/fence` reader is never used by Venus (poll-only), it may
  still be worth keeping for a non-Venus client and for the test; decide whether
  it is load-bearing or test-only before the audit.
- **ring_idx multiplexing** -- one ring blob per `ring_idx`, or one blob with 64
  sub-rings? Upstream Venus allocates a shmem per ring context; one-blob-per-idx
  matches and keeps the per-idx weft share + budget clean. Start there.
