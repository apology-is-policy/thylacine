# WARP-WSI-DESIGN — the Vulkan present path (the vkQuake arc W-2; the Halcyon-on-vk substrate)

**Status: RATIFIED — operator signoff 2026-08-26 (the direction §0 by explicit
vote; the full design by signoff on this doc). Binding scripture; W-3
implements against it. Amendments follow the scripture-first pattern.**

Chunk lineage: the vkQuake arc (operator-voted 2026-08-26; W-1 = the offscreen
pipeline witness, CLOSED at `4efe9bc4`/mesa `150990a`). W-2 is this design.
W-3 = the implementation. W-4 = vkQuake as the E2E proof.

---

## 0. The ratified direction (operator, 2026-08-26)

Two operator statements bind this design:

1. **The standard**: "we want to build our system to the highest extent and
   standard possible, and will accept no workarounds or less proper
   implementations of our subsystems — we don't need to move fast, we need to
   build quality for the future and not look at cost. Even if you think some of
   our architecture that we've already built on was not right, there is no
   problem at all going back and redoing it better. **Halcyon might even run on
   vk at one point, or from the beginning.**"
2. **The vote**: of the three candidate architectures (§0.2), **Option B built
   fully** — the zero-copy render↔present capability unification — ratified
   with **Halcyon-on-Vulkan as a first-class design requirement**, not a
   door-left-open.

### 0.1 What this design must produce

A Venus-rendered `VkImage` becomes a **first-class presentable resource**:
scanned out directly (fullscreen, zero-copy) or fenced-composed (windowed) by
tapestryd, with a proper acquire/release fence on the I-40 bracket — the
general, continuous compositor substrate Halcyon-on-vk would run on, proven
first by vkQuake.

### 0.2 The alternatives, and why they were rejected (the record)

- **A — CPU/prime-blit into a tapestry weave** (mesa wsi_common's CPU-image
  path; render → `vkCmdCopyImageToBuffer` → host-visible buffer → copy into a
  weave → the existing `/srv/tapestry` present). Every mechanism proven (W-1
  step 8 + the thyla_tap path); no tapestryd ABI change; fastest to vkQuake.
  **REJECTED as a workaround**: the per-frame CPU copy is negligible at
  vkQuake's 320×240 and a *permanent tax* at Halcyon's full-resolution
  continuous compositing — the property that made it cheap is exactly the
  property that does not hold for the real target.
- **C — dma-buf export + mesa's `vn_wsi.c` native path** (the most
  upstream-faithful shape). **REJECTED for paradigm, not effort**: dma-buf +
  DRM/KMS is Linux's ambient-authority answer — exported handles into a global
  compositor protocol — and does not map onto Thylacine's per-Proc,
  capability-scoped model. `thylacine_bo_export_dma_buf` returning -1 is not
  an omission to fix; it is the correct absence of a concept we do not have.
- **B — the zero-copy unification** (this design): the Fuchsia
  (sysmem + Flatland) / Genode (Gpu + Gui session) SOTA shape — **render
  authority and present authority are separate capabilities; the handoff is a
  shared buffer both sides already map, scanned out directly**. Thylacine's
  Warp hostmem/host3d substrate already shares the host pages between the
  guest (the venus render target) and the host (virglrenderer); what is
  missing is only the present half's recognition of those resources.
  **RATIFIED.**

What B *reuses* and what it *replaces* — because "built fully" must not mean
"rebuilt needlessly": mesa's `wsi_common` swapchain/acquire/present **state
machine** is kept (well-tested upstream infrastructure; reimplementing it
would be the anti-quality move). What is replaced is only the **image path**:
a Thylacine `wsi_interface` whose swapchain image is a native shared Venus
resource registered for scanout — no dma-buf, no CPU blit.

---

## 1. Prior art (the homework behind the fork)

### 1.1 The heritage: Plan 9

Plan 9 has no swapchain and no GPU present protocol. Its model: the display is
a **file server** (`/dev/draw`); a window is a file-shaped image resource; the
window system (rio) **owns the screen** and composites client images — a
client hands rio a finished image and rio decides when it reaches glass.
The inheritances this design keeps:
- **tapestryd is rio**: the compositor owns the scanout (I-27's trusted-sink
  posture); a client never programs the display.
- **Present is "hand the compositor your image"**, named through the
  namespace — not a client-programmed flip.
