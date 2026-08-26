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
  reference); the SUBMIT_CMD forward of the venus stream on the controlq (V-3b-2,
  section 0.12); the OWED host-side rescue (round-3 F1) applies to the
  fenced-submit drain. The reply-shmem (`vkSetReplyCommandStreamMESA` + a second
  `FD_SHM`) needs NO new tapestryd substrate (the 2026-08-25 design pass, section
  0.13): it is a second host3d ring the client mints + a RING command Mesa writes
  into the command ring -- tapestryd forwards nothing. It folds into V-3b-3 (there
  is no separate V-3b-2b chunk), audited there (the section-25.4 Warp row).
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
later rung (V-3b-1c/2), correctly not claimed here. The VA is mapped at the
**host-dictated** cache attribute (`map_info_to_cache(map_info)`, CACHED on KVM),
never a hardcoded WC -- the GPU-DESIGN 6.2 honored-exactly rule the V-3b-1b F1 fix
established (a guest/host cache mismatch on ARM64 loses coherency; this sentence
said "mapped WC" until the F1-fix sweep corrected the code-doc drift). The
allocator is bump-only at V-3b-1b; a persistent free-list arrives with the ring
engine (V-3b-1c-1, section 0.8). Proven on GL (thyla-pi KVM/V3D): the
`hostmem-map MAPPED+ROUNDTRIP` line (renamed `hostmem-ring MAPPED+ROUNDTRIP x2` at
V-3b-1c-1); the control leg (no F_RESOURCE_BLOB) self-skips. Folded into the
`venus` verb's `venus-verdict`.

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
  subregion), so no half-minted ring is ever left behind. `drop_host3d_ring(HostRing)`
  is the inverse (`t_burrow_detach` -> `unmap_blob` -> `resource_unref` ->
  reclaim the offset), taken **by value** -- `HostRing` is a non-Copy single-use
  token, so the type system forbids a second drop (the F1 fix). `HostRing`
  carries exactly what teardown needs; the caller disarms any weft share first
  (I-7 #847, V-3b-1c-2's concern).
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

### 0.9 V-3b-1c-2 design: the client-claimable HOST3D ring (design-first)

The 1c-1 engine mints/retires a HOST3D ring tapestryd-internally. V-3b-1c-2 makes
it a ring a **client Proc** claims and maps through `/srv/warp`, so a later Mesa
Venus backend (V-3b-3) writes its command stream into it and virglrenderer polls
the same host pages. It is invariant-bearing (I-45 GPU-authority, I-32 resource
floor, I-7 lifetime, I-37 weft dataplane), so this design lands as scripture
before the code. Every open question here is a section-5 "pin during impl", NOT a
signoff fork: Model B (section 0.3) and `WEFT_BIND_HOSTMEM` (V-2) are ratified.

**Four pieces.**

1. **The per-client venus device-ctx.** `wctx_mint` (`server.rs:5393`) creates a
   client's warp ctx as a VIRGL device-ctx (`dev_ctx = slot+1`, `ctx_create(dev_ctx,
   "warp")`) -- that serves the V-3a guest-blob rings + BOs. A HOST3D ring needs a
   VENUS (capset-4) device-ctx (V-3b-1a's proven requirement). Per section 0.6 the
   ring is owned by "one host-side venus ctx tapestryd owns... the same context the
   guest's venus stream runs in" -- i.e. ONE venus ctx per client warp ctx, owning
   all that client's HOST3D rings. `WarpCtx` gains `venus_ctx: Option<u32>`,
   **lazily created** on the first HOST3D ring mint (`ctx_create_capset(id, VENUS,
   ...)`) and destroyed with the warp ctx in `wctx_retire`. The id must be
   statically-asserted DISJOINT from the `dev_ctx` range (`1..=MAX_WARP_CTXS`),
   `COMPOSITOR_CTX`, and the fixed probe ctx ids -- e.g. `dev_ctx + MAX_WARP_CTXS`
   (the probe-id-disjointness assert is the precedent). Lazy so a client that mints
   only guest-blob rings pays no venus ctx; per-client so V-3b-2's forward uses the
   same ctx (no rework). A venus-ctx create failure fails the ring mint clean
   (`E_IO`), never the warp ctx.

2. **The HOST3D ring flavor in the `/srv/warp` ring subtree.** Section 0.2 keeps the
   V-3a guest-blob ring (coherent non-venus clients + `warp-prove`); Model B ADDS a
   HOST3D ring -- ADD, not replace. `ring/new` gains a flavor: `"<bytes> <ridx>
   host3d"` mints via the 1c-1 `mint_host3d_ring` engine under `venus_ctx` (creating
   it lazily) instead of the guest-blob `t_dma_create_gpu_bo` path; a bare
   `"<bytes> <ridx>"` stays the V-3a guest-blob ring. `WarpRing` gains a
   backing-kind (a `HostRing` for the host3d flavor, or `None` for guest-blob) so
   `wring_teardown` (`server.rs:6690`) drives `drop_host3d_ring` (detach -> unmap ->
   unref -> reclaim the offset) for a host3d ring vs the guest-blob `t_close` path.
   Both share the ctx page budget (I-32) + the per-ring bounds.

