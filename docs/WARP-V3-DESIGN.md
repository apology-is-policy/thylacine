# WARP-V3-DESIGN.md -- Venus over the coherent ring

Implementation design for **Warp-6 V-3** (`vn_renderer_thylacine` + the coherent
shmem ring). Elaborates `GPU-DESIGN.md` sections 2.3, 2.4, 6.2, and the section-12
V-3 ladder entry into an implementable design. Binding for the V-3 sub-chunks;
where it and `GPU-DESIGN.md` disagree, GPU-DESIGN wins and this doc is corrected.

Status: **DESIGN (V-3a detailed; V-3b/c/d sketched).** No code has landed against
it yet. Tip context: V-2 (`SYS_BURROW_FROM_HOSTMEM=107`) shipped at `7973f8dc`.

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
| **V-3a** | the coherent shmem ring primitive: a server-minted, weft-shared, coherently-mapped guest blob + the submit doorbell (idle-skip) + the fence feedback-slot signal + **F2** (validate client pa/len). The `/srv/warp` ABI addition. | tapestryd (Rust, in-tree, local) + a `warp-prove` synthetic-ring round-trip. **No Venus driver needed.** | **No new syscall** (rides weft + the V-1 guest-blob path) |
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

### 3.7 Tests (V-3a, local, no Venus driver)

A `warp-prove` synthetic-ring scenario -- the ring mechanism validated without
Mesa:

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