- The swapchain's N-image round-robin is *mesa's* abstraction; the Thylacine
  side owes only "accept a finished image, composite it under I-40, signal
  when the buffer is reusable."

### 1.2 The capability-microkernel SOTA

Not Linux (dma-buf/DRM/Wayland is the ambient-authority shape rejected in
§0.2-C). The relevant peers converged on one answer:

- **Fuchsia (Magma + Flatland/Scenic)**: the GPU channel (render) and the
  Flatland channel (present) are **separate capabilities**; the handoff is a
  **sysmem buffer collection** — a shared buffer whose format/tiling both
  producer and consumer negotiated up front, so the compositor scans out the
  exact pages the GPU wrote. Zero-copy by construction.
- **Genode (Gpu session + Gui/Framebuffer session)**: the same split, sharper —
  two sessions from two servers, a shared dataspace as the handoff, present as
  "flip/blit this dataspace."

The convergent principle: **render authority and present authority are
distinct capabilities; the handoff is a shared buffer with a release fence.**
Mapped onto Thylacine: the `/srv/warp` venus ctx is the render capability
(built, W-1); the present capability is tapestryd's present path; the shared
buffer is the hostmem/host3d substrate (built, V-2/V-3b); the release fence is
the I-40 bracket surfaced as a poll-able completion.

### 1.3 The synthesis (the NOVEL candidate)

The fusion of §1.1 and §1.2 — **a capability-scoped, namespace-named,
zero-copy swapchain with no dma-buf and no global compositor protocol**: the
swapchain image is a shared Venus resource, presented by naming it to the
compositor over the same 9P service that scoped its creation, composited under
a formally-specified no-torn-scanout invariant. If the direct scanout of a
venus resource is genuinely new on this class of stack, this is a NOVEL.md
candidate — to be recorded at signoff.

---

## 2. Design requirements (binding once signed off)

R1. **Zero-copy on the steady-state path.** A fullscreen presentable image's
    pages are scanned out directly; no per-frame CPU copy anywhere in the
    present path. Windowed composition may touch pages only host-side
    (the fenced compose), never a guest CPU blit.
R2. **Halcyon-scale**: the path is sized and specified for continuous
    full-resolution compositing (the Halcyon-on-vk substrate), not for the
    vkQuake demo. vkQuake (W-4) is the *proof*, not the *target*.
R3. **Capability separation**: render authority (the venus ctx) and present
    authority are distinct; holding one never implies the other; both are
    namespace-named and I-45-scoped (a client presents only surfaces it owns,
    only resources its ctx owns).
R4. **I-40 holds, formally**: the no-torn-scanout invariant extends to
    venus-resource presents, and `specs/tapestry_present.tla` is extended to
    model the new present class BEFORE the implementation lands (the spec-first
    re-enablement point (a) — an invariant-bearing feature that genuinely
    benefits from machine-checked exploration; the I-40 spec already exists
    and gates additively, the C-1/C-6 precedent).
R5. **Proper fencing**: `vkAcquireNextImageKHR` blocks on a real
    release-completion (the compositor is done with that image), never a
    sleep/poll workaround; `vkQueuePresentKHR` orders render-complete before
    compose/scanout via a real fence (the multi-queue timelines), never a
    CPU-synchronization stand-in.
R6. **Reuse upstream where it is the quality path**: mesa `wsi_common`'s
    swapchain state machine is the implementation substrate for the Vulkan
    semantics; only the image/present backend is Thylacine-native.
R7. **The two-tree debt is resolved, not worked around** (§3): a presentable
    GPU resource is first-class in the present ABI — not bolted onto the
    virgl-only `present-to` bridge by another special case.

---

## 3. The architectural debt this design resolves

tapestryd serves two disjoint worlds today:

- `/srv/tapestry` — surfaces/weaves: 2D, weave-backed
  (`RESOURCE_CREATE_2D` + `TRANSFER_TO_HOST_2D`), the proven present path
  (thyla_tap / libtapestry), I-40-enforced.
- `/srv/warp` — GPU contexts and resources: virgl BOs (`bos[]`, `create3d`,
  `dma_fd`-carrying) and venus device memory (`mems[]`, HOST3D-blob-backed,
  no `dma_fd`).