3. **The weft-share of the hostmem burrow.** A HOST3D ring's backing is a
   `BURROW_TYPE_HOSTMEM` burrow (the 1c-1 `burrow_from_hostmem` result at
   tapestryd's VA). `wring_weft_ensure` (`server.rs:6404`) already lazily
   `t_weft_share(va, size)`s a ring's backing; for a hostmem burrow that call
   routes to `WEFT_BIND_HOSTMEM` (`weft.c:401` admits `BURROW_TYPE_HOSTMEM &&
   kobj_pci && ring_entries==0`) -- the V-2 surface never yet exercised by a real
   client. The client opens `ring/<ridx>/map`, `t_weft_map`s the share, and gets
   the ring VA at the **host-dictated** cache (I-37; the share carries the burrow's
   attribute -- confirm the weft map preserves it, else that is a kernel-side pin).
   No new kernel syscall.

4. **The `warp-prove` cross-Proc leg.** A new `warp-prove ring-host3d` mode: mint a
   HOST3D ring under a warp ctx, claim `ring/<ridx>/map` from the prover Proc (a
   DIFFERENT Proc than tapestryd), and round-trip a sentinel -- the E2E proof of the
   weft-hostmem client path (the V-2 `WEFT_BIND_HOSTMEM` finally exercised
   cross-Proc). Venus-gated (a 2D device SKIPs, like the V-3a ring prover).

**Invariants on the line.** I-45: the HOST3D ring lives under the client's OWN
venus ctx (`wctx` enforces one ctx per conn + owner identity; a client cannot name
another's ring). I-32: the ring's bytes count against the ctx's page/backing
budget exactly as the guest-blob ring does. I-7: the ring backing lives until the
last client unmap AND the retire -- `wring_teardown` disarms the weft share BEFORE
`drop_host3d_ring` frees (the #847 dual count; the client's mapping survives via
its own ref). I-37: registration-is-the-capability -- the weft share bounds the
client to exactly the ring backing, no per-op mediation.

**The split (fit).** 1c-2a = the venus device-ctx lifecycle + the HOST3D ring
flavor in the subtree + `wring_teardown` (a test-lever mints a host3d ring under a
real warp ctx, tapestryd-side sentinel, tears down -- provable without a client).
1c-2b = the weft-share client claim + the `warp-prove` cross-Proc leg. Each lands
independently, GL-gated, audited.

---

### 0.10 V-3b-1c-2a as-built: the server host3d-ring path (2026-08-24)

The 1c-1 engine minted rings tapestryd-internally (the probe). V-3b-1c-2a wires
it into the `/srv/warp` SERVER so a HOST3D ring becomes a first-class ring flavor
under a client's warp ctx -- the tapestryd half of the client-claimable ring.
Per the 1c-2a/1c-2b split (section 0.9), this rung is the venus device-ctx
lifecycle + the ring flavor + teardown, provable by a tapestryd-side self-test
with NO client; the weft-share client claim + the `warp-prove` cross-Proc leg are
1c-2b. All in `usr/tapestryd/src/server.rs` (+ a one-line `gpu.rs` wrapper and a
`main.rs` call):

- **The per-client venus device-ctx.** `WarpCtx` gains `venus_ctx: Option<u32>`,
  **lazily** created on the first host3d ring mint (`wctx_venus_ensure`) via
  `gpu.ctx_create_venus` (capset-4) and destroyed with the warp ctx in
  `wctx_finish` (condemn-slot-on-refuse, before the dev_ctx destroy). A client
  that mints only V-3a guest-blob rings pays no venus ctx. The id is a dedicated
  band `WARP_VENUS_CTX_BASE (0x200) + slot` -- a `const _` gap assert + a
  `conv_attempt` `debug_assert` pin it disjoint from dev_ctx (`1..=MAX_WARP_CTXS`),
  the gpu probe ids (200-203), `COMPOSITOR_CTX` (0x100), AND the conv-probe
  throwaways (`CONV_PROBE_CTX_BASE = COMPOSITOR_CTX+1`, which the original
  `+slot` scheme would have aliased -- the enumerate-mirrors catch).
- **The HOST3D ring flavor -- ADD, not replace.** `ring/new` accepts
  `"<bytes> <ridx> host3d"` (a bare `"<bytes> <ridx>"` stays the V-3a guest-blob
  ring; an unknown third token is rejected). `wring_mint` gains a `host3d: bool`
  and, after the SHARED validation + I-32 backing-budget + ridx checks, branches
  to `wring_install_host3d` -- mint via the 1c-1 `mint_host3d_ring` under the
  venus ctx, install a `WarpRing { dma_fd: -1, host3d: Some(hr), .. }`. A
  venus-ctx or engine failure fails the mint CLEAN (the engine unwinds its own
  partial state; nothing installed).
- **Teardown routes to the engine.** `WarpRing` gains
  `host3d: Option<crate::gpu::HostRing>`; `wring_teardown` moves the non-Copy
  token into `drop_host3d_ring` and `return`s BEFORE the guest-blob
  res_unref/dma_fd path, so a host3d ring is never double-freed (the type system
  enforces the single drop). The weft share, when 1c-2b adds it, is disarmed
  first (already the top of `wring_teardown`).
- **The boot self-test (no client).** `warp_host3d_selftest` (called from
  `serve()` before READY, self-skipping like the gpu probes) mints a warp ctx
  under a synthetic conn, mints a host3d ring, round-trips a sentinel at the ring
  VA, and finishes the ctx -- exercising the venus-ctx create/destroy, the ring
  install, and `wring_teardown`'s host3d arm end to end. One line:
  `tapestryd: warp host3d-ring venus-ctx=<id> MAPPED+ROUNDTRIP teardown OK`
  (emitted ONLY on a successful round-trip; a mismatch emits `FAIL`, a
  2D/no-blob/no-venus device emits `skipped`). This asserts the SERVER WIRING,
  not host distinctness -- 1c-1's `hostmem_ring_probe` already proves the
  physical host-backing.

The `venus-verdict` gate gains a server-path leg (the `venus-ctx=` success line
on the test boot, its absence + a `skipped` line on the 2D control);
`tools/test-venus-verdict.sh` proves the discrimination without a boot (28/28),
including a `FAIL` sabotage. Deliberately NOT here: no client Proc maps the ring
yet (`ring/<ridx>/map` -> `WEFT_BIND_HOSTMEM` -> the client's `t_weft_map`), and
`wring_kick` still reads the V-3a `WARP_RING_OFF_*` header -- a host3d ring's
memory is Venus's format, so `wring_kick` must branch on `host3d` when a client
kick path exists; both are V-3b-1c-2b (unreachable at 1c-2a: no client fid).

### 0.11 V-3b-1c-2b design: the client-claim, and the F2 teardown-lifetime (design-first)

The 1c-2b-a *attempt* (an uncommitted delta over `3e12ef12`) went fully green --
build, 29/29 discriminator, and VENUS GATE VERIFIED on real V3D -- over a client
claim path that is **structurally dead**, and it was reverted. A Fable holotype
found two defects; F1 is a small bug, F2 is a lifetime design, and F1 must not
land without F2. This section is the design for both (operator-directed
2026-08-24: park was to give F2 an un-rushed pass, not to defer it).

#### 0.11.1 F1 -- the claim binding has no HOSTMEM arm (a half-widen)

FOUR kernel sites must admit a burrow kind for a weft share to be **claimable**;
V-2 widened three for HOSTMEM and missed the fourth:

- register gate (`syscall.c:6002`), kind decision (`weft.c:401`), client-map
  admit (`burrow.c:1374`) -- all admit `BURROW_TYPE_HOSTMEM && kobj_pci`.
- **`weft_binding_alloc_maponly` (`weft.c:472`)** gates on `burrow->type !=
  BURROW_TYPE_DMA -> return NULL` and handles only `kobj_dma->weave` / `gpu_bo`,
  else `return NULL`. A HOSTMEM burrow returns NULL, so the client's `t_weft_map`
  unwinds to -1. This is the exact half-widen the code's own comment warns about
  (`syscall.c:6004` "The two MUST widen together"), but the widen touched N-1 of
  the property set's members.

**Fix (no ABI):** add the `BURROW_TYPE_HOSTMEM && kobj_pci -> WEFT_BIND_HOSTMEM`
arm, mirroring the `gpu_bo` arm (create-immutable re-check), plus a kernel-test
leg that asserts the binding-kind (V-2's `test_weft_hostmem_share` asserts
create+kind+maponly+share_into but never calls the binding alloc -- sibling
symmetry without coverage symmetry, so the dead half was untested). Small, but it
ARMS F2: the instant a hostmem share is claimable, the teardown hazard is live.

#### 0.11.2 F2 -- the teardown-vs-live-mapping lifetime (the real work)

**The kernel side is already sound; the split is host-backing ownership.** A
hostmem ring's backing is a `BURROW_TYPE_HOSTMEM` burrow whose pages map a GPA in
the hostmem BAR (`hostmem_base + offset`). The #847 dual-count keeps that burrow
alive while a client maps it (`mapping_count > 0`), and the V-2 F1 death-quiesce
(`burrow.c:656`) keeps the PCI claim + the BAR MEM_SPACE decode alive for exactly
as long -- "a live client mapping holds this ref, so the claim stays alive across
the server's handle_close ... MEM_SPACE clears at the last unref, AFTER the
mapping is gone." So the client's PTEs stay valid as PTEs.

What breaks is one layer below the kernel: the GPA's **host bytes** are a
tapestryd-owned QEMU subregion (`map_blob` = `memory_region_add_subregion`,
`gpu.rs:2313`), and `drop_host3d_ring` (`gpu.rs:2429`) does `unmap_blob`
(`memory_region_del_subregion`) + `hostmem_free` (reclaim the offset)
**unconditionally**. So: client A maps GPA(off); A closes its `/srv/warp` conn but
keeps the VA (weft maps persist independent of the conn fid); `wctx_finish` ->
`drop_host3d_ring` deletes the subregion behind A's live PTEs and re-hands `off`;
client B's next `mint_host3d_ring` first-fits the SAME `off` -> `map_blob` places
B's subregion at GPA(off) -> A reads/writes B's live venus ring. A cross-client
I-37/I-45 breach. The teardown comment (`gpu.rs:2426`, "a client's own mapping
survives via its own ref") is true of the kernel Burrow OBJECT and FALSE of the
host bytes -- vacuous under 1c-2a, load-bearing-and-false the instant F1 lands.

**Prior art.** Plan 9 refcounts a shared `Segment` (freed at last detach); Fuchsia
refcounts a VMO and Genode a Dataspace -- pages live until the last handle AND
last mapping drop, and crucially the *kernel* owns the device-memory lifetime, so
there is no userspace free racing the refcount. Thylacine's #847 dual-count is the
same kernel-side lifetime; F2 exists only because ONE actor in the chain
(tapestryd, freeing the QEMU subregion) sits OUTSIDE that refcount -- a split the
capability microkernels avoid by keeping the whole lifetime in the kernel. The
tree's own nearest precedent is decisive: `image.c` reclaims a cached burrow only
at `burrow_handle_count(b) == 1 && burrow_mapping_count(b) == 0` (`image.c:116`) --
the exact "observe the mapping count before you reclaim" check, done kernel-side
because the image cache lives in the kernel. F2 needs the SAME check tapestryd-side.

**Options.**

- **(a) observe-and-reap [RATIFIED -- operator signoff 2026-08-24; syscall detail
  corrected post-audit, see 0.11.3].** On teardown, reclaim the ring's offset
  (`drop_host3d_ring`: detach VA + `unmap_blob` + `resource_unref` +
  `hostmem_free`) ONLY when no client references the GPA. In the common case
  (client died/unmapped first) the ref count is back to tapestryd's own map and
  reclaim is immediate. Otherwise PARK the whole ring on a reaper list (tapestryd
  keeps its VA mapped to re-query; the G-3 fence-reaper's park-and-reap shape),
  freeing when the last client reference drops. Correct, and **bounded**: a
  malicious client pins at most its own I-32 page budget of offsets (it cannot
  mint past its budget), never the whole region. This is `image.c`'s eviction
  check lifted to tapestryd -- which needs tapestryd to OBSERVE a kernel-computed
  count it cannot see today, via the `SYS_HOSTMEM_REFCOUNT` read-only syscall
  (0.11.3; it returns handle+mapping, not mapping alone -- the audit-F1 fix).

- **(b) leak-on-claim [rejected].** Never reclaim a claimed offset (never
  `unmap_blob`/`hostmem_free` a ring a client mapped). No ABI, trivially correct.
  But the leak is MONOTONIC over the daemon's life -- ~1024 rings at 256 KiB in a
  256 MiB region, ever -- and tapestryd is persistent (RW-7), so a desktop
  exhausts it after a few hundred Venus app-lifetimes and new rings fail
  `E_NOMEM`. The bounded version of (b) -- reclaim on the common path, leak only
  on the race -- IS option (a) (it needs the same observation to tell the two
  apart). So (b) as a standalone is inadequate for a persistent daemon.

- **(c) kernel owns the free [rejected].** Move the host-backing free into the
  kernel, gated by the burrow's own dual-count. Impossible here: `unmap_blob` is a
  tapestryd -> device controlq op (`RESOURCE_UNMAP_BLOB`), NOT a kernel operation.
  The kernel cannot issue it, so it cannot own the full lifetime -- the best it can
  do is TELL tapestryd when it is safe, which is (a).

#### 0.11.3 The ratified design (a)

1. **The observation syscall (`SYS_HOSTMEM_REFCOUNT`, VA-keyed).** tapestryd holds
   only a VA for a hostmem burrow (`burrow_from_hostmem` returns a VA, not a
   handle). The read-only syscall resolves that VA to its burrow and returns the
   burrow's TOTAL #847 reference count -- `handle_count + mapping_count` -- under
   `p->as->lock`. Shape: `SYS_HOSTMEM_REFCOUNT(va, len) -> i64` (>= 1 the count, <
   0 a `T_E_*` errno): it requires `[va, va+len)` to resolve to a SINGLE
   `BURROW_TYPE_HOSTMEM` VMA owned by the caller (else `-T_E_INVAL`) -- a caller
   can only count a hostmem burrow it maps. Because the caller maps it the sum is
   always >= 1, and `count == 1` iff the ONLY reference is the caller's single
   map.

   **The SUM, not `mapping_count` alone (audit F1 correction).** The design was
   ratified with `mapping_count`; the V-3b-1c-2b holotype proved that UNSOUND. A
   client's map is committed at `weft_share_claim` (`weft.c`), which consumes the
   share and TRANSFERS the registration pin -- a `handle_count` ref -- to the
   client, and returns BEFORE `burrow_share_into` bumps `mapping_count` later in
   the same `SYS_WEFT_MAP`. In that window a client is irrevocably going to map
   GPA(off) yet `mapping_count` still reads 1, so a reclaim keyed on it would free
   the offset under the pending map (a cross-client alias). This is exactly why
   `image.c`'s eviction gate is `handle_count==1 && mapping_count==0` -- the
   `handle_count` half excludes the in-flight mapper. The SUM folds both halves to
   one value for the tapestryd side (where the caller holds the mapping, not the
   handle): the transferred pin makes the sum >= 2, closing the window.

   Rejected alternatives (recorded): a handle-keyed form (would widen the V-2
   return ABI to also hand out a KObj), and a pollable "fully unmapped" event fd
   (a new pollable kernel object). The VA-keyed form is the smallest ABI and
   reuses the VA tapestryd already holds. **Non-goal:** NOT general
   burrow-introspection -- it answers only "the total ref count of the hostmem
   burrow under this VA," no other burrow type, other Proc, per-kind breakdown, or
   kernel address (the KASLR surface stays closed).
2. **Teardown split** in `wring_teardown` -> `retire_host3d_ring`: disarm the weft
   share (already the top of `wring_teardown`, so no NEW claim can consume it),
   then read the ref count. At count==1 reclaim NOW (`drop_host3d_ring`: detach VA
   + `unmap_blob` + `resource_unref` + reclaim the offset). At count > 1 PARK the
   WHOLE `HostRing` (VA kept mapped, resource + offset held) so the VA-keyed syscall
   can re-query it; `reap_hostmem_parked` runs `drop_host3d_ring` when the count
   falls to 1. Nothing is freed while a client reference (map OR pending-claim pin)
   is live, so the subregion always outlives any client mapping.
3. **The reaper** re-checks parked extents at MINT-time (reclaim-before-alloc),
   NOT the completion pump: reclaim issues controlq teardown (`drop_host3d_ring`),
   established safe only from the serve-loop request context mint runs in -- moving
   it into the pump would need a re-entrancy re-examination (audit F2). Mint is
   also where offset pressure arises, so reclaim happens exactly when the space is
   needed. A per-pass cap (`HOSTMEM_REAP_PER_PASS`) bounds the controlq teardowns
   one mint issues. A parked extent whose client never unmaps is bounded by that
   client's I-32 budget and is freed at the client's Proc death (address-space
   teardown drops the mapping + releases the pin -> count 1).

#### 0.11.4 Invariants, tests, and the split

- **I-7** (BURROW dual-count): the host bytes now outlive the last client mapping,
  matching the kernel Burrow's own lifetime -- the split is closed.
- **I-37 / I-45**: an offset is never re-handed while a client maps its GPA, so no
  cross-context aliasing; the ring stays bounded to the ctx owner.
- **I-32**: parked extents are charged to the pinning client's budget; the region
  cannot be exhausted by one client past its cap.
- **Tests (as-built)**: `weft.hostmem_share` -- the F1 binding-kind kernel leg
  (0.11.1), the `weft_binding_alloc_maponly` HOSTMEM arm returns a
  `WEFT_BIND_HOSTMEM` binding. `weft.hostmem_refcount` -- the `SYS_HOSTMEM_REFCOUNT`
  core, and critically the audit-F1 window: a burrow with the transferred claim
  pin but NO mapping reads total 2 (PARK) though `mapping_count` alone is still 1,
  so the reap predicate itself is now covered kernel-adjacent (the F3 gap). The
  boot self-test witnesses `refcount=1` on the freshly-minted ring (the reap-safe
  arm) on GL. **Deferred (tracked follow-on)**: the `warp-prove ring-host3d`
  cross-Proc E2E (0.9 piece 4) -- a real client Proc claims + maps + the full
  cross-client-alias reproduction; the kind-specific pieces are kernel-tested here
  and the `SYS_WEFT_MAP` wrapper is kind-agnostic, so the claim path rides tested
  machinery.
- **Split**: F1 + F2 land together (F1 alone un-masks F2). The new syscall is
  audit-bearing (kernel ABI + the section-25.4 Warp row).

**Ratified 2026-08-24 (operator signoff):** option (a) observe-and-reap, with the
VA-keyed `SYS_HOSTMEM_REFCOUNT` read-only syscall (0.11.3; ratified as
`_MAPCOUNT` returning mapping_count, corrected to the handle+mapping SUM after the
V-3b-1c-2b holotype proved mapping_count alone misses the in-flight claim -- the
approach is unchanged, the count is the full image.c predicate). This section is
the pin; the impl commit (F1 + syscall + F2 teardown/reaper + the client claim
path, landed together and audited) references it. Impl: `7696540a` (initial) +
the audit-close.

### 0.12 V-3b-2 design: the SUBMIT_CMD forward + the ring bootstrap (design-first)

The client-claimable HOST3D ring (0.9-0.11) is minted, mapped, and lifetime-safe,
but nothing yet tells virglrenderer to POLL it. V-3b-2 forwards the venus
SUBMIT_CMD stream -- chiefly the ring-bootstrap command `vkCreateRingMESA` -- from
the client to virglrenderer, so the host maps the same shmem and begins polling.

**Source-grounded (spike 2026-08-25: Mesa main @`0cd184e9` + virglrenderer main
@`7fcfce49` + venus-protocol @`e94b12f3`, the revision Mesa's wrap pins):**

1. **SUBMIT_CMD is `DRM_IOCTL_VIRTGPU_EXECBUFFER`**, dispatched host-side by
   `vkr_context_submit_cmd` (`vkr_context.c:164-186`) against the CONTEXT decoder.
   Only four commands ride it -- `vkCreateRingMESA` / `vkDestroyRingMESA` /
   `vkNotifyRingMESA` / `vkSubmitVirtqueueSeqnoMESA` -- everything else threads
   through the ring (polled). All four are `<proto>void`: fence-signaled, NO data
   reply (`VK_MESA_venus_protocol.xml:165-211`; `vkr_transport.c:187-351` writes
   none).
2. **Ring layout** (`vn_ring.c:257-283`, host view `vkr_transport.c:110-175`):
   five disjoint 4-byte-aligned regions in the HOST3D shmem -- head@0 (consumer,
   host writes), tail@64 (producer, guest writes), status@128 (IDLE/FATAL/ALIVE
   bits), buffer@192 (128 KiB power-of-two command stream), extra@192+128KiB (a
   4-byte host->guest scratch, `vkWriteRingExtraMESA`). NO reply region lives in
   the ring.
3. **`vkCreateRingMESA` references the ring by its virtio-gpu `res_id`** (the
   HOST3D mappable blob's, minted + registered by 0.8/0.10) via
   `VkRingCreateInfoMESA{resourceId, offset=0, size=shmem_size, idleTimeout,
   {head,tail,status,buffer,extra}Offset/Size}` (`vn_ring.c:359-379`); the host
   requires FD_SHM (`vkr_transport.c:200-204`). A ~124-byte (140 w/ the monitor
   pNext) PURE-serialization encode -- no Vulkan object, no live instance; Mesa
   encodes it into a 256-byte stack buffer before any instance exists.

**The design.**
- **The forward plumbing ALREADY EXISTS** (Warp-2/C): `ctx/<id>/submit` (`WFK_SUBMIT`)
  -> `warp_submit` -> `warp_fenced_admit` -> `gpu.submit_3d(dev_ctx, pub, stream)`
  on the fenced lane, opaque bytes. **But it targets `c.dev_ctx` (the VIRGL ctx),
  and the venus stream must target `c.venus_ctx`** -- a host3d ring's resource is
  created under the venus ctx (`wring_install_host3d` mints via `wctx_venus_ensure`
  -> `mint_host3d_ring(res, venus_ctx, ...)`), so `vkr_context_get_resource` resolves
  its `res_id` ONLY on the venus context's decoder. (Spike-traced 2026-08-25: NO
`CTX_ATTACH_RESOURCE` is needed -- for venus, RESOURCE_CREATE_BLOB dispatched on the
venus ctx IS the attach, inserting the res_id into that ctx's `resource_table`, the
same table `vkr_context_get_resource` reads; Mesa's real driver does the identical
flow with zero attach. The res_id resolves by CO-LOCATION on one venus ctx -- exactly
I-45, the ctx as the resource scope.) So V-3b-2's forward delta is a
  **venus-ctx-targeted submit** (`gpu.submit_3d(c.venus_ctx, c.pub, bytes)`, ensuring
  `venus_ctx` first), reusing the SAME fenced lane + admission. Shape (impl call):
  either a distinct `ctx/<id>/venus` verb, or `submit` routing to `venus_ctx` when the
  ctx has one armed -- decided at impl (a Venus client uses only the venus path, so
  the routing is unambiguous per-client; the distinct-verb option is the fallback if
  a single ctx ever mixes virgl + venus streams). tapestryd NEVER parses the stream
  (opaque; the venus ctx is the host-side resource SCOPE, not command parsing -- the
  0.6 venus-agnostic principle). Bounded by `warp_fenced_admit` + a `WARP_SUBMIT_MAX`
  byte cap (I-32).
- **The bootstrap** is the client submitting `vkCreateRingMESA` (res_id = its
  host3d ring's) once; virglrenderer's `vkr_dispatch_vkCreateRingMESA` starts its
  poll thread (`vkr_ring.c:351`).
- **The doorbell** is `vkNotifyRingMESA` via the same `submit`, sent by the guest
  ONLY when it observes `status & IDLE` after publishing tail (the seq_cst
  register-then-observe, symmetric: guest stores tail seq_cst then loads status
  seq_cst `vn_ring.c:446-491`; host sets IDLE seq_cst then re-reads tail
  `vkr_ring.c:270-300`). Our side merely forwards -- the I-9 handshake is
  virglrenderer's + the guest's, over the cache-coherent shmem 0.7/0.8 already
  established.
- **Fence/completion**: `gpu.submit_3d` returns a CTX fence id, retired by
  `warp_service_fences` and delivered by `poll_fences` on the SAME `ctx/<id>/fence`
  surface a virgl submit uses -- NOT `poll_ring_fences` (`completed_seq`), which is
  the V-3a echo-drain's RING-fence surface, unused by the host3d submit path (this
  corrects an earlier draft of this bullet). The bootstrap commands are void, so a
  client typically POLLS the ring `status` shmem for the witness (virglrenderer's
  own poll thread sets it) rather than waiting the fence; the fence is the lane's
  ordering.
- **The host-side rescue (round-3 F1 OWED, [[design-v3b-ring-kick-rescue-owed]]) --
  DISCHARGED for this path (sub-step B finding, impl 2026-08-25).** The mechanism
  0.12 anticipated as "genuinely new" -- a bounded serve-loop follow-up drain for
  the fenced-submit path -- already EXISTS as `warp_service_fences` (built at W2d):
  it runs every serve-loop iteration (`main.rs`), is bounded per pass by
  `FENCED_SLOTS` (16, the device fence-slot ring), and `warp_venus_submit` reuses
  it byte-identically to `warp_submit` (admission caps in-flight at
  `WARP_CTX_FENCE_MAX`=8/ctx; the drain retires <=16/pass; neither can pin the
  serve thread). So NO new mechanism lands for the fenced-submit path -- the
  verification IS the deliverable. The round-3 F1 note's LITERAL subject is a
  DIFFERENT path: the V-3a echo drain in `wring_kick` (non-host3d rings), which
  Model B does not route through `gpu.submit_3d` (a host3d kick returns
  `E_OPNOTSUPP`; virglrenderer polls the ring). That path's cap-and-re-kick
  contract is documented + prover-honored (warp-prove leg 8); its own robust rescue
  is a robustness-NOT-soundness item (a misbehaving client strands only its OWN
  ring's fence read -- no cross-client breach, no corruption, the cap bounds the
  serve thread) on a superseded POC ring ("the V-3a ring is not Venus's ring",
  `34dbe5d3`) -- TRACKED + deferred, not part of V-3b-2.

**Scope: the reply-shmem SPLITS OFF (this corrects 0.4).** Replies from
ring-executed BULK commands go to a SEPARATE client-registered reply shmem
(`vkSetReplyCommandStreamMESA` + a second FD_SHM resource; `vn_ring.c:678-742`),
NOT the ring buffer or the extra region. Since the four SUBMIT_CMD bootstrap
commands return nothing, V-3b-2 needs NO reply plumbing. The 2026-08-25 design
pass (section 0.13) then traced the reply mechanism end-to-end and found it needs
no new Thylacine substrate at all -- it is a second host3d ring + a RING command,
entirely Mesa-side -- so it folds into V-3b-3, not a separate V-3b-2b chunk.

**Witness (standalone, GL-only).** A `warp-prove ring-host3d` verb: mint a host3d
ring (0.9), hand-build a ~124-byte `vkCreateRingMESA`, submit it via
`ctx/<id>/submit`, and observe virglrenderer set `status & IDLE` after
`idleTimeout` on the empty ring (`vkr_ring.c:270-278`) -- proof the host mapped
the shmem and polls, with NO Mesa. Optionally write one command into the buffer +
observe `head` advance from 0 (`vkr_ring.c:302-317`). Local 2D devices skip (no
venus ctx), like the existing self-test.

**Invariants.** I-45: the submit is ctx-scoped -- opaque bytes to the ctx's own
`venus_ctx`, one ctx per conn, no cross-ctx naming (the 0.9 pin). I-9: the idle/kick
register-then-observe, upheld by virglrenderer + the guest across the coherent
shmem (our forward is transparent to it). I-32: `WARP_SUBMIT_MAX` bounds a
submit; the fenced-lane admission bounds concurrency.

**RESOLVED at impl (sub-step C spike, 2026-08-25 -- byte-exact from Mesa's OWN
generated encoder, compiled + run, not hand-derived).** venus-protocol
`e94b12f3` (Mesa main's pin) + virglrenderer `7fcfce49`:
- **`vkCreateRingMESA` = 124 bytes, bare (NULL-pNext) form ACCEPTED.** Framing:
  `[cmd_type=188 u32][cmd_flags=0 u32][ring cookie u64][pCreateInfo present u64=1]`,
  then `VkRingCreateInfoMESA{sType=1000384000, pNext=NULL(8B 0), flags, resourceId,
  offset, size, idleTimeout, headOffset, tailOffset, statusOffset, bufferOffset,
  bufferSize, extraOffset, extraSize}`. All words host-LE; every offset/size is a
  size_t = 8 bytes on the wire. Corrections vs the pre-spike guess: `flags` comes
  FIRST (before resourceId); there is an `idleTimeout` u64 between `size` and
  `headOffset`; head/tail/status are OFFSET-only (no `*Size`); there is NO
  `alignment`/`bufferChunkSize`/`numExtraOffsets`. Stock Mesa chains a
  `VkRingMonitorInfoMESA` pNext (140 B) but virglrenderer's
  `vkr_dispatch_vkCreateRingMESA` takes the bare form -- the monitor only starts
  the separate ALIVE watchdog, unneeded here.
- **Witness = status word (u32 @ statusOffset=128).** Bits (VK_MESA_venus_protocol.xml):
  IDLE=0x1, FATAL=0x2, ALIVE=0x4. virglrenderer's poll thread (`vkr_ring.c:270-278`)
  sets IDLE when it finds the ring idle; on a zeroed-then-created EMPTY ring with
  `idleTimeout=0` it sets IDLE on its first poll iteration. Create REQUIRES
  `*head==0 && *status==0` (`vkr_ring.c:53`) -- the install-time zeroing satisfies
  it. So `(status & 0x1) && !(status & 0x2)` on a zeroed-then-created ring proves
  the host mapped the resource AND ran its poll loop, with NO Mesa. FATAL=0x2 is a
  decode/layout rejection (a distinct fail); `status != 0` is NOT the assertion
  (FATAL is also nonzero). Head-advance is NOT needed -- the IDLE bit is the
  single sound assertion. Byte table + citations: `[[design-v3b2-submit-forward]]`.

**Ratified 2026-08-25 (operator signoff):** the SUBMIT_CMD forward + ring
bootstrap + host-side rescue, standalone GL-witnessed, with the reply-shmem
deferred. This section is the design pin; the impl commit (the `ctx/<id>/submit`
interface + the rescue + the `warp-prove` witness, then the Fable audit round)
references it.

**AS-BUILT (2026-08-25):** A `836855da` (forward), B `c1477a91` (rescue
DISCHARGED -- the "genuinely new" mechanism already existed as
`warp_service_fences`), C `84ac8a27` (`warp-prove ring-host3d` GL witness
VERIFIED on thyla-pi V3D). **Audit round 1** (Fable 5, `4e5a1a40`): 0 P0 / 1 P1 /
0 P2 / 3 P3, all fixed -- the forward sound on every lane; F1 [P1] was the
1c-2a wedge-arm venus_ctx destroy whose "quiesced by construction" premise THIS
chunk voided (venus_ctx now carries live fenced chains), moved to the vindication
path (`warp_ctx_venus_vindicate`), exactly as the 1c-2a code's own forward comment
had named V-3b-2 to do. **Dirty close -> audit round 2** (Fable 5, scoped to the
F1 restructure): 0 P0 / 0 P1 / 0 P2 / 2 P3, all fixed -- CLEAN. The restructure is
sound on all five focus lanes (`take_vindications` single-drain -> no
partial-destroy re-fire; both submit paths tag fences by `ctx_pub` so the
poisoned-slot gate covers venus chains; the clean-arm quiesce premise verified).
F1 [P3]: the vindication now attempts BOTH ctx destroys (was: `continue` on a dev
refuse, skipping venus -- asymmetric with the clean arm). F2 [P3]: the
ring-teardown comment stated the dead V-3a "no submit lands" premise (the same rot
class as round 1's F1) -> rewritten to the real safety chain (host-memory backing,
FIFO controlq, trusted-host renderer, monotonic res_id) + the v3d-fork obligation
to defer the host3d-ring unref once the renderer is ours. Full closed list:
`[[audit-v3b2-r2-closed-list]]`.

### 0.13 V-3b-2b reply-shmem design pass (2026-08-25): no new substrate; folds into V-3b-3

"Open the reply-shmem design pass" (operator, 2026-08-25). The pass traced
`vkSetReplyCommandStreamMESA` + the reply command stream end-to-end on both sides
(Mesa working fork `mesa-26.1.6-7-gb7f9ed2` + virglrenderer `7fcfce49`). The
conclusion is a FINDING, not a build: **the reply-shmem needs no new Thylacine
substrate or ABI.** It reverses the earlier "new FD_SHM ABI -> design-fork"
framing.

Source-grounded, the reply path owes Thylacine exactly two things, both already
in the tree:
- **A reply region** = a second host3d mappable blob on the client's venus ctx,
  weft-shared. That is the existing `ring/new host3d <bytes> 1` verb
  (`WARP_RINGS_PER_CTX` = 64) + the existing `wring_weft_ensure`. Its virtio-gpu
  `res_id` is exposed by the existing `ctx/<id>/ring/<ridx>/info` `res` field (the
  V-3b-2 `warp-prove ring-host3d` witness already reads ring 0's this way).
- **Nothing else.** `vkSetReplyCommandStreamMESA` (cmd_type 178, a 36-byte
  `VkCommandStreamDescriptionMESA{resourceId, offset:u64, size:u64}` payload) is a
  RING command Mesa writes INTO the command ring, consumed in-order by
  virglrenderer's poll thread -- NOT a SUBMIT_CMD, so `warp_venus_submit` is not
  involved and tapestryd forwards nothing. The host writes each command's reply
  ZERO-COPY into the client's mapped reply pages; the guest learns a reply is
  ready by polling the ring `head` index (release on the host store, acquire on
  the guest load -- `vkr_ring.c:60-67` / `vn_ring.c:92-99`), NOT a fence or a
  status word in the reply region. tapestryd is in neither the reply-write nor the
  sync path.

Mechanism specifics that pin the above: the reply region is ONE per-VkInstance
bump-suballocated blob (1 MiB, grows by doubling), re-registered before every
reply-bearing command; created at instance-create (before the ring) and destroyed
at instance-destroy (after the ring); each reply refcounts the backing blob, and
the head-seqno gate guarantees the host has finished writing before the guest
reads/reuses (no mid-write UAF on teardown). 134 of ~325 commands are
reply-bearing (`vn_call_*`), including ALL of bring-up (`vkCreateInstance`,
`vkCreateDevice`, `vkEnumeratePhysicalDevices`,
`vkGetPhysicalDeviceMemoryProperties`, `vkAllocateMemory`) -- so the reply stream
is mandatory for bring-up, and there is no cheap pre-Mesa witness (the simplest
reply-bearing command needs a live host Venus instance, i.e. Mesa).

Invariant posture (all COVERED by "a second host3d ring", audited at V-3b-3): the
reply region is a HOST3D blob on the client's own venus ctx (I-45 ctx-bounded),
charged to `WARP_CTX_BACKING_MAX` alongside the command ring (I-32), parks/reclaims
via the same dual-refcount path now cross-Proc-witnessed at `b7b712dc` (I-7),
weft-shared over the F1 HOSTMEM binding (I-37). The one new behavior -- the HOST
writes into it (the command ring is guest-written) -- is bounded by construction:
virglrenderer bounds-checks `offset + size <= res->size` and fatals the ctx on
overflow (`vkr_cs.c:23-29`), writing only into the client's OWN region at the
client's OWN chosen offset; a hostile guest under-provisioning `reply_size` kills
only its own ctx (I-45 fault isolation), never a cross-client breach.

**Ratified 2026-08-25 (operator signoff):** reply-shmem folds into V-3b-3 (the
Mesa `vn_renderer` backend), witnessed by real Venus bring-up; there is no
separate V-3b-2b chunk. Full research record:
`scratchpad/v3b2b/REPLY-SHMEM-RESEARCH.md` (the superseded pre-fork skeleton) +
`[[design-v3b2-submit-forward]]`.

### 0.14 V-3b-3 design: the Mesa vn_renderer backend (design-first, 2026-08-25)

Operator-ratified 2026-08-25 as the arc endpoint. `vn_renderer_thylacine.c`
(~1-1.5 kLOC) implements `struct vn_renderer` over the shipped Model B substrate.
A source read of `mesa-thylacine` @`b7f9ed2` (= mesa-26.1.6+7) + `virglrenderer`
`7fcfce49` settled the shape.

**Reconciliation (sections 4 + 3.4-3.6 are V-3a-framed; THIS governs).** Those
sections put tapestryd in the ring hot path (a ring thread that echoes head..tail
to `gpu.submit_3d` and writes the feedback slot). Model B (section 0) moved that
OUT: virglrenderer POLLS the HOST3D ring directly; tapestryd runs NO ring thread,
writes NO feedback slot, does NO echo-drain -- it only mints/maps the ring
(coherent shmem), forwards the bootstrap SUBMIT_CMD (`warp_venus_submit`), and
retires fences (`warp_service_fences`). The idle/kick/head/status protocol is
virglrenderer <-> the Venus driver over the coherent shmem. So the backend
implements `vn_renderer` over /srv/warp and re-implements NONE of that machinery.

**The backend is vtest-shaped, not virtgpu-shaped.** Upstream has two backends:
`vn_renderer_virtgpu.c` (DRM ioctls) and `vn_renderer_vtest.c` (a userspace
socket transport, no kernel driver). A /srv/warp backend is the VTEST shape with
9P file ops replacing the socket and `t_weft_map` replacing the SCM_RIGHTS-fd +
mmap. It reuses `warp_client.c` (the Warp-3 transport, 417 LOC: `t_open` /
`t_write` / `t_pread` / `t_weft_map` / the `issued`/`signaled` fence-counter model
/ the wedge latch), reached through the cross-file's `-I$ROOT/usr/lib/libt/include`
(`<thyla/syscall.h>`). `t_poll` (SYS_POLL=29) and `t_weft_map` (SYS_WEFT_MAP=82)
both exist -- the `ops.wait` ns-timeout is buildable.

**The `vn_renderer` contract = 20 function pointers** (`vn_renderer.h`), 4
sub-vtables + the `info` struct (NOT a callback; a private `_init_renderer_info()`
fills it from the caps blob): `ops`{destroy, submit, wait}, `shmem_ops`{create,
destroy}, `bo_ops`{8}, `sync_ops`{7}. Exactly 3 are optional (the socket backend
NULLs `bo.create_from_dma_buf`, `sync.create_from_syncobj`, `sync.export_syncobj`)
-> 16 mandatory data-plane + the info-init = the "19-fn/16-mandatory" the doc
sketched; 20 pointers is the ground truth. Shared helpers reused verbatim:
`vn_renderer_shmem_cache_*`, `vn_renderer_bo_export_sync_file_internal`,
`vn_renderer_submit_simple_sync`, `vn_renderer_shmem_pool_*`.

**Op -> substrate (Model B):**
| vn_renderer op | Thylacine primitive (BUILT unless noted) |
|---|---|
| shmem_ops.create/destroy | `ring/new host3d <bytes> <ridx>` -> HOST3D blob on venus_ctx + `t_weft_map`; res_id via `ring/<ridx>/info` `res` |
| ops.submit (execbuffer) | `ctx/<id>/submit` -> `warp_venus_submit` (opaque bytes on venus_ctx, fenced lane) + the ring bootstrap (`vkCreateRingMESA`) |
| ops.wait | `t_poll` on `ctx/<id>/fence` w/ the Vulkan ns timeout; the fast path polls the guest feedback slots |
| sync_ops (u64 timeline) | the SIMULATE_SYNCOBJ feedback slots in the coherent shmem (guest-side) |
| bo_ops (HOST_VISIBLE VkDeviceMemory) | the hostmem BAR: `MAP_BLOB` + `SYS_BURROW_FROM_HOSTMEM` (V-2/V-3b-1b) |
| info (get_info) | the retained Venus capset blob (the `caps` file) |
| reply stream | a 2nd `ring/new host3d ... 1` + the `vkSetReplyCommandStreamMESA` RING command (section 0.13) |

**Two build blockers (new work, distinct from backend logic):**
1. The cross-file sets `system='linux'` -> `system_has_kms_drm=true` -> the venus
   meson pulls in `vn_renderer_virtgpu.c` + a hard `dep_libdrm` the pouch sysroot
   cannot satisfy. Gate them out (build vtest + thylacine only) in
   `src/virtio/vulkan/meson.build`.
2. The ICD is upstream a `shared_library`, and Thylacine is static/no-loader
   (`kernel/elf.c` requires ET_EXEC, no PT_DYNAMIC). Build a LOADER-LESS ICD entry
   (named in the GPU-DESIGN Vulkan-adaptation note, unbuilt) -- the static
   registration of the driver's `vk_icdGetInstanceProcAddr` without dlopen. This
   is the novel piece + the highest risk; it lands in 3a.

**Sub-chunk plan (operator-ratified 2026-08-25):**
- **V-3b-3a -- build path + loader-less ICD + backend skeleton.** Clear the 2
  blockers; add `vn_renderer_thylacine.c` (shmem_create + info-init + destroy +
  the mandatory op stubs); wire `-Dvulkan-drivers=virtio` on thyla-keep. Milestone:
  the ICD builds STATIC + loads loader-less + enumerates the Thylacine Venus
  driver (a link+load proof, the `virgl_prove.c` analog). Highest risk.
- **V-3b-3b -- shmem + submit + wait + sync (bring-up). AS-BUILT (verified on
  thyla-pi/KVM, real V3D).** `vkCreateInstance` + `vkEnumeratePhysicalDevices`
  (2 Venus devices, `Virtio-GPU Venus (V3D 4.2.14.0)`) +
  `vkGetPhysicalDeviceMemoryProperties` + `vkDestroyInstance` round-trip, boot
  green. Two dependencies surfaced + pulled forward: **(a) the VENUS capset must
  be SERVED** -- the existing `caps` file serves the RANKED virgl capset (the GL
  winsys reads it, virgl_thylacine_winsys.c:529), so re-ranking was out;
  tapestryd now fetches + serves the venus capset on a separate `caps-venus`
  file (V-3c's SERVING half pulled forward; the enforcement half stays V-3c).
  **(b) the smoke must run POST-WARDEN** (tapestryd serving /srv/warp), not in
  joey's pre-warden boot-test suite where 3a placed it (3a needed no transport).
  **Correction to the op table above:** `wait` is a BLOCKING Tread of
  `ctx/<id>/fence` (via `warp_fence_wait`), NOT `t_poll` -- the seam's qids carry
  no QTPOLL, so t_poll returns POLLIN immediately and would busy-spin. sync +
  wait are on the TEARDOWN path only (vkDestroyRingMESA); bring-up's replies are
  pure guest-side ring-head polls (no `vn_renderer_wait`).
- **V-3b-3c -- bo / device memory.** The HOST_VISIBLE bo path over the hostmem
  BAR; milestone: `vkAllocateMemory` + `vkMapMemory`. **Split into
  audit-bearing sub-chunks** (sequencing within this ratified arc): 3c-1 closes
  the V-3b-3b audit's owed P1 (F1) so device-memory churn lands on a sound ring
  lifecycle; 3c-2 is the bo milestone, itself split into 3c-2a (the tapestryd
  server-side `mem/` substrate + a gate-wired boot self-test, a Mac+pi loop, no
  mesa rebuild) and 3c-2b (the mesa `vn_renderer` bo_ops + the vkAllocateMemory
  E2E on real V3D). The split reflects that the substrate is a ~200-LOC change
  across ~24 sites in the 11.6k-line audit-bearing server.rs -- a substantial
  independently-reviewable unit, whose tight audit gives a known-good base before
  the remote E2E work.
  - **V-3b-3c-1 -- the F1 full fix (per-ring destroy verb). AS-BUILT.** The
    V-3b-3b interim made the backend's host3d ring-slot (ridx) alloc MONOTONIC
    because tapestryd retired a host3d ring only at ctx death; a reused ridx
    collided with the still-installed server slot. 3c-1 adds a client-invocable
    `ring/<ridx>/ctl destroy` verb (`WFK_RING_CTL`, tapestryd `wring_destroy`):
    it TAKES the WarpRing out of its ctx slot (freeing the slot for re-mint) then
    runs the existing `wring_teardown` (disarm the weft share ->
    `retire_host3d_ring`: observe-and-reap the hostmem backing). Ownership-gated
    by the same conn scan as `wring_kick` (I-45). The backend
    (`thylacine_shmem_destroy_now`) unmaps -> issues the destroy verb -> frees
    the guest ridx ONLY on the verb's success, preserving "guest ridx free <=>
    server slot free" (a refused destroy falls back to the interim's
    leak-until-ctx-death for that one slot -- fail-safe). Because the backend
    `t_close`s the map fid before the destroy RPC, the client mapping is already
    gone when `retire_host3d_ring` reads `SYS_HOSTMEM_REFCOUNT` == 1, so the ring
    reaps immediately (not parked). Regression witness (the bring-up was blind to
    it -- it never re-mints a ridx): a tapestryd boot self-test
    (`warp_ring_recreate_selftest`) mints at ridx 0, destroys, asserts the slot
    freed, re-mints at ridx 0 -- witnessed `warp ring-recreate ridx-reuse OK` +
    `THYLACINE-VENUS-PROVE PASS` + `Thylacine boot OK` on thyla-pi/KVM real V3D.
    **F5 (the wait ns-timeout) stays a documented P3 deferral** (no live
    finite-timeout caller; teardown wants completion). Impl: tapestryd
    `d8d969b3` + mesa `77fc80a` (patch 0012, round-trip `c317dd63`). Audit:
    holotype Fable 5, 0 P0 / 0 P1 / 1 P2 / 2 P3, not dirty (F1 [P2] = the
    ridx-reuse witness was gate-invisible -> wired into boot-probe + the venus
    gate + test-venus-verdict; F2/F3 [P3] comment fixes, F2 convergence code
    deferred to 3c-2). `memory/audit_v3b3c1_closed_list.md`.
  - **V-3b-3c-2a -- the server-side device-memory substrate. AS-BUILT.** A lean
    new `mem/` subtree on /srv/warp (`ctx/<id>/mem/new` write-verb
    "<bytes> <handle> <mem_id>"; `mem/<handle>/{info,map,ctl}`) exposing a
    HOST_VISIBLE device-memory blob -- the persistent HOST3D engine
    (`mint_host3d_ring`) with blob_id = the Venus mem_id, weft-shared to the
    client (WEFT_BIND_HOSTMEM). DESIGN FORK (decided this session, within the
    ratified milestone; not scripture-altering -- it follows the established Warp
    exposure precedent, where the ring tree was designed as-built + audited, not
    via a separate scripture-first conversation): a lean `mem/` subtree (new qid
    tag `WARP_MEM`=1<<44, `struct WarpMem`, a `mems[]` row) OVER a host3d flavor
    on the compositor/leak-park-complex `bo/` tree -- isolation-safety beats ABI
    economy on an I-45 surface. Ring-shaped, NOT bo-shaped: a one-step write-verb
    mint (client-owned handle 0..MAX_WARP_MEMS_PER_CTX=256, monotonic pub_id in
    the qid) with no control header / doorbell / fence / geometry -- device
    memory is the client's to write, so there is no unbuilt-corpse state (the
    bo/create3d #218 hazard has no analog). The I-32 cap is made HOLISTIC
    (`ctx_backing_total` = bos + rings + mems + leaked, on all three mint paths),
    closing a pre-existing hole where `wbo_create` summed bos + leaked ONLY
    (ignored rings). The backing is zeroed at mint (the 1c-2b disclosure floor:
    the hostmem free-list hands reclaimed offsets back verbatim). Teardown
    (`wmem_teardown`, by-value consume) disarms the weft share BEFORE
    `retire_host3d_ring` (reap-if-safe-else-PARK on the client-map refcount), the
    I-7 #847 order; no `retiring`/fence field (device memory carries no
    tapestryd-tracked fence -- the client frees it under Vulkan valid-usage).
    Gate-wired boot self-test (`warp_mem_selftest`: alloc -> sentinel round-trip
    through tapestryd's own RW map -> destroy -> handle-reuse; boot-probe filter +
    venus gate + test-venus-verdict, 34/34). Witnessed on thyla-pi/KVM real V3D:
    `warp mem-recreate handle-reuse OK`. **Audit** (holotype Fable 5 max,
    MODEL(start)==MODEL(end)): **0 P0 / 0 P1 / 0 P2 / 4 P3, all fixed, CLEAN.** The
    chunk is a faithful transplant of the closed ring pattern; every mem-specific
    deviation was checked and found to STRENGTHEN the ring properties. F1 [P3]: the
    ctl cap comments were falsified by the holistic cap + the enforced quantity was
    unobservable -> comments fixed + a `backing-bytes` ctl key emits
    ctx_backing_total. F2 [P3]: `wctx_has_venus`'s "iff it minted a host3d ring"
    comment falsified by wmem_mint (the 2nd venus armer) -> extended. F3 [P3]: the
    64 MiB per-word-SeqCst disclosure zeroing was a client-repeatable latency lever
    -> `write_bytes` at both host3d sites (the codebase's alloc_weave method). F4
    [P3]: the mint's zeroing bakes a client-ordering contract 3c-2b could violate
    (lazy-mint-after-GPU-write zeroes results) -> documented on wmem_mint/WarpMem
    ("mint at vkAllocateMemory time, before any device use of mem_id"). Impl:
    tapestryd `54e2f334`. `memory/audit_v3b3c2a_closed_list.md`. The mesa backend
    bo_ops + the `vkAllocateMemory`+`vkMapMemory` E2E are V-3b-3c-2b (next).
  - **V-3b-3c-2b -- the mesa vn_renderer device-memory bo_ops + the E2E.
    AS-BUILT.** The three pre-wired bo_ops filled over the `mem/` ABI:
    `create_from_device_memory` (page-rounds + guards <= 64 MiB, allocs a
    client handle from a 256-slot `mem_bitmap`, `warp_mem_new`
    "<bytes> <handle> <mem_id>" with blob_id = the Venus memory object id,
    stores the bo in bo_array keyed by res_id); `bo_map` (lazy `warp_mem_map`
    -> `t_weft_map`, cached -- the client holds no PCI handle, so weft not
    SYS_BURROW_FROM_HOSTMEM); `bo_destroy` (unmap -> `warp_mem_destroy` -> free
    the handle ONLY on a confirmed server retire, the two-sided invariant
    mirroring the ring ridx -- a create-error handle stays marked, so a re-handed
    handle can never collide with a still-installed server slot, the ring-F1
    wedge class). Transport `warp_mem_{new,map,unmap,destroy}` mirror the ring
    verbs. The F4 mint-eagerly contract is honored by construction: the driver
    calls create_from_device_memory at vkAllocateMemory time for HOST_VISIBLE.
    **Two masking-bug layers under the first vkCreateDevice ever run on real V3D**
    (V-3b-3b stopped at instance): (1) `vk_icdGetInstanceProcAddr` returns mesa
    LOADER dispatch trampolines (`vk_tramp_CreateDevice`: `ldr x4,[x0,#0x1380];
    br x4`) built for the loader's object layout -- loader-less the
    physical-device slot is null, so it tail-branches to `pc=0`. Fix: call the
    Venus entrypoints directly by symbol (`extern vn_CreateDevice` /
    `vn_AllocateMemory` / the 1.4 `vn_MapMemory2`/`vn_UnmapMemory2` /
    `vn_FreeMemory` / `vn_DestroyDevice`), the loader-less pattern the prove
    already used for the ICD entry. (2) `vn_instance_acquire_ring_idx` reserves
    ring 0 for the CPU timeline and hands a queue index 1, rejected when
    `>= max_timeline_count`; the V-3b-3b F3 cap of 1 made EVERY vkCreateDevice
    with a queue impossible (a latent bug, not a safe default). Fix:
    `max_timeline_count = 2` -- exactly one queue timeline; self-limiting (a 2nd
    queue's acquire returns -1 -> clean VK_ERROR_INITIALIZATION_FAILED, never a
    silent mis-fence). The F3 seam-carries-ring_idx fix stays OWED and gates a
    second queue timeline (multi-queue SUBMIT), orthogonal to this allocate+map
    path (creates the queue, never submits). Witnessed on thyla-pi/KVM real V3D
    4.2.14 (probe-free clean build): `device-memory sentinel OK (zero-at-map +
    c0deface round-tripped)` then `THYLACINE-VENUS-PROVE PASS ... V-3b-3c-2b` --
    vkAllocateMemory -> create_from_device_memory -> warp_mem_new, vkMapMemory ->
    bo_map -> t_weft_map, the backing observed ZERO at map (the server's
    disclosure floor, a cross-boundary read through the weft mapping) before the
    sentinel. Gate-wired (#245): the client witness `device-memory sentinel OK`
    joined `venus-verdict` + `test-venus-verdict` (DISCRIMINATES 37/37) + the
    boot-probe filter. **Audit** (holotype-reviewer Fable 5 max, MODEL(end)==Fable
    5, I-45 client surface): **0 P0 / 2 P1 / 0 P2 / 1 P3, all fixed -- DIRTY,
    re-audit owed on the fixes.** F1 [P1]: the F4 mint-before-device-use ordering
    was NOT honored -- the driving Venus flow (`vn_device_memory.c`) defers
    renderer-bo creation to first `vkMapMemory` for a plain HOST_VISIBLE type, so
    the server's mint-time zeroing would destroy a GPU write landed between
    allocate and first map; both the backend + server comments asserted the
    opposite; the E2E is structurally blind (maps immediately, no GPU work). Fix:
    a new `vn_renderer_info.bo_must_init_at_alloc` bit -> `vn_device_memory`
    reifies the bo (and so the mint) at vkAllocateMemory for HOST_VISIBLE
    (alloc_export's eager tail); the ordering witness needs a GPU submit and lands
    with the GPU-submit chunk. F2 [P1]: every refused mint permanently burned a
    handle, and for mem the dominant refusal is the routine E_NOMEM at the 64 MiB
    holistic cap -> <=256 refusals wedged vkAllocateMemory for the instance's
    life. Fix: `warp_mem_new` is three-valued (1 = slot provably NOT installed ->
    free the handle; -1 = maybe installed -> keep marked); the backend frees on
    the not-installed arm. F3 [P3]: the timeline comment overclaimed single-queue
    submit-fence soundness (creation is sound; submit fencing is owed-F3) ->
    reworded. 18 items verified sound (token order, mem_bitmap, forward two-sided
    invariant, no anonymous-map masking, ...). Re-witnessed GREEN post-fix
    (reify-at-alloc does not regress the E2E). Mesa 6 files (the F1 fix touches
    `vn_device_memory.c` + `vn_renderer.h` -- otherwise the core Venus driver is
    untouched); impl mesa `2c742991` (patch 0013, round-trip `41ee6252`).
    `memory/audit_v3b3c2b_closed_list.md`.
- **V-3b-3d -- the Vulkan prove-gate on thyla-pi (real V3D).** A headless
  compute/clear + fenced readback through the full stack; `virgl_prove.c` template
  + a new `warp-host.sh venus`/`vk` verb; renderer-identity discrimination (assert
  the Venus ICD, not lavapipe). The E2E endpoint (subsumes the old V-3d).
- **V-3c (capset authority, tapestryd-side)** is orthogonal (`WarpCtx.capset`
  goes live; reject a never-enumerated capset); sequence with/before 3b if the
  caps blob must validate for bring-up.

**Invariants.** I-45 (each ring/bo minted under the client's venus ctx, named only
through `ctx/<id>/`), I-9 (the idle-skip kick, upheld by virglrenderer + the guest
over the coherent shmem -- the backend is transparent to it), I-32
(`WARP_SUBMIT_MAX` + `WARP_CTX_BACKING_MAX` bound submits + backing), I-7/I-37 (the
ring/reply/bo lifetimes ride the built dual-refcount + weft paths). Audit-bearing
at each sub-chunk (the section-25.4 Warp row); a focused Fable round after 3b (the
first sub-chunk with a client-writable bring-up path) + 3c.

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