The only bridge is Warp-4's mutual adoption (`glsrc` / `present-to`), and it
is **narrow by construction**: `present-to` resolves its argument against
`bos[]` only, keyed on `dma_fd >= 0` — a virgl-shaped test that no venus
resource can pass. Extending that test case-by-case is exactly the
"less-proper implementation" the ratified standard forbids. This design makes
presentability a **property of a shared GPU resource**, with one validation
model covering both resource families — the redo the operator explicitly
invited.

### 3.1 The measured gaps (mechanism research, 2026-08-26; file:line-cited)

What a venus image would need to be presented that does not exist today —
each a design obligation below, none a reason to fall back to a workaround:

1. **A `WarpMem`'s `res_id` is not scanout-shaped.** It is minted by
   `RESOURCE_CREATE_BLOB(HOST3D)` (`gpu.rs:2377-2410`), whose wire format
   carries no format/width/height/target at all; `SET_SCANOUT`
   (`gpu.rs:2791-2802`) "fetches the host GL texture id" (GPU-DESIGN §4.4) —
   a blob has none. `SET_SCANOUT(mem.res_id, …)` has nothing to fetch.
2. **`SET_SCANOUT_BLOB` — the virtio-gpu command that scans out a blob
   directly — is absent from our wire vocabulary** (not defined, not
   negotiated anywhere in `gpu.rs`). A structural gap, not a validation gap.
3. **`present-to` and `gl_adoption` are typed to `bos[]`** (`server.rs:
   10627-10635`, `5479-5483`); `WarpCtx.present_to` carries a `bo_pub`; no
   path resolves a `mems[]` entry.
4. **`WarpMem` carries no geometry** (`server.rs:1739-1748` — by design), so
   none of the eligibility gates (`w/h` match, `w*h*4 <= size`) have a field
   to check.
5. **Venus resources live under a different device ctx** (`venus_ctx =
   WARP_VENUS_CTX_BASE + slot`) than everything the scanout/compose machinery
   has ever bound (virgl `dev_ctx` / `COMPOSITOR_CTX`).
6. **The I-40 spec models exactly three in-flight host-work classes**
   (transfer / blit / readback), each with a `NoTorn*` + `Drained*` pair
   (`specs/tapestry_present.tla:361-391, 453-461, 718-726, 760-766,
   863-905`); a venus-image scanout is a fourth class with no state variable,
   no invariant, and no drain conjunct yet.
7. **`WarpMem` teardown has no display-safety ordering**: `gl_evict_res`'s
   "unbind the scanout BEFORE the unref — the one order the display cannot
   survive" (`server.rs:5539-5562`) has no analog on the
   `wmem_destroy`/`retire_host3d_ring` path, because no mem has ever been a
   scanout target.

---

## 4. The present-path mechanism

### 4.1 The presentable-image model (the sysmem lesson, made Thylacine-shaped)

The Fuchsia lesson (§1.2) is that zero-copy present works when producer and
consumer agree on the buffer's shape **up front** — not when a raw allocation
is retrofitted into a display. The design therefore does NOT bolt geometry
onto `WarpMem` (a `VkDeviceMemory` is correctly geometry-less — gap 4 is a
feature of the memory model, not a bug). Instead:

**A swapchain image is a first-class `presentable` object**: a venus-created
`VkImage` whose backing the server minted as a **HOST3D blob it never maps**
(bound by `blob_id` to the venus allocation, exactly the existing
`mint_host3d_ring` binding — `gpu.rs:2481-2536`; this said *shareable* until
§4.1 was amended by measurement — see §4.1 — and the correction matters here
too: what withholds the image from the guest is the absence of any map, not a
flag) and whose **display shape
(width, height, format, stride) is declared at registration**, validated
against what mesa's WSI negotiated. Three consequences:

- The declaration is the negotiation: the compositor accepts the registration
  only for shapes it can scan out / compose (BGRA8/XRGB8 at stage 0 — the
  formats the console path composes today), so eligibility is decided ONCE at
  create, not re-derived per frame. (The sysmem "buffer collection
  constraint" idea, without a new protocol: the constraint set is the accept
  set of one registration verb.)
- A **DEVICE_LOCAL** swapchain image never needs a guest mapping: it exists
  to be *named* (scanout, compose), not mapped. The mappable path
  (HOST_VISIBLE) stays what it is today. This splits gap 1 correctly:
  scanout needs a nameable host resource with a declared shape, not a
  guest-visible one.

  **AMENDED at W-3c-1 (2026-08-26, operator-ratified) — the mechanism, not
  the property.** This bullet originally specified the mint as a *shareable*
  blob created **without** `USE_MAPPABLE`, on the reasoning that omitting the
  flag is what keeps the image out of the guest. The W-3c-1 self-test
  measured that mint on the real chain (thyla-pi, QEMU 10.0.11 +
  virglrenderer 1.1.0 + v3dv) and the host refused it. Isolated cleanly —
  same size, same venus ctx, `blob_id` 0, only the flag varying:

  | `blob_flags` | verdict |
  |---|---|
  | `USE_SHAREABLE` alone (as specified here) | **refused** (`RESP_ERR_UNSPEC`) |
  | `USE_MAPPABLE \| USE_SHAREABLE` | **refused** |
  | `USE_MAPPABLE` alone | **accepted** |

  So `USE_SHAREABLE` is refused outright on a HOST3D blob by this
  virglrenderer — it is not that `USE_MAPPABLE` is *additionally* required.

  The correction is that the original text **conflated a FLAG with an
  ACTION**. `USE_MAPPABLE` declares that the host *may* place the blob in the
  hostmem BAR; it is `RESOURCE_MAP_BLOB` + `SYS_BURROW_FROM_HOSTMEM` that
  actually expose bytes to the guest. The property this design wants — a
  swapchain image the guest cannot touch — is therefore secured by **never
  mapping it**, not by omitting the flag. So the as-built mint is
  `create_host3d_blob(USE_MAPPABLE)` and the presentable path calls neither
  `map_blob` nor `burrow_from_hostmem`: no hostmem offset, no weft share, no
  guest VA, no reclaim park, no #847 dual count. Every consequence the
  original bullet claimed still holds; only the mechanism that delivers them
  changed.

  **What this measurement does NOT establish** (the W-3a class-scoping rule):
  it was taken with `blob_id = 0`, the self-test's stand-in for a venus
  allocation, and virglrenderer's blob-id-0 path is its own plain-memory arm.
  Whether `USE_SHAREABLE` is accepted for a **real** venus allocation
  (`blob_id != 0`) is unmeasured and unmeasurable without a client; it lands
  with W-3d. If it turns out to be accepted there, revisiting this bullet is
  optional — the mapped-never property is delivered either way — so the
  amendment is not contingent on that answer.
- The presentable's lifetime is the swapchain's: `wsi_common` creates the
  `VkImage`s at `vkCreateSwapchainKHR`; the backend registers each once;
  `vkDestroySwapchainKHR` retires them. No per-frame create/destroy anywhere.

### 4.2 The Direct arm (fullscreen, zero-copy)

The steady-state Halcyon path. The surface↔ctx adoption generalizes (§5.1);
when the adopted source is a presentable and the surface is fullscreen,
tapestryd binds the display to the presentable's resource:

- **The wire mechanism is `SET_SCANOUT_BLOB`** (gap 2): the virtio-gpu
  command whose argument set (res_id + format + width/height/stride +
  offsets) exists precisely because a blob resource carries no implicit
  shape. tapestryd's `gpu.rs` grows the command + its feature check; the
  registration's declared shape supplies the arguments. Per-frame, a present
  is then **`RESOURCE_FLUSH` only** — the same zero-guest-transfer steady
  state the Warp-4 GL Direct arm already proved (`server.rs:11207-11289`).
- **Host capability is verified, never assumed** (the V-0 discipline): W-3's
  FIRST sub-chunk is a host-capability probe — does this QEMU + virglrenderer
  + v3dv chain accept `SET_SCANOUT_BLOB` on a venus-bound HOST3D blob, with a
  paired negative control? The probe's verdict gates the arm. If the real
  host chain refuses (the export bridge between the venus render-server and
  the display is a host-internal question we cannot legislate), **the
  fallback is the Composed arm (§4.3) — host-side GPU work, still zero guest
  copies** — and the Direct arm lands when the host chain supports it. The
  fallback is explicitly NOT a guest CPU blit; R1 holds on both arms.
- Ordering: render-complete precedes scanout CLIENT-side at stage 0 — the
  backend waits the frame's fence (the multi-queue per-timeline ledgers)
  before issuing the present RPC (§4.4), so the compositor never needs to
  observe a client fence. A server-side observe ("present names a timeline
  point") is the async evolution's shape, recorded with the §4.4 seam.

### 4.3 The Composed arm (windowed; host-side only)

The windowed case (and the Direct fallback) reuses the Warp-C machinery,
extended across the ctx boundary (gap 5):

- Preferred: the **C-3 GPU blit** — import the presentable's resource into
  the compositor's context and blit into the screen resource
  (`submit_blits`, `server.rs:11417-11437`). Whether a venus-ctx-created
  resource is blittable by the virgl compositor ctx is the same
  host-capability question as §4.2 and rides the same probe.
- Fallback: the **C-6 fenced readback** (`rb_issue`/`comp_readback_retired`,
  `server.rs:8075-8212`) — a fenced host DMA read of the presentable into
  the weave, completing off the fence pump. Host-side work only; the guest
  never copies. The C-6 bookkeeping (the reserved fenced slot, the
  double-counted in-flight, abandon-poisons-the-ctx) carries over unchanged.

### 4.4 Acquire/release fencing (I-9 + R5)

Stage 0 keeps the **synchronous present bracket** that is load-bearing for
I-40 today (`server.rs:9-79`: the in-flight window opens and closes inside
one dispatch): `vkQueuePresentKHR` → the backend's present RPC blocks until
tapestryd has composited/bound the image → the reply IS the release of the
*previous* image on that surface. Consequences:

- `vkAcquireNextImageKHR` = take the next free image from the swapchain's
  free list; block (on the present RPC's completion, via the wsi_common
  serialization) only when all images are in flight — with N>=2 images the
  render of frame K+1 overlaps the compose of frame K, which is the
  double-buffering the swapchain contract wants. No polling, no sleeps.
- Render-before-present ordering: the backend submits the present only after
  the rendering queue's fence timeline reaches the frame's point (the
  multi-queue ledgers; the wsi_common present-wait machinery drives this).
- The async evolution (a fenced, non-blocking present with an explicit
  release event) is a recorded seam, NOT stage 0: it follows the C-6
  precedent — a new in-flight class goes async only WITH its own fence tag,
  drain conjunct, and spec extension (GPU-DESIGN §4.5.6: "I-40 does not
  mandate synchrony; it mandates quiesce-before-retire").

## 5. The ABI (the two-tree reconciliation, R7)

### 5.1 One adoption model, two resource families

The mutual-adoption shape (surface names ctx, ctx names resource,
incarnation-pinned, re-resolved per use — `server.rs:11052-11086`,
`10586-10666`, `5448-5522`) is CORRECT and is kept. What changes: the ctx
half's argument becomes a **presentable id**, and the resolver
(`gl_adoption`'s successor) resolves it against ONE eligibility model:

    presentable = a WarpBo (virgl; shape = its create3d w/h/format)
                | a registered venus image (shape = its registration)

with one shared gate: owned by the naming ctx (I-45), alive, not retiring,
shape matches the surface, backing sufficient, fence-clean. The `bos[]`-only
`present-to` (gap 3) is superseded — the virgl arm becomes one family of the
general model rather than the special case the venus arm bolts onto. The
`dma_fd >= 0` test dissolves into the per-family "built" predicate.

### 5.2 The registration verbs (sketch; exact wire format at W-3)

Under the client's own ctx (all I-45-scoped, all namespace-named):

- `ctx/<id>/img/new` — register a presentable. **AS BUILT at W-3c-1**
  (this bullet was labelled a sketch; W-3 has now landed the format):
  write `"<handle> <w> <h> <format> <stride> <mem_id>"`. The handle space is
  the CLIENT's (0..15, the `ring`/`mem` discipline), which is what removes
  the need to return an id — the sketch's "returns the presentable id" is
  superseded. `format` is the stage-0 accept set (BGRA8/XRGB8); `stride`
  must cover `w*4`; the mint is `create_host3d_blob(USE_MAPPABLE)` and the
  path never maps it (§4.1 as amended). A duplicate live handle is
  `E_INVAL`; over the I-32 byte cap is `E_NOMEM`.
- `ctx/<id>/img/<n>/info` — the accepted shape (the negotiation's record).
- `ctx/<id>/img/<n>/ctl` — `destroy` (display-safe: the §6 unbind-first
  ordering).

  > **THE CLIENT-ORDERING CONTRACT — binding on the WSI backend (W-3d).**
  > **Destroy the registration BEFORE freeing the venus allocation it names.**
  > In wsi terms: `vkDestroySwapchainKHR` retires the images, and each one's
  > `img/<n>/ctl destroy` must precede the `vkFreeMemory` of its backing.
  >
  > This discharges the fourth holder in `tapestry_present.tla`'s `PFree`
  > (`~venusRef`). Three holders are the server's and are enforced there — the
  > registration slot, and the two observer arms (`PUnbound`/`PDrained` in
  > `wimg_teardown`). The fourth is the client's own `VkDeviceMemory`, live in
  > its address space and **invisible to the server**, so it can only be a
  > contract, and it is stated here because the party bound by it reads this
  > document and not `server.rs` (round-2 F9).
  >
  > Getting it backwards — free-then-destroy — leaves the host holding a
  > binding to freed memory. Nothing guest-side can detect it: `mem_id` is
  > opaque to the server, and by the time the destroy arrives the damage is
  > host-side. It is bounded by the same trusted-host posture as every other
  > `blob_id` claim (GPU-DESIGN §9.2) and becomes ours to enforce at the v3d
  > fork. What the server guarantees regardless of client ordering is its own
  > half: the display never keeps a reference across the unref.
- `ctx/<id>/ctl` — `present-to <surface> img <n>` (the generalized arm).
- The surface half (`surface/<id>/ctl glsrc <ctx>`) is unchanged — consent
  stays mutual and incarnation-pinned.

### 5.3 The mesa backend

A Thylacine `wsi_interface` (the 7-entry vtable: 6 queries +
`create_swapchain`) registered into `wsi_device.wsi[]`; `wsi_common` keeps
the swapchain state machine (R6). The image path: `create_swapchain` creates
the VkImages via the driver as usual, then registers each as a presentable
(§5.2) instead of exporting a dma-buf; `queue_present` = the present RPC;
`acquire` = the free-list + serialization of §4.4. `vn_wsi.c`'s
dma-buf-assuming native path stays off; the Thylacine backend is the
platform. Surface enumeration: one surface class ("the tapestry surface"),
created from the SDL glue below.

### 5.4 The SDL2 Vulkan glue (W-3's second half)

The inert 5-hook `Vulkan_*` vtable (`SDL_sysvideo.h:296-300`) gets a
Thylacine arm: `Vulkan_CreateSurface` wraps the tapestry surface the video
backend already owns (`thyla_tap` / `SDL_thylacinevideo.c`) into the
`VkSurfaceKHR` the mesa backend defined; `GetInstanceExtensions` reports the
Thylacine surface extension; the khronos Vulkan headers the vendored SDL
expects are restored for the build. vkQuake then runs unmodified through
`SDL_Vulkan_*` — the W-4 proof.

## 6. Invariants + the spec extension (R4)

- **I-40**: the venus-image scanout/compose is the FOURTH in-flight class.
  `specs/tapestry_present.tla` grows — additively, behind its own `ALLOW_*`
  switch with measured no-drift on the existing cfgs (the C-1/C-6
  precedent) — a presentable-bound state, its `NoTorn*` invariant (the
  display never observes a retired presentable), its `Drained*` conjunct on
  `ServerRelease`/`Free`, and a `BUGGY_*` counterexample cfg. The spec lands
  TLC-green BEFORE the W-3 implementation (spec-first re-enablement point
  (a)).
- **The display-safe teardown** (gap 7): a presentable's destroy/ctx-death
  path gains the `gl_evict_res` ordering — unbind (`SET_SCANOUT(0)` /
  evict from the compose set) BEFORE the resource unref — plus the fence
  drain (no in-flight compose names it). This is the `WarpMem`-side analog
  that never needed to exist before.
- **I-45**: the registration + present verbs resolve only under the owning
  conn's ctx (the existing owner-scan shape); a ctx presents only its own
  presentables to only surfaces that named it back.
- **I-7/I-37**: the presentable's blob lives until the last of {the venus
  allocation, the registration, an in-flight compose/scanout} releases —
  the dual-count discipline extended by one holder class.
- **I-9**: the acquire path's block rides the present RPC (a parked 9P
  reply), which is already lost-wakeup-proof; no new wait/wake primitive is
  introduced at stage 0.

## 7. The W-3 sub-chunk plan

1. **W-3a — the host-capability probe** (the V-0 discipline): does the real
   QEMU 10 + virglrenderer 1.1.0 + v3dv chain accept `SET_SCANOUT_BLOB` on a
   venus-bound HOST3D blob, and can the compositor ctx blit a venus-ctx
   resource? Paired positive/negative controls; the verdict selects §4.2
   Direct vs §4.3-only, recorded in this doc.
   **MEASURED (thyla-pi/KVM, 2026-08-26):** `dispatch=present neg=0x1203
   pos=0x1100 attach=0x1100 attach-neg=0x1100`. Meaning: (a) the
   `SET_SCANOUT_BLOB` vocabulary EXISTS and discriminates — the bogus id drew
   exactly `INVALID_RESOURCE_ID`, and the shmem-class positive was ACCEPTED
   (`OK_NODATA`; acceptance, not pixels — the pixel truth stays with W-3e's
   witness, and the venus-IMAGE class stays open per the probe's class
   scoping). The §4.2 Direct arm proceeds. (b) The cross-ctx ATTACH leg is
   **BLIND** — the bogus id was ALSO accepted, so attach-acceptance proves
   nothing at this layer; the compose-arm capability question moves to the
   blit-USE, observable at W-3c/W-3e. The paired negative catching its own
   instrument is the probe working as designed. Control leg (no blob)
   witnessed the positive skip on the local 2D suite the same day.
   **Audit + re-measure (same day):** the round-1 holotype found the first
   attach measurement itself unconstructed — `COMPOSITOR_CTX` did not exist
   at probe time (its creator ran after READY), so the original values
   measured indifference to a nonexistent context. Fixed
   (`ensure_comp_ctx` before the attach legs) and RE-MEASURED: byte-identical
   values against the now-existing ctx — so the blindness claim is valid as
   stated: even with the target ctx present, a bogus RESOURCE id draws OK,
   i.e. the host defers *resource* resolution past attach. On a future
   validating host (the v3d fork re-run) the fixed probe measures attach
   semantics rather than manufacturing `INVALID_CONTEXT_ID` from its own
   ordering.
2. **W-3b — the spec extension** (the fourth class; TLC-green + the buggy
   cfg + measured additivity) — BEFORE the server code.
   **LANDED (2026-08-26):** `tapestry_present.tla` grows the presentable
   behind its own `ALLOW_PRESENTABLE` switch — 6 variables (a 4-state
   `pstate` lifecycle, the host-resource liveness `pbacked`, the I-7/I-37
   holders `venusRef`+`regRef`, the two observer arms `pbound` standing +
   `pinflight` transient), 9 actions, and the §6 obligations exactly:
   `NoTornPresentable` (the display never observes a retired presentable —
   both observer arms), the `PUnbound`+`PDrained` conjuncts on
   `PServerRelease`/`PFree` (the display-safe teardown; each omission its
   own `BUGGY_*` cfg per the per-direction-sabotage discipline), `PGoneClean`
   + `PObserverScoped`, and `PresentableEventuallyRetired` (the ordered
   teardown terminates). Modeling decisions on the record IN the module: the
   compose arm is ONE read class (blit + readback source — the readback's
   write side stays the existing `inread` class per §4.3); no content leg at
   stage 0 (client-side fence + wsi acquire; re-opens with the async seam);
   one presentable models the N-image class; the I-45 adoption gate is
   verb-resolution with no lifetime edge (prosecuted at W-3c);
   `ServerDeath` is atomic totality for this class (device-side backing AND
   observers die in one reset — unlike the weave arms, whose guest pages
   have an observer that outlives the reap window). **Additivity measured**:
   all four pre-existing clean cfgs reproduce exactly (5413/5413/94680/94680
   — the composed pair now PINNED in `check-tapestry.sh`, which the C-6
   close had left unpinned); the all-features presentable cfg explores
   1557073 distinct states green.
3. **W-3c — tapestryd**: the presentable object (`img/` subtree), the
   generalized adoption/eligibility model, the wire command(s), the
   display-safe teardown. Audit-bearing (I-40/I-45; a new AUDIT-TRIGGERS
   row). **Split into two sub-chunks so each lands with its own witness:**
   - **W-3c-1 — the OBJECT and its lifetime.** The `img/` ABI
     (`new` / `info` / `ctl destroy`), the `WarpImg` slot row folded into
     the I-32 holistic cap, the **`USE_MAPPABLE`, never-mapped** HOST3D mint
     (`create_presentable`; §4.1 as AMENDED -- this line said *shareable
     non-mappable* until measurement refuted it), the Direct bind (`set_scanout_blob`, the
     verdict wrapper over W-3a's raw-resp probe — one wire implementation),
     and the **display-safe teardown**: unbind before unref, reusing
     `gl_evict_res` rather than re-implementing an ordering rule, and
     issued *unconditionally* so a stale per-object flag cannot skip it.
     Witness: `warp_img_selftest`, four arms — `shape=` (three refusals one
     variable away, so the accept set is shown to be a gate rather than a
     rubber stamp), `mint=`, `bind=`, and `unbind=`, which destroys the
     presentable **while the display is bound to it** and observes the
     binding dropped first. That last arm is the runtime twin of
     `tapestry_present_buggy_punbind_skipped.cfg`: it witnesses the modeled
     bug's absence, not a generic teardown success.
   - **W-3c-2 — the CLIENT-FACING present path.** The generalized
     adoption/eligibility model (`presentable = WarpBo | registered venus
     image`, superseding the `bos[]`-only `present-to`) and `present-to
     <surface> img <n>`.
     **LANDED (2026-08-31), with the Composed arm RESEQUENCED to W-3d
     slice 1** per the run-6 fork resolution (`docs/JOURNAL.md` 2026-08-31):
     the W-3c-2a probe's `noreadback` was the blob_id=0 STAND-IN being
     categorically untypeable (SHM `fd_type`; virglrenderer's
     `pipe_resource_set_type` takes DMABUF only), not a host refusal — the
     REAL class (a `blob_id`-named VkDeviceMemory, which vkr force-exports
     as a dmabuf on HOST_VISIBLE allocations) has the designed
     attach→SET_TYPE→EGLImage→C-3-blit path, host conditions verified on
     the Pi. So the compose arm is built where the first real-class blob
     exists (W-3d), gated by re-running the compose probe against it — and
     the `PDrained` drain lands in that same commit (the W-3c-2 Direct
     adoption reads `imgs` but creates NO `pinflight` member: the bind is a
     standing binding tracked by `Comp.bound_res`, completed inside one
     dispatch, so the conjunct stays vacuously discharged exactly as at
     W-3c-1).
     As built: `PresentSrc {Bo, Img}` (both families PUB-keyed — an img
     handle resolves to its pub id at the verb, so a freed handle's later
     tenant can never inherit a consent); `gl_adoption`'s img arm carries
     the display-MODE half of the accept set (geometry vs the CURRENT
     surface incarnation — round-2 F13 discharged; no `dma_fd`/`va`/size
     analogues, that absence being I-7); `direct_bind_adopted` is the ONE
     copy of the family dispatch (`SET_SCANOUT` | `SET_SCANOUT_BLOB` at the
     declared shape — the spec's `PPresentBind`); every composed-machinery
     consumer is HARD-GATED to the Bo family (`rb_issue` DMA-writes into
     `g.va`, and an img adoption's `va` is 0); `wimg_destroy` gains the
     consent-clear arm; a composed-mode img consent says so once per ctx
     and shows the surface's own 2D weave until W-3d. Driver: `warp-prove
     img-direct` (mutual adoption → zoom → `scanout direct N img res R` →
     `bound` observed through `img/0/info` → destroy-WHILE-BOUND → the
     weave arm re-takes) + the img-xproc I-45 leg; gate: the four-witness
     conjunction in `warp-host.sh img`.
4. **W-3d — mesa**: the Thylacine `wsi_interface` + the swapchain image
   path + acquire/present. Audit-bearing (the Warp mesa row).
5. **W-3e — SDL2 Vulkan glue** + a prove extension (an offscreen→present
   witness: render the W-1 triangle INTO a swapchain image and present it —
   the first Vulkan frame on the Thylacine display).
6. **W-4 — vkQuake** (its own chunk; the E2E proof).

---

*Prior-art working notes: `scratchpad/w2-wsi-priorart.md`,
`scratchpad/w2-wsi-synthesis.md` (session-local). The tree-fact research and
the mechanism research are reproduced in the relevant sections above at
completion.*
