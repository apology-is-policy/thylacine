# GPU-DESIGN.md — the hardware-accelerated graphics seam

> **STATUS: BINDING. Signed off 2026-08-07.** All four forks in §10 are decided
> by the user; §§1–9 were resolved by the prior-art research and the in-tree
> fit-check. Implementation may begin at Warp-1 (`docs/CLAUDE.md` "Design
> conversation -> scripture commit": scripture first, code second, audit third).
>
> **The recorded votes**
>
> | Fork | Decision |
> |---|---|
> | **F1** dev-loop substrate | **Local Parallels Linux VM** — resolved by measurement, not vote (`GPU-HOST-SETUP.md`); GCP leg stays the fallback |
> | **F2** blob scope | **Skip blobs in Warp-2; GL 4.3 first light.** Costs only `ARB_buffer_storage`; blobs arrive with Venus at Warp-6. #166 fixed regardless |
> | **F3** v3d isolation | **Per-client GMP enforcement**, staged RESERVED→ENFORCED. The seam carries per-client identity to submit **from day one** |
> | **F4** NOVEL entry | **Yes** — post-v1.0 candidate |
> | **Naming** | **Warp** (user's choice, over the proposed "Jacquard"). Arc prefix `Warp-N`; service tree `/dev/warp` |

---

## 1. The charter, and what it forces

Task #157, user-directed 2026-08-06: **design the GPU seam so that RPi 500/400
bare metal is "a driver, not a rework."** virgl-in-QEMU is the first CI-testable
chunk. Amended the same day: **Vulkan-readiness is a seam requirement, not a
later arc.**

That amendment is the whole difficulty. The seam must carry four consumers whose
only common ancestor is "a GPU":

| Consumer | API | Where it runs | Transport |
|---|---|---|---|
| **virgl** | OpenGL | QEMU guest | virtio-gpu 3D, Gallium `VIRGL_CCMD_*` stream |
| **Venus** | Vulkan | QEMU guest | virtio-gpu 3D, `VK_MESA_venus_protocol` over a shmem ring |
| **v3d** | OpenGL | RPi 5 bare metal | MMIO registers + a GPU MMU + 6 hardware queues |
| **v3dv** | Vulkan | RPi 5 bare metal | same kernel-side driver as v3d |

A seam that fits only the first two is a virtio abstraction, and the RPi becomes
a rework. A seam that fits only the last two is a DRM clone, and the QEMU work is
throwaway. The design below is the intersection, chosen so that **nothing learned
in the QEMU arc has to be unlearned on hardware.**

Software rendering (llvmpipe/OSMesa, the CL-7 arc) is unaffected and permanent:
it stays the fallback tier wherever no GPU exists, and it is what every current
in-guest GL result was measured on (192.8 fps unpaced GLQuake, `#165` —
**macOS/HVF**; the figure does not transfer to a TCG host: thyla-gl's own
llvmpipe band is single-digit, the Warp-1 status row).

---

## 2. What the prior art settles

Four independent research passes (Fuchsia Magma + Genode; virtio-gpu/virgl +
QEMU-on-macOS; Venus; v3d/v3dv), read against primary sources — upstream Linux
`master`, Mesa `main` @26.2, virglrenderer `main`, QEMU `master`, the OASIS
virtio spec, fuchsia.dev RFCs, and the Genode release notes. The load-bearing
findings, each of which changed a design answer:

### 2.1 Both capability microkernels converged on the same isolation answer

Fuchsia RFC-0198, verbatim: *"For performance reasons, it isn't practical to
validate client commands and programs, so the hardware is expected to be
resilient."* … *"Client connections are isolated from one another through
independent hardware address spaces."*

Genode arrived at the identical posture independently: its Intel multiplexer
gives each session a hardware context with its own PPGTT page tables, partitions
the aperture and fence registers, and **parses no batch buffers at all**. Their
stated rationale for writing a from-scratch multiplexer was that modern
per-context GPU MMUs make command inspection unnecessary — Broadwell+ only, ~4–10
kLOC in the TCB against ~450 kLOC of client-side Mesa.

**Consequence for us:** the trusted GPU server does **not** parse client command
streams. Confidentiality and integrity ride the GPU's own address-translation
hardware; availability is best-effort (both systems document hangs as a real,
mitigated-not-eliminated risk). This is section 8's invariant.

### 2.2 The five-object vocabulary is stable across every target

Magma's `magma.h` and Genode's `Gpu::Session` (~13 RPCs) differ in spelling and
agree in structure, and both structures survive all four of our consumers:

> **device** → **connection** → **context** → **buffer** (create / import /
> export / map-into-GPU-VA) → **semaphore**, with **submit** returning a
> sequence number and **completion** arriving asynchronously.

Genode's interface carried three radically different backends — a native Intel
multiplexer, a ported etnaviv DRM driver, and a ported lima driver — without
changing shape. That is the direct precedent for one-seam-four-consumers.

### 2.3 Mesa is already cut where we need to cut it

Both guest drivers have a designed, upstream, OS-agnostic seam:

- **OpenGL**: `struct virgl_winsys` (27 slots; **18 are load-bearing** — the
  count established empirically from which slots the `vtest` backend leaves
  NULL). The Gallium driver above it is genuinely portable: a grep of
  `virgl_screen.c`, `virgl_context.c`, `virgl_resource.c`, `virgl_encode.c`,
  `virgl_buffer.c` for `__linux__` / `<sys/*>` / `DETECT_OS` finds **one** hit in
  the entire core — a single `#if DETECT_OS_ANDROID`. All platform dependency
  lives in the winsys.
- **Vulkan**: `struct vn_renderer` (shmem / bo / sync + `submit` / `wait` /
  `get_info`), with exactly two backends today — `vn_renderer_virtgpu` (Linux
  ioctls) and `vn_renderer_vtest` (a Unix socket, no kernel driver at all).

Both existing backends measure ~1.4 kLOC. **A Thylacine backend is a ~1–1.5 kLOC
file and zero patches to the Mesa driver above it.**

### 2.4 Venus needs far less kernel machinery than it appears to

Mesa's Venus driver **simulates syncobjs in userspace today** —
`vn_renderer_virtgpu.c` carries `#define SIMULATE_SYNCOBJ 1` with the comment
`/* XXX comment these out to really use kernel uapi */`. `VkFence` and
`VkSemaphore` are *feedback slots in shared memory* written by the host GPU and
polled by the guest driver; there is no host→guest interrupt for ring replies at
all. The Linux 6.5 virtio-gpu syncobj uAPI was driven by DRM native contexts, not
by Venus.

Likewise the API traffic barely touches the kernel: Venus calls travel over a
guest-visible shmem **ring** (a blob resource with head/tail/status cachelines);
the virtqueue is used only for bootstrap, doorbell kicks, and fences, and the
doorbell is skipped entirely unless the host's ring thread has gone idle.

**So the kernel contract for Vulkan is:** fence-bearing submit carrying a
`ring_idx` (u8, 0–63; Venus allocates one per `VkQueue`), in-order completion
*per timeline*, one waitable per submission. That is a Loom CQE. We need no
syncobj object model, no timeline semaphore primitive, no dma-fence graph.

### 2.5 v3d has **no** inter-client isolation — and the hardware to fix it

This overturned the optimistic assumption the design started from. Upstream
`drivers/gpu/drm/v3d/v3d_mmu.c`, verbatim and complete:

> *"Because the 4MB of contiguous memory for page tables is precious, and
> switching between them is expensive, **we load all BOs into the same 4GB
> address space.** To protect clients from each other, we should use the GMP to
> quickly mask out (at 128kb granularity) what pages are available to each
> client. **This is not yet implemented.**"*

The implementation matches exactly: one device-global `drm_mm` over 2^20 PTEs,
`V3D_MMU_PT_PA_BASE` written **twice in the driver's life** (resume and reset),
every BO's mapping pinned for its lifetime, `V3D_PTE_WRITEABLE` set
unconditionally on every page, and `CREATE_BO` returning the raw GPU virtual
address to userspace. The command-list address fields (`bcl_start`, `rcl_start`,
`qma`, `qts`, and every TFU/CSD register image) are copied through **unvalidated**
— vc4's 55 KB of CL and shader validation was deliberately dropped at the
VC4→V3D boundary, on the theory that the MMU replaced it.

It bounds the GPU against the *system* (a bogus address raises an MMU fault with
abort+interrupt, and the fault is reported with the offending AXI client), but
between GPU clients there is nothing: any process holding the render node can
read or write any other client's GPU memory by naming its address.

**But the hardware has a GMP** (Global Memory Protection unit, 2-bit permissions
per 128 KB region), and **Mesa's own simulator implements per-client GMP tables**
— `v3d_simulator.c` allocates a GMP table per fd and reloads
`V3D_GMP_TABLE_ADDR` + `V3D_GMP_CLEAR_LOAD` on every submit. That is a working
reference implementation of exactly the scheme the kernel comment describes,
costing one table reload per submission. Linux never adopted it. Nobody has.

### 2.6 The macOS host cannot run virgl, and the reason is structural

This is the design's one genuine blocker, and it is not a packaging gap. Traced
through QEMU `master`:

1. `virtio-gpu-gl` refuses to realize unless `display_opengl` is set.
2. `display_opengl` is assigned in exactly four places: SDL (under
   `CONFIG_OPENGL`), GTK GL-area, GTK EGL (gated `when: [x11, opengl]`), and
   `egl_init()`. **`ui/cocoa.m` contains zero occurrences of it.**
3. QEMU's `opengl` dependency *is* `epoxy/egl.h` — libepoxy built with EGL, which
   macOS (no native EGL; CGL and Metal) does not provide from stock.
4. `egl_init()`'s body is `#ifdef WIN32 … #elif defined(CONFIG_GBM) … #endif`,
   then `"egl: not available on this platform"`. Windows has a real ANGLE branch;
   `CONFIG_GBM` is Linux/DRM. **macOS has neither.**
5. The device is only compiled `if virgl.found() and opengl.found()`.

Homebrew's `qemu` 11.0.3 lists `libepoxy` and `mesa` as **Linux-only**
dependencies and does not depend on `virglrenderer` at all — so on the Mac,
`virtio-gpu-gl` is not merely disabled, it **is not in the binary**. Locally
verified: our `qemu-system-aarch64 10.0.2` offers only `virtio-gpu-pci` /
`virtio-gpu-device`, and its display backends are `none, curses, cocoa, dbus` —
no `gl`, no `sdl`, no `gtk`.

Third-party taps exist (`startergo/homebrew-virglrenderer`, building against
ANGLE + a Mesa-derived Vulkan layer) but could not be verified to work, on which
QEMU version, or whether they are maintained. Treat as "someone is trying."

Venus is worse: it inherits every gate above **and** its host stack is Linux-only
(virglrenderer render server + host Vulkan with `VK_KHR_external_memory_fd` +
QEMU ≥9.2, with a documented "Linux 6.13+ host" requirement).

### 2.7 CI must follow Mesa's own pattern, which is not QEMU

QEMU's `-display egl-headless` is a trap: `egl_init` → `egl_rendernode_init`
opens a **real `/dev/dri/renderD*`**, creates a GBM device, and hard-requires
`EGL_KHR_surfaceless_context` + `EGL_MESA_image_dma_buf_export` +
`EGL_EXT_image_dma_buf_import_modifiers`. A container without `/dev/dri` fails
immediately. `-display none` never sets `display_opengl`, so it fails gate 1.

Mesa's own virgl CI (`src/gallium/drivers/virgl/ci/gitlab-ci.yml`) runs **two
lanes on GPU-less runners**, and neither is QEMU:

- `virpipe-on-gl` — the **vtest** lane: host Mesa with `GALLIUM_DRIVER=virpipe`
  talking to `virgl_test_server` over a Unix socket. No VM at all.
- `virgl-on-gl` / `virgl-on-gles` — **crosvm** with
  `backend=virglrenderer,egl=true,surfaceless=true`, host GL being llvmpipe
  (given away by `LP_NUM_THREADS: 1` and its comment about concurrent crosvm
  processes).

---

## 3. The in-tree fit — verified, not assumed

Every claim below was checked against the tree this session.

**What already exists and fits:**

- **tapestryd owns the GPU.** It absorbed the G-1 `gpud` driver at G-3 and holds
  `virtio-pci:16` through the warden's `gather` manifest — one exclusive claimant
  per function, structurally. `docs/reference/138-gpud.md` records why residency
  is mandatory: a virtio-gpu reset at driver death destroys host-side resources
  and disables the scanout, so a non-resident GPU owner loses the display.
- **The present path is already the right shape.** `LOOM_OP_WRITE` of a 32-byte
  `tpresent` descriptor on `surface/<id>/present`; the CQE is the recycle gate
  (D1); slots give triple buffering; back-pressure is by slot reuse, never
  cancellation. Mapping a GPU fence onto that CQE is a re-use, not a new
  mechanism.
- **Direct vs Composed scanout** (G-6a) already distinguishes "scan out the
  client's own resource" from "compose into a compositor-owned buffer." That
  distinction is exactly the 3D fullscreen-vs-windowed split.
- **The kernel-minted-subtype precedent is built and audited.**
  `SYS_DMA_CREATE_WEAVE` (99) sets a create-immutable `KObj_DMA.weave` bit; the
  share path admits only that subtype; `SYS_WEFT_UNSHARE` (100) disarms;
  `Proc.shared_map_pages` is the I-32 fifth axis; a kernel reaper reclaims a dead
  compositor's orphaned client mappings. **I-40 is enforced on both halves.** A
  GPU buffer object is the same shape one step further.
- **The PCI layer is capability-driven, not BAR-index-driven.** `PciDev::claim`
  resolves the four virtio capability regions by `cfg_type` through
  `SYS_PCI_INFO`, so QEMU's hostmem-induced BAR renumbering (MSI-X moves to BAR
  1, modern-mem to BAR 2) does **not** break us the way it would break a driver
  with hardcoded indices.

**What does not fit, found by fit-check:**

- **`PciDev::claim` fails outright on a hostmem-enabled GPU.** **[FIXED at
  Warp-2a (#166)]** `hardware.rs` returned `PciError::BarTooLarge` for *any*
  present BAR larger than `PCI_BAR_VA_STRIDE` (1 MiB), and it mapped every
  present BAR eagerly. A virtio-gpu with `hostmem=4G` presents a 4 GiB BAR 4 →
  the claim failed, and the error named "misconfigured device" rather than
  "this device has a shared-memory window." As-built fix: an oversized BAR is
  claimed but left unmapped (`region()` fails closed on it), and the kernel
  cap walk now discovers shared-memory capabilities (`cfg_type = 8`,
  `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`, 64-bit halves, `id` = shmid) into
  `t_pci_info.shm[2]`, surfaced as `PciDev::shm_region(shmid)`. Mapping a
  subrange of the shm window remains the §6.2 Venus-chunk delta.
- **virgl is PCI-only in QEMU.** There is no `virtio-gpu-gl-device` (MMIO)
  variant. Our GPU is already on PCI, so this costs nothing — but it forecloses
  the MMIO fallback.
- **No local host can run it** — see §2.6 and §9.

---

## 4. The seam

### 4.1 Shape: a 9P service tree, Loom-carried

The GPU seam is a **protocol**, not a process. It is served as a 9P tree in the
Plan 9 idiom the rest of the system uses (`/dev/tapestry`, `/net`, `/proc`), with
the hot path riding Loom exactly as presents already do.

```
/dev/warp/                 (name provisional -- section 11)
  ctl                          # global: capsets, limits, test-mode
  caps                         # read: the capability blob (virgl/venus/v3d)
  ctx/
    new                        # read: allocate a context, yields <id>
    <id>/
      ctl                      # capset <id> | rings <n> | destroy
      submit                   # LOOM_OP_WRITE: an opaque command stream
      bo/                      # buffer objects owned by this context
        new                    # read: allocate, yields <id>
        <id>/
          ctl                  # create <size> <flags> | destroy
          map                  # the Tweft map-capability fid (I-40 shape)
          info                 # read: gpu_va, size, stride, offset
      fence                    # multishot LOOM_OP_READ: completion stream
```

Why this shape rather than a syscall family: it inherits per-Proc namespace
scoping (a Proc that cannot see the tree has no GPU authority — I-1/I-28 do the
access control for free), it is introspectable, and it costs no new kernel ABI
beyond the two deltas in §6. It is also the Genode lesson applied: their
`Gpu::Session` is ~13 operations, and it survived three backends.

### 4.2 The object model

Five objects, per §2.2, each mapped onto something Thylacine already has:

| Seam object | Thylacine realization |
|---|---|
| **device** | the tree itself; its `caps` file is the capset blob |
| **connection** | the mounted session (a client's 9P attach), per-Proc by construction |
| **context** | `ctx/<id>`; carries a capset (virgl / venus / v3d) and N rings |
| **buffer object** | a Burrow of a kernel-minted GPU subtype, shared by `Tweft` |
| **fence / semaphore** | a Loom CQE on the submitting op; no new object |

**GPU virtual addresses are assigned at buffer creation and stable for the
buffer's lifetime.** This is non-negotiable and comes straight from v3d: its
`CREATE_BO` returns the VA, the mapping is pinned for the BO's life, and the
kernel never relocates anything. A seam that maps lazily at submit, or that
expects the server to patch addresses into the command stream, is a seam the RPi
cannot use.

**Submissions are opaque byte ranges.** The server does not parse them (§2.1).
What they *contain* is determined by the context's capset — `VIRGL_CCMD_*` for
virgl (Gallium IR, shaders as TGSI text), `vn_protocol` for Venus, a v3d control
list for v3d. This is what lets one seam carry four consumers whose command
encodings have nothing in common.

### 4.3 Queues and ordering

The seam carries **N ordered submission rings per context**, because both ends
demand it and neither can be retrofitted:

- Venus allocates **one `ring_idx` per `VkQueue`** (0–63), and the virtio spec's
  ordering guarantee is per-timeline: completing `fence_id` retires every
  outstanding command with a lower sequence number *on that timeline*. On the
  global timeline, by contrast, QEMU's own comment says *"the guest can end up
  emitting fences out of order"* — so a FIFO assumption there is wrong.
- v3d has **six** hardware queues (bin, render, TFU, CSD, cache-clean, CPU) with
  cross-queue dependency edges, per-client FIFO, and one job in flight per queue.

A single-queue seam would need surgery for both. Rings are declared at context
creation (`rings <n>`), which is also exactly what virtio's
`VIRTGPU_CONTEXT_PARAM_NUM_RINGS` expects.

**Fences are Loom CQEs.** A fence-bearing submit gets a terminal CQE when its
ring's completion passes it; ordering within a ring is FIFO; `min_complete >= 1`
waits are already death-interruptible (`#811`). On the virtio side this is nearly
free: `fence_id` is guest-allocated, opaque to the device, and echoed back in the
response — **completion is the virtqueue used-buffer notification we already
process.** We are not building fence machinery; we are labelling what we have.

### 4.4 Presentation

**Fullscreen (the game case): unchanged.** QEMU's `SET_SCANOUT` accepts a 3D
`resource_id` directly — it fetches the host GL texture id and hands it to the
console. Our existing `SET_SCANOUT` + `RESOURCE_FLUSH` present loop points at a
3D resource and works, with no `SET_SCANOUT_BLOB` and no blob support at all.
Venus is confirmed to work the same way: resource ids are device-global and
QEMU's blob-scanout path only requires the host side be dmabuf-exportable, which
Venus guarantees by chaining `VkExportMemoryAllocateInfo` unconditionally.

**Windowed / composed:** the compositor reads back (`TRANSFER_FROM_HOST_3D`) into
its screen weave, or — later — composes on the GPU with its own context. GPU
composition is recorded as a follow-on, not arc-v1: it is the Fuchsia
`DisplayCompositor` pattern (prefer direct scanout; fall back to a Vulkan
renderer) and it should be built once the direct path is proven.

**On RPi the same split holds, with a hardware constraint attached:** v3d renders
into scattered pages behind its MMU, but the Pi 5 display pipeline (HVS) **has no
IOMMU and rejects non-contiguous imports**. So the scanout buffer must come from
a contiguous pool and be imported into the v3d side — which is precisely what
Mesa's WSI already does (`CREATE_DUMB` on the display device → PRIME → render
device). Our `t_dma_create` allocations are already contiguous, so the weave is
natively the right object on that side; what the seam must carry is the
**import** direction.

### 4.5 GPU composition — the Warp-C arc (designed 2026-08-13; **BUILT** C-1..C-4 2026-08-16/17, §4.5.10–4.5.12; **C-5 audited + closed 2026-08-17**, §4.5.12 tail)

§4.4 above records GPU composition as a follow-on "to be built once the direct
path is proven." **That precondition is now discharged**: Warp-4 built the
direct path and #215 priced it. This subsection is the design; it was
**RESERVED** in the I-20/I-40 staged sense — the mechanism is fixed, two
premises (§4.5.4) are gating, and it becomes ENFORCED at the sub-chunk that
lands each. (This heading read "RESERVED, not yet built" until 2026-08-17,
two days after C-2 landed — the flip was nobody's step. The as-built record
is §4.5.10a (C-2c), §4.5.11 (C-3) and §4.5.12 (C-4); the audit round that
would make the ENFORCED claim more than the author's is C-5, owed.)

#### 4.5.1 What it costs today, measured

At 1280×800 the composed present is **39.3 ms/frame against the direct path's
22.5 ms — 43% overhead, a measured 1.75× (25.4 → 44.4 fps)**, priced two
independent ways agreeing to ~2% (#215; `docs/reference/149-warp.md`). The
overhead is not one copy but **three passes over the frame**:
`transfer_from_3d_sync` (host→guest, ~4 MB) → `blit_composed_pixels` (a CPU
pass) → `screen_push` (guest→host, ~4 MB). And because direct scanout demands
ONE visible surface AND ONE visible leaf AND an exactly display-sized surface
(`server.rs:1535-1552`), **that 1.75× is the standing cost of *windowing* a GL
client** — the normal case, not the exceptional one.

#### 4.5.2 The mechanism

Make the screen a **host-side 3D resource owned by a compositor-owned virgl
context**, and compose into it with GPU blits.

Per frame: one `VIRGL_CCMD_BLIT` per visible surface, src = the client's 3D
resource, dst = the screen resource, dst box = the pane content rect; all
blits for a frame in ONE fenced `submit_3d`; on fence completion
`SET_SCANOUT`(screen) + `RESOURCE_FLUSH`. **Per-frame guest↔host pixel traffic
becomes zero.**

The load-bearing feasibility fact is already proven by shipping code: the
direct path binds a *client's* host-side 3D resource as the scanout
(`server.rs:5824`, `set_scanout(g.res_id, w, h)`), so a compositor-owned 3D
screen resource is the same operation redirected to a resource we own — not a
new capability. Likewise tapestryd already issues GPU commands on a *client's*
context (the readback uses `g.dev_ctx`), so authoring commands against a
foreign context is existing practice, not a new authority.

Three consequences beyond the headline number:

- **Chrome stops being a per-frame cost.** Borders, strips and console text
  become textures uploaded by `TRANSFER_TO_HOST_3D` **on damage**, not
  re-pushed every frame as `screen_flush_full`/`screen_push` do today.
- **Scaling becomes free.** `VIRGL_CCMD_BLIT` carries a filter and both dst
  and src boxes (21 dwords; `DST_RES_HANDLE`@4, `SRC_RES_HANDLE`@13), so it
  scales and format-converts. A surface need no longer be pane-sized — today
  that requires a CPU scale.
- **N GL clients composite at full speed.** Today each additional GL client
  adds its own full-frame readback.

#### 4.5.3 Why this is ONE model and not two

Everything visible becomes a texture; composition becomes an ordered list of
blits into a compositor-owned target. Chrome, console text, software surfaces
and GL surfaces then differ **only in how their texture is filled** (CPU
upload on damage vs client-side rendering), never in how they are composed.
That is what dissolves the mixed CPU-chrome / GPU-surface problem instead of
special-casing it, and it is the property Venus must later join rather than
bypass (§4.5.5).

#### 4.5.4 The two gating premises — verify BEFORE structural work

Neither is established. Both are cheap and decisive on thyla-pi (real V3D +
KVM), and the arc's first sub-chunk is exactly these probes.

**P1 — a blit must be able to read a resource created by another context.**
§4.4 states resource ids are device-global, which is a strong prior, and
`ctx_attach_resource` is presumably the access-granting step. If P1 is false
the design does not stand. **Trap: a blit that silently no-ops presents as a
black screen, not an error** — so the probe MUST assert *pixels*, with a
positive control (blit a known-nonzero source into a known-zero destination
and require the destination to change), never merely "no error returned".

> **P1a ANSWERED 2026-08-13 — NO, and that is the good answer.** Probed on
> thyla-pi (KVM, real V3D) by the `warp-prove` C-0 leg. With the
> same-context **control passing** (`control same-ctx blit = GREEN`, so the
> `VIRGL_CCMD_BLIT` encoding is valid and blits do move pixels), the
> cross-context attempt is refused by name:
> `vrend_renderer_blit: context error reported 1 "warp" Illegal resource 1080`
> — 1080 being the other context's resource.
>
> **So I-45's context bound is ENFORCED by virglrenderer, verified rather
> than assumed.** This was a live question about the seam we already ship,
> not only about Warp-C: `submit` hands the host an *opaque* stream carrying
> raw device-global resource ids, so it was entirely possible that any warp
> client could read any other client's framebuffer by naming one. It cannot.
>
> **Consequence for Warp-C: P1b is the live path — C-2 must explicitly
> attach each client resource to the compositor context.** This is a better
> design than implicit reach: composition authority becomes a deliberate,
> auditable per-surface grant rather than ambient access across the device.
>
> **The control earned its place.** The first probe run used the wrong
> opcode (21, which is `GET_QUERY_RESULT`; the correct `VIRGL_CCMD_BLIT` is
> 16) and vrend answered `Illegal command buffer`. Without a same-context
> control, that mis-encoding would have produced an identical "no pixels
> moved" reading and been reported as "cross-context access is refused" —
> the same conclusion, reached for a false reason, sending C-2 to build a
> cross-attach verb on evidence that proved nothing.
>
> **Caveat on the oracle, and it is load-bearing (task #240).** The verdict
> rests on the HOST log, not on the guest. The rejection is invisible from
> inside the guest, and the probe correctly reports `NO P1 VERDICT` from the
> pixel channel. **A compositor cannot be built on a submit channel with no
> failure report**, so #240 is a Warp-C prerequisite and it also bounds what
> the P1b re-test can assert.
>
> > *This paragraph as written on 2026-08-13 also claimed "the fence never
> > signals, and the following `transfer_from` never completes — so a refused
> > submit and a hung submit are the same observation."* **SUPERSEDED
> > 2026-08-14 — that was wrong, and wrong in the more dangerous direction.
> > See §4.5.4a.**

> **P1b ANSWERED 2026-08-16 — YES: an explicit `ctx_attach_resource` permits
> the cross-context blit.** Measured on thyla-pi against the real 1.9.0
> virglrenderer, deterministic across 3 runs per arm:
>
> | Arm | `ctx_attach_resource(B, A's res)` | Destination after blit |
> |---|---|---|
> | 1 | yes | **GREEN** — the blit ran |
> | 2 | no | **RED**, `vrend_renderer_blit: context error reported 2 "ctxB" Illegal resource 1080` |
>
> **Answered HOST-SIDE, outside Thylacine, because the guest path is
> circular.** P1b must pass before anything structural lands, but in-guest it
> needs a cross-attach verb that does not exist — `CTX_ATTACH_RESOURCE` lives
> only inside tapestryd and both call sites attach to the resource's OWN
> `dev_ctx`, while the client-facing ctl verbs are only `verify` /
> `present-to` / `submit`. Building that verb is C-2, and P1b gates C-2. Asking
> virglrenderer directly cuts the circle with no guest change, no I-45
> authority decision, and no scripture change.
>
> **The two arms are the point, not the first one.** A success WITH the attach
> shows only that the attach did not *prevent* the blit; it takes the arm
> without it to show the attach is what *permits* it. The pair distinguishes
> two opposite readings — "the attach is the authority gate" versus
> "virglrenderer does not isolate resources between contexts at all", the
> latter of which would mean the guest-exposure half of I-45 cannot rest on the
> renderer refusing.
>
> **The instrument agrees with an independently obtained result.** Arm 2
> reproduces P1a's refusal in the same words (`Illegal resource 1080`), from a
> different program, on a different day, through the host API rather than the
> guest seam. A new instrument that re-derives a known answer before reporting
> a new one is worth more than either measurement alone.
>
> **Consequence for Warp-C: the design survives, and C-2's attach verb is the
> authority-conferral point** rather than a formality — composition authority
> is a deliberate per-surface grant, exactly as §4.5.4 hoped.
>
> **This does NOT retire the C-0d prerequisite.** `submit_cmd` returned **rc=0
> in BOTH arms** — the refusal reports success at the host API too, confirming
> §4.5.4a's finding one layer down. Pixel readback was the only oracle that
> could tell the arms apart. So the *design* risk is closed while the *guest
> readability* gap is untouched: a compositor still cannot be built on a submit
> channel with no failure report.
>
> Probe: `tools/warp/p1b-cross-ctx-blit.c` (`P1B_NO_ATTACH=1` selects the control
> arm). Built against the fetched 1.9.0 headers, never Debian's 1.1.0
> `-dev` package, and linked `-l:libvirglrenderer.so.1` — a header from one ABI
> over a runtime from another is the setup that yields a confident wrong
> answer.

> **P2 MEASURED 2026-08-16 — no reordering observed in 500 unsynced trials, and
> the probe was proven able to see one.** Host-side on thyla-pi (real V3D),
> `tools/warp-host.sh p2`, across queue depths 24 / 64 / 256 at 1024×1024:
>
> | Arm | What it establishes | Result |
> |---|---|---|
> | SYNCED | the probe can report a CLEAN run (encoding, blit, readback sound) | 0 mismatches |
> | **INVERTED** | **the probe can report a DIRTY one** — blits BEFORE the clear, so staleness is guaranteed by construction | **40/40 mismatch, every run** |
> | UNSYNCED | the measurement | **0 / 500** |
>
> **The INVERTED arm is why the result means anything.** A clean UNSYNCED run is
> equally consistent with "the ordering holds" and "this probe cannot see a stale
> read", and separating those is the entire job — the same trap that made P1a's
> same-context control load-bearing. Building it took two corrections, both
> instructive: the arm first scored 39/40 because trial 0 has no previous frame
> to be stale from, and the first fix seeded the *destination*, which changed
> nothing **because a blit overwrites the destination wholesale — staleness here
> is always a property of the SOURCE.** Seeding the source restored a strict
> all-must-mismatch bar rather than relaxing it to *n−1*, which would have
> papered over a real one-trial blind spot.
>
> **What this does NOT establish.** A negative is not a proof: a race that did
> not reproduce is not a race that cannot happen. By the rule of three, 0 events
> in 500 trials bounds any per-trial reorder rate at roughly **0.6% (95%)** on
> *this* stack (virglrenderer 1.9.0 + Mesa/V3D) for *this* access pattern.
> Note also that V3D is a single-queue tiled renderer, so submission-order
> execution may be a property of the hardware here rather than of virglrenderer
> — which would say nothing about a multi-queue desktop GPU. **C-1 still models
> the hazard**; this bounds how hard it is to hit and gives the spec a measured
> starting point instead of a guess.
>
> One structural limit worth stating: each trial ends in a readback, which
> serializes it, so the amount of work that can be outstanding when the blit
> issues is bounded. The INVERTED arm runs under the *same* serialization and
> still detects staleness every time, so the sensitivity claim holds for this
> pattern — but a probe that never reads back would stress it harder and cannot
> detect anything, which is the tension the design has to live with.
>
> Probe: `tools/warp/p2-cross-ctx-order.c` (`P2_DEPTH` / `P2_TRIALS`; the verb
> sweeps `P2_DEPTHS`).

##### 4.5.4a #240 measured — a refusal reads as SUCCESS, and it is sticky (2026-08-14)

Measured on thyla-pi (KVM, real V3D) by the `warp-prove reject` leg
(`tools/warp-host.sh reject`; log `build/warp-reject.log`). Two ctxs built
identically on separate connections, differing in exactly one variable —
whether vrend accepts the submitted stream — with the fence counters read
BEFORE either submit so the delta is attributable to the stream and not to
the ctx build's own fenced work:

```
pre-submit fence-signaled: bad 0  ok 0
t=0ms   bad(poison 0 sig 1 inflight 0)   ok(poison 0 sig 1 inflight 0)
```

The **refused** stream retires its fence at 0 ms exactly like the valid
control. `fence-signaled` 0→1 on both, `fences-in-flight` back to 0,
`poisoned` never sets. **A refusal is indistinguishable from SUCCESS, not
from a hang** — strictly worse, because a hang is at least visible as a
stall. And it is **sticky**: a later *valid* stream on that ctx moves no
pixels (`SENTINEL`) while the identical stream on a fresh ctx works
(`GREEN`), so one malformed submit kills the context for its whole life
while every fence keeps reporting success. The `transfer_from` is still
accepted and still retires — it just delivers stale data — so the readback
path lies too.

**Blast radius is confined to the submitting context** (the second
connection's ctx was unaffected throughout), so this is a robustness and
observability defect rather than a privilege escalation; an unprivileged
client can only self-DoS. But it is squarely a **Warp-C blocker**: the
compositor composes by submitting a per-frame blit stream on *its own*
context on behalf of clients, so one rejection — a stale res id, a retired
surface, a declined format — freezes the screen permanently while every
fence reports composed frames. This is §4.5.4's "present as a black screen
rather than an error" failure mode arriving through the SUBMIT path.

**Why the original reading was wrong is itself the lesson.** It came from a
200-iteration poll — a few hundred ms of 9P round trips — against
tapestryd's `FENCE_ABANDON_MS` of 30 s. It described the *probe's budget*
and was recorded as a property of the *seam*. The old log's missing reaper
line proved nothing either: the probe exited two lines after the rejection,
so the guest never lived 30 s past it. A bound on iterations is not a bound
on time, and an absence observed inside too small a window is not evidence.

C-2 must therefore carry a detection mechanism, not merely an attach verb.

##### 4.5.4b The #240 detector — a sentinel stamp behind a GL-robustness contract (designed 2026-08-14, user-voted; RESERVED)

**The contract is not novel, and that is the point.** Sticky context death
surfaced through a polled status is exactly `GL_ARB_robustness`'s
`glGetGraphicsResetStatus()` and Vulkan's `VK_ERROR_DEVICE_LOST`: the
context is gone, the remedy is *recreate it*, never *retry the operation*.
Plan 9's idiom agrees — an asynchronous device error lives in the device's
own status file, read when the client cares. Thylacine already has that
file, and it already carries a sticky per-ctx failure bit (`poisoned`), so
this is an additive field on an existing surface rather than a new channel:

```
ctx/<id>/ctl:
    poisoned         0     # the FENCE lane abandoned a chain (timeout/wedge)
    stream-rejected  1     # the HOST refused our commands          (NEW, sticky)
    rejected-at      4127  # the submit seq of the verify that caught it (NEW)
```

`poisoned` and `stream-rejected` are deliberately distinct: they have
different causes (a chain that never retired vs. commands the host
declined), and #240 exists precisely because the second was being read
through the first. A client that treats them as one bit re-creates the bug.

**The detector, since QEMU gives us nothing.** `virgl_cmd_submit_3d`
discards `virgl_renderer_submit_cmd`'s return and creates the fence
afterwards unconditionally, so no virtio-gpu field carries the refusal.
The detector must therefore ask the context a question only a *live*
context can answer.

**The probe is stateless, server-owned, and runs at VERIFY time only.**
Two tiny (1x1) resources are minted per ctx at create and never exposed to
the client: `mark`, painted once with a distinctive value, and `sentinel`,
the target. One verify is three steps:

```
1. upload token T into `sentinel`      TRANSFER_TO_HOST_3D   (virtio-gpu cmd)
2. submit  RESOURCE_COPY_REGION(dst=sentinel, src=mark, 1x1) (cmdbuf, stateless)
3. read back `sentinel`                TRANSFER_FROM_HOST_3D (virtio-gpu cmd)

   reads mark's value -> the copy RAN     -> context healthy
   reads T            -> the copy was DROPPED -> context latched, sticky
```

**Why each choice is forced.** Steps 1 and 3 are *virtio-gpu* commands, not
command-buffer commands, so they keep working on a latched context — which
is exactly what §4.5.4a measured (the `transfer_from` still completed and
delivered stale bytes). Step 2 is `VIRGL_CCMD_RESOURCE_COPY_REGION` (13
payload dwords, explicit src/dst handles) because it touches **no bound
state**. That matters more than it looks: virgl context state persists
across command buffers, and Mesa's driver dirty-tracks what it has bound,
so a `CLEAR`-based stamp — which needs `SET_FRAMEBUFFER_STATE` — would
leave the client's framebuffer binding pointing at our sentinel with Mesa
unaware it must rebind. **The obvious stamp corrupts client rendering; the
copy does not.**

**The submit path pays nothing.** An earlier draft of this section stamped
every submit. That was wasted work: the vrend latch is **sticky**, so one
stamped command *after* the last submit of a window detects everything the
window contained. Per-submit stamping bought no extra information, cost a
command (or a whole extra fenced chain) on the hot path, and raised a
question about mutating a client's stream that now does not arise. A
separate command buffer is dropped by a latched context identically — the
§4.5.4a STICKY leg proves it, since its recovery stream was its own submit.

**Cadence is the client's, not the server's**: per-frame, every Nth, or
never. Warp-C verifies once per composed frame, riding the fence sync it
already performs. **The cost, quantified**: ~4 bytes each way against the
~4 MB/frame framebuffer readback Warp-C exists to delete — the thing that
costs 43% of the frame (§4.5.1).

**What it does NOT give you, stated plainly.** Detection granularity is the
*verify window*, not the individual command: `rejected-at` names the submit
sequence of the verify that caught the loss, so the offending stream is
somewhere in `(previous_verify, rejected-at]`. Per-frame verification makes
that window one frame, which is enough to freeze-and-report rather than
freeze-and-lie, but it is not a per-command error code and must not be
documented as one. Narrowing it further would need a probe *between* every
client command, which is not worth its cost.

**Nor does it distinguish WHY.** A dropped copy proves the context is
latched; it does not say which command vrend objected to. That answer
exists only in the host log, and the guest cannot read it (§4.5.4a). The
honest contract is "this context is dead, recreate it" — which is exactly
what `glGetGraphicsResetStatus` and `VK_ERROR_DEVICE_LOST` promise, and no
more.

**Regression witness.** `tools/warp-host.sh reject` (§`149-warp.md`) becomes
the gate: after the fix its rejected arm must report `stream-rejected 1`,
and its class-matched VALID control must still report `0`. Both directions
are required — a detector that latches on everything passes the first
assertion alone, which is the failure this arc has already met twice.

**Audit.** The warp seam is an audit-trigger surface, so the implementing
sub-chunk carries a focused round. The load-bearing questions: `mark` and
`sentinel` must be unreachable to the client through every path that
resolves a BO (an attacker who can write either can forge health, or
manufacture a false rejection against a healthy ctx); the two extra
resources per ctx must be charged against — or explicitly exempted from,
with an argument — the per-ctx BO count and byte caps that #218/#204 sized;
and the probe's own failure arms must fail CLOSED (an upload or readback
that errors is "unknown", never "healthy").

**ANSWERED by the round (2026-08-14), and the first question's premise was
wrong.** Reachability: `mark`/`sentinel` are unreachable through every path
that RESOLVES a BO — and that is not the same as unreachable. The submit
stream is unparsed and carries raw device-global resource ids, which
`bo/<id>/info` hands out from one shared counter, so a client derives
`mark = its_first_res - 2` and overwrites it with a plain
`RESOURCE_COPY_REGION`. **Measured on real V3D**: the server read its mark
back as the client's own green. Containment therefore cannot rest on
naming; it rests on the per-verify **repaint**, which bounds any corruption
to less than one probe. Fail-closed: the arms do return "unknown" — but
"fails closed" is a claim about what the CONSUMER can see, and UNKNOWN had
no representation on the ctl, so a blinded probe read exactly like a healthy
one. `verify-ok` (advanced only on a healthy verdict) is what makes the
distinction expressible; requiring it to move is now the documented contract
for reading `stream-rejected 0`. Cap exemption: the two resources remain
exempt from the BO count/byte caps by design, but the argument that made
that safe assumed they were reclaimed like BOs, and they were not — the
wedge path leaked them permanently until the probe graveyard landed.

**RE-PROSECUTED ON FABLE (2026-08-17, the first non-Opus read after three
Opus rounds; `memory/audit_c0d_fable_closed_list.md`): 0 P0 / 2 P1 / 1 P2 /
2 P3, dirty.** What it found, and what changed:

- **F1 [P1] — the client probe was still a TEXTURE pair after C-4.** C-4
  (§4.5.12) measured that on a tiled renderer every texture transfer and
  readback is a blit job behind everything the *device* has queued, and
  moved the *compositor's* health pair to buffers — the *client* detector
  kept the texture pair. So client A's `verify` blocked the console for as
  long as client B's queue was deep, while the `verify` admission gate
  (`fences-in-flight`/`poisoned`, audit F7) reads only A's own gauges and
  could not see it. **Fixed:** every ctx's probe is now minted with
  `warp_hprobe_build` (buffers), the texture pair only where that mint
  fails (counted as `probe-texture` on the global ctl); the verify's
  transfers and copy width follow the pair's kind through one set of
  helpers shared with the compositor's health pair (`probe_upload` /
  `probe_readback` / `probe_copy_region`); the C0-F1 prover leg attacks the
  buffer pair from a BUFFER source of the probe's own shape (a
  texture->buffer copy is not a legal copy and would defend for the wrong
  reason). The gate's contract, stated exactly now: it bounds waits on the
  caller's OWN queue — which on the buffer pair is the whole exposure.
- **F2 [P1] — the composed READBACK arm** (`transfer_from_3d_sync` of the
  frame under the client's ctx, the `!done` fallback of the composed-GL
  present) is a synchronous full-frame readback whose wait is the client's
  own queue, on the console's dispatch thread, taken for every BO the blit
  arm cannot compose; `fence_poisoned` structurally cannot guard it (the
  poison comes from `reap_abandoned` on the serve loop — the loop that is
  blocked). Only readbacks carry this: a blit's SUBMIT_3D response is
  written at decode time. Gating the arm on `fences-in-flight == 0` was
  rejected (it would collapse the §4.5.9 CPU safety net to stale frames for
  every continuously-rendering client). **OPEN, stated exactly in
  `149-warp.md` and at the arm; the fix is Warp-C C-6, the fenced / bounded
  readback (§4.5.13), the pipelined form §4.5.12 already cut for.**
- **F3 [P2] — the probe's 2 pages per ctx mint ride the never-reclaimed
  `weave_va_next` bump** (a ctx-churn driver on the #171 class). Recorded on
  #171 (its reclaim must cover the probe pages); the ~186-day bound stands,
  composed with #171's. Comment at `warp_probe_res_kind`.
- **F5 [P3] — the `present-to` import witness had no rate limit**: `N bo` /
  `off` / `N bo` re-ran the attach + health copy + two witness rounds on the
  SHARED compositor context at 9P-write rate. **Fixed:** one witness per
  ctx per compositor tick (`WarpCtx.import_tick`, the `verify_tick` shape);
  a second consent in the same tick is deferred to the next tick's replay
  (`comp_replay_deferred_imports` in `frame_tick`), never dropped.
- **F6 [P3] — the reject scenario passed on a blind detector**: warp-prove
  printed `C0-REJECT DONE` unconditionally and only `warp-host.sh reject`'s
  5-term grep gated it. **Fixed:** DONE iff every C0 arm passed, else
  `C0-REJECT INCOMPLETE(<arm>)`, which `warp-reject.exp` hard-fails on
  (`lc_run_expect_hardfail_re`); the 5 terms stay as the belt.

The verified-sound list of the round (repaint containment, the three-valued
verdict on every client-facing surface, probe teardown in I-7 order, slot
poison / dev_ctx aliasing, the REQ_REGION bound, the `composable` gate,
present-to cross-conn safety) is in the closed list. Dirty close: a
follow-up round on these fixes + C-6 is owed after C-6 lands.

**P2 — cross-context ordering: does the blit observe the client's finished
frame?** The client renders on its context, the compositor blits on its own;
virglrenderer maps each guest context to a host GL context, and in-order
controlq dequeue orders the *commands* but not GL execution across two host
contexts sharing an object. Today the hazard is masked because
`transfer_from_3d_sync` must produce bytes and so forces the sync as a side
effect; **a blit has no such side effect, so the hazard goes live exactly when
the readback is removed.** The failure mode is a torn or stale frame — an I-40
tearing-freedom violation, i.e. a soundness question, not a perf detail. This
is where the spec work concentrates.

##### 4.5.4c #240 generalized — a virtio-gpu OK is NOT the renderer's verdict for the whole mint family (found 2026-08-17)

§4.5.4a measured that a refused `SUBMIT_3D` reads as success. That finding was
filed against one command and not checked against its neighbours, and the
neighbours are worse. Read from QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c`
(thyla-pi runs 10.0.11), the virgl path's handlers **ignore the
`virgl_renderer_*` return value** for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`,
`CTX_ATTACH_RESOURCE`, `CTX_DETACH_RESOURCE`, `TRANSFER_TO_HOST_3D`,
`SUBMIT_3D` and `CTX_DESTROY`, and for `ATTACH_BACKING` check it only to
clean up the iov mapping — never setting `cmd->error`. What an OK response
attests for those is that **QEMU parsed the command**: a nonzero,
non-duplicate resource id (QEMU keeps its own `reslist`, and inserts into it
*before* calling the renderer, so its list and the renderer's can disagree), a
valid iov, a permitted `context_init` flag. Whether virglrenderer created,
attached, or transferred anything is invisible. Two commands DO consult the
renderer: **`SET_SCANOUT`** (`virgl_renderer_resource_get_info_ext` — an
unknown resource is `INVALID_RESOURCE_ID`) and `RESOURCE_UNREF` (QEMU-side
existence only).

**What that falsified, and what it broke.** The C-2b gate's header and
`149-warp.md` said the screen's "3D" word was "the conjunction of four
response-checked round trips the host answered OK — a claim about the host
accepting the object". False: those four are exactly the ignored ones. And it
was not only prose: `alloc_screen`'s "a 3D failure is NOT fatal — it falls back
to the 2D resource" was dead for a renderer-side refusal, because
`resource_create_3d(..).is_ok()` is true whenever QEMU parsed it, so `is3d`
reduced to `comp_ctx`, "3D" printed, and the failure landed later — silently —
as an `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose result the
code dropped, leaving the *previous* scanout on the display. The C-2b gate
would have passed identically against that host.

**The repair, in the shape #240's detector already established: make the
producer prove it, with pixels.** `alloc_screen` now earns "3D" by a sentinel
round trip through the compositor context — write 16 sentinel pixels into the
fresh screen's backing, `TRANSFER_TO_HOST_3D`, clobber the backing,
`TRANSFER_FROM_HOST_3D`, compare, restore the zeros. It succeeds only if the
renderer holds the resource, has it attached to `COMPOSITOR_CTX`, and moves
pixels through it — none of which a response can fake, and each of which a
refused create or attach silently defeats (their transfers become no-ops at
the renderer). A refusal now falls back to 2D *for real* and the screen line
says why (`-- 3D refused: create | ctx attach | attach backing | renderer round
trip`). The composed switch prints its line **after** the bind with the verdict
(`res N bound` / `BIND FAILED`), and `composed-screen.exp` grew a fifth term:
the resource the display was handed IS the minted screen. Sabotage that
discriminates it — a bogus `VIRGL_FORMAT` (0x7FFF) in the 3D create, which
the renderer refuses and QEMU answers OK — measured on thyla-pi (results in
`149-warp.md` "What the 3D word attests"): the reason printed is `renderer
round trip`, i.e. create, attach and backing all returned OK from the device
under a format the renderer cannot accept — the measured form of this
section's claim — and the old `is3d` (`comp_ctx && create.is_ok() &&
attach.is_ok()`) would have printed 3D.

**Consequence for C-2c and C-3, stated here because it changes their gates.**
`CTX_ATTACH_RESOURCE`'s response witnesses nothing, so C-2c cannot be verified
by its attach responses at all: its gate is P1b's two arms in-guest — attach +
one blit + readback pixel oracle, with the no-attach control red — which
means C-2c lands *with* the first blit witness rather than before it. And
every future "the device accepted X" claim in this arc names which command's
response carries a verdict, or reads back pixels.

#### 4.5.5 Venus — the model generalizes, the mechanism must be extended (F2 vote, 2026-08-13)

The composition **model** is capset-neutral. The composition **mechanism** is
not: `VIRGL_CCMD_BLIT` is a virgl command interpreted by a virgl context,
while a Venus client's resource belongs to a Venus context, so **one
compositor context cannot blit across capsets.** The capset-neutral escape is
blob/dmabuf sharing — and §4.4 already notes Venus chains
`VkExportMemoryAllocateInfo` unconditionally, so its resources are always
dmabuf-exportable.

**Decided (user vote 2026-08-13): virgl-first; Venus at Warp-6** — consistent
with F2, which already sequences blobs "with Venus at Warp-6". Rationale:
Venus is a genuine SEAM and not a dependency of this chunk (composition works
fully without it), so the chunk-completeness rule does not force it; and #166
means hostmem is inert under HVF on the M2 dev host, so a blob-first design
could not be exercised on the primary dev loop.

**The obligation this vote carries is binding on the arc:** the composition
model must stay free of virgl-specific assumptions, and the blob-mediated blit
must be named as the capset-neutral generalization, **so that Warp-6 extends
the mechanism without reshaping the model.** A Warp-C design that would force
a second compositor path at Warp-6 has failed this requirement.

#### 4.5.6 Relationship to I-40 — an obligation discharged, not amended

**I-40 does not mandate synchrony.** It mandates quiesce-before-retire
(`ServerRelease` in `specs/tapestry_present.tla`); synchrony is merely the
stage-0 mechanism that discharges it "BY CONSTRUCTION". Both the §28 I-40 row
and the tapestryd audit-trigger row pre-record the successor's obligation in
terms — *"a pipelined controlq, G-6+, must implement a real drain before
touching retire"*. Warp-C is therefore the anticipated evolution occupying a
slot scripture cut for it, and **the arc owes a real drain plus a
demonstration that it discharges `ServerRelease` and `NoStaleMap` as strongly
as synchrony did — not an amendment to I-40.** `tapestry_present.tla` is
model-first and spec-first is RE-ENABLED on this surface, so the model is
extended BEFORE the impl, with a `drain_skipped` counterexample cfg and (per
P2) a cross-context-ordering counterexample.

> **C-1 LANDED 2026-08-16 — the model is extended and TLC-green, and it
> refuted a premise of its own first draft.** `tapestry_present.tla` gains the
> composed path behind `ALLOW_COMPOSE`: `Attach`/`Detach` (P1b's
> authority-conferral point), `ComposeBlit`/`ComposeComplete`, the
> `DrainedOfBlits` conjunct on `ServerRelease` + `Free`, and two invariants
> repeating T-1's own LIFETIME/CONTENT split — `NoTornCompose` (pages stay
> backed under an in-flight blit) and `NoStaleCompose` (a blit and a fill of
> one host resource never overlap). Eleven cfgs, all reporting what their
> headers claim: 4 clean (`tapestry_present`, `_liveness`, `_composed`,
> `_composed_liveness`) + 7 buggy, verified by `specs/check-tapestry.sh`.
>
> **The additive claim is CHECKED, not asserted.** With `ALLOW_COMPOSE =
> FALSE` the two new variables never leave their initial values, so the six
> pre-existing cfgs must reproduce their exact distinct-state counts — and do
> (5413, measured before and after). Coverage confirms the composed actions
> fire `0:0` on the direct path and 2264/7328 times on the composed one, so
> the green is over a constructed state rather than an unreachable one.
>
> **What the model found, and it is a real design obligation for C-2/C-3.**
> The first draft carried the in-flight blit as the *slot* it reads, on the
> assumption that slots are host-side buffers — so a client filling a
> different slot during a composition would be legitimate pipelining. TLC
> refuted the model built on that assumption, and the tree refutes the
> assumption: tapestryd allocates **one 2D resource per surface** with
> whole-weave `ATTACH_BACKING` and transfers at a per-present *offset*
> (`usr/tapestryd/src/gpu.rs`). Guest-side slots buy **no host-side
> concurrency**, so a fill of any slot collides with a blit, and the exclusion
> is whole-resource. Concretely: **the D1 recycle gate does not survive the
> composed path unchanged.** In the direct path a slot is released by its
> present's terminal CQE, and that CQE genuinely does mean the host has
> finished reading; once the compositor is a *second* reader of the same host
> resource, the CQE stops meaning the resource is free — and nothing in the
> old rule notices. C-2/C-3 must supply that exclusion (fence ordering, or a
> double-buffered host resource), which is the sort of thing that is cheap
> here and expensive at C-4.
>
> The exclusion is symmetric, so it is sabotaged per direction rather than
> once: `buggy_blit_during_fill` (P2 proper — the absent cross-context sync)
> and `buggy_fill_during_blit` (the buffer-in-use violation). A single flag
> opening both gates would only ever demonstrate whichever end TLC reached
> first.
>
> Note what `_composed_liveness` is for: the load-bearing question a real
> drain raises is not safety but **termination** — a drain that blocks forever
> strands the weave as surely as no drain corrupts it, and stage-0 synchrony
> could not deadlock because it had nothing to wait on. `EventuallyRetired` is
> verified over the complete 44696-state space (TLC: *"Checking temporal
> properties for the complete state space"*), so the drain provably does not
> deadlock teardown.

#### 4.5.7 Prior art

The pattern is **Fuchsia's `DisplayCompositor`** (prefer direct scanout, fall
back to a renderer that composes) — already selected by §4.4, and our
Direct/Composed split *is* that pattern with the fallback's GPU half unbuilt.
SurfaceFlinger + HWC is the per-layer generalization (delegate what you can to
overlay planes, GPU-compose the rest); it is not reachable today because
virtio-gpu's multiple scanouts are displays, not overlay planes.

**What we deliberately do NOT inherit is the buffer-sharing half.** Wayland
(`linux-dmabuf` + `drm_syncobj`) and Fuchsia (sysmem buffer collections, the
sharper form) both exist to negotiate allocation and import between mutually
distrusting processes holding separate GPU contexts. **Thylacine has no such
negotiation to do:** the compositor IS the GPU driver and the sole
`/dev/warp` server, both resources already live behind one device that one
trusted process owns, and the capability is the warp ctx — already
namespace-gated (I-1/I-28). See NOVEL.md for the angle.

#### 4.5.8 The blit/fill exclusion — one host resource PER SLOT (user-voted 2026-08-16; RESERVED, lands at C-2)

**Surfaced by C-1's model, not by the prose.** The composed path makes the
compositor a second, *asynchronous* reader of a client's host resource. The
guest weave is triple-buffered — *"one weave carries three page-aligned
slots"* (`usr/tapestryd/src/server.rs:198`) — but the host side is
**single-buffered**: tapestryd allocates one 2D resource per
surface-*generation*, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot
(`usr/tapestryd/src/gpu.rs:1515-1518`). So guest-side slots buy **no
host-side concurrency**, and a fill of *any* slot collides with a blit.

In the direct path this is harmless, and that is exactly why it went
unnoticed: the transfer is synchronous inside one dispatch, so nothing else
is ever touching the resource. **The consequence is that the D1 recycle gate
does not survive the composed path unchanged.** A present's terminal CQE
genuinely does mean "the host has finished reading" — until the compositor
becomes a second reader of the same object, at which point the CQE stops
meaning the resource is free and *nothing in the old rule notices*. This is
the composed-path twin of I-40's own LIFETIME/CONTENT split, and
`tapestry_present.tla`'s `NoStaleCompose` + the
`buggy_fill_during_blit` cfg are its counterexample.

**Decided: give each slot its own host resource** — slot ↔ host resource
becomes 1:1, restoring the symmetry the offset-transfer collapsed. The
collision then *does not exist* rather than being scheduled around, and the
existing per-slot D1 discipline extends directly instead of needing a second,
parallel rule. Resource ids stay per-generation and minted above
`SCREEN_RES` (the #317 no-alias property is unchanged); a generation simply
mints three of them, and `ATTACH_BACKING` becomes per-slot at the existing
`slot_stride` rather than whole-weave.

Alternatives considered and rejected:

- **Double-buffer the host (2×)** — the minimum that removes the collision,
  at 2× VRAM instead of 3×, with slot *N* transferring into resource
  *N* mod 2. Rejected because that mapping corresponds to nothing else in the
  design: it introduces a new concept to save one buffer, where 1:1 reuses a
  correspondence the client, the weave layout, and D1 already share.
- **Keep one resource and fence-order it (1×)** — no extra VRAM; the blit
  waits on the fill and the client's next transfer waits on the composition
  fence. Rejected because it makes the guest's triple buffering stop buying
  what it exists to buy: the client could still *draw* ahead but no longer
  *upload* ahead, re-introducing a serialization Warp-C exists to delete.

**The cost is real and is stated rather than buried:** 3× host VRAM per
surface (~12 MB at 1280×800; ~100 MB at 4K). It interacts with the existing
64-MiB weave cap, which already cannot hold a triple-buffered 4K weave — the
G-6d F5 finding, task #44, whose disposition is a graceful `E_NOMEM`. C-2
inherits that bound rather than changing it.

**GL surfaces need the other mechanism, and both are owed.** Where a surface
has a `gl_src` the fill is the client's own GL stream, not a
`TRANSFER_TO_HOST_2D`, and you cannot double-buffer a client's render target
from outside it — so ordering there must be a **fence**, which is P2 proper
(§4.5.4) and what `buggy_blit_during_fill` models. This is the same split the
SOTA arrived at independently: Wayland pairs `wl_buffer.release` for software
clients with `drm_syncobj` for GPU ones, and Android pairs BufferQueue
release with acquire fences. Note this does **not** reopen §4.5.7's
deliberate non-inheritance: what we decline from Wayland/Fuchsia is buffer
*negotiation* between mutually distrusting allocators, which tapestryd does
not need because it allocates the weave itself. Buffer *release* is a
different thing, and we already have it — the CQE.

**The landed model does not change with this vote.** `NoStaleCompose` is
stated whole-generation, which is correct for today's geometry and remains
sound — merely conservative — once slots become distinct host objects.

##### 4.5.8a REFUTED AS WRITTEN — per-slot resources break damage-only presents (found 2026-08-16, before C-2d wrote a line; OPEN, needs a vote)

**The vote stands; its stated mechanism does not survive the tree.** Reading the
present path to implement §4.5.8 turned up a dependency the decision analysis
did not have. Three facts, each checkable:

1. **Every client rotates slots on every present** —
   `self.cur_slot = (self.cur_slot + 1) % self.nslots`
   (`usr/lib/libtapestry/src/lib.rs:525`), unconditionally, in both scanout
   modes.
2. **Nothing carries content from slot *N* to slot *N+1*.** `pixels()` hands
   back the raw current slot (`lib.rs:396-404`); there is no copy-forward
   anywhere in the client or the server. So a slot's non-repainted pixels are
   whatever that slot held `nslots` presents ago.
3. **Today the single per-generation host resource IS the accumulation
   buffer.** A damage-only present transfers only its rect
   (`server.rs:6770-6778`), so the host resource retains the rest of the
   previous frame and the stale guest slots never reach the host.

**Therefore one host resource per slot makes damage-only presents render a
`nslots`-frames-stale background around each fresh rect** — in Direct
immediately, and in Composed at C-3 once the blit sources from a slot's
resource. This is not a corner: **aurora, the console renderer, is a
damage-only accumulator** (`usr/aurora/src/main.rs:1027-1038` renders only rows
`r0..r1` and presents that rect) **and is the default Direct-scanout client on
every boot** — the `scanout direct 0 (1280x800)` line in every Pi log.

A second, smaller consequence rides along: per-slot resources also make Direct
scanout rebind (`SET_SCANOUT` + the #57 post-bind full flush) on every slot
rotation. That is normal for a display stack — it is exactly a KMS page flip
with a per-buffer framebuffer — but it is a per-frame cost on the path Warp-C
exists to make fast, and it was not in the comparison either.

**Options, for the vote. The recommendation is 4.**

1. **Client copies slot *N-1* → *N*.** Correct, trivially. Costs a full-surface
   memcpy per frame in every client (~4 MB at 1280×800), which is precisely the
   copy the zero-copy weave exists to delete. Rejected on its face; listed
   because it is the obvious first thought.
2. **Keep one host resource per generation; fence the blit against the fill.**
   §4.5.8's own rejected 1× option — but this finding is a NEW argument for it,
   because the single resource is not merely cheaper, it is *doing a job*
   (accumulation) that nothing else currently does. Cost is the serialization
   the vote rejected: draw-ahead survives, upload-ahead does not.
3. **Per-slot resources PLUS a separate accumulation resource** the compositor
   blits from. Restores correctness at 4× VRAM plus a host-side slot →
   accumulator copy per frame. Strictly worse than 4 on both axes.
4. **Per-slot resources plus BUFFER AGE** — report how old the slot handed back
   is, and let an accumulator repaint the union of damage since that slot was
   last presented. **This is exactly `EGL_EXT_buffer_age` + Wayland's
   `wl_surface.damage_buffer`**, the mechanism the rest of the world built for
   this identical problem, and Android's BufferQueue exposes the same thing.
   No extra VRAM, no memcpy; the client repaints slightly more area. Costs a
   small ABI addition (the age on the present completion) and a change in
   aurora. It keeps the §4.5.8 vote intact and makes it implementable, and it
   retires a latent hazard rather than working around one.

**Do not implement C-2d until this is settled** — every option changes what
C-2c attaches and what C-3 blits from.

##### 4.5.8b RATIFIED: buffer age, delivered client-side (operator vote 2026-08-16)

**Option 4 chosen.** Per-slot host resources stand as voted; an accumulator
repaints the union of damage since the slot it is about to draw into was last
presented. First use of a slot, and any invalidation, means a full repaint.

**Which path actually needs this, sharpened — it is DIRECT, not composed.**
Worth stating because §4.5.8 solves a *composed*-path problem (the blit/fill
collision) and the breakage it causes is a *direct*-path one, and conflating
them will mislead C-3.

- **Direct** scans out the whole resource (`set_scanout(R_S)`), so `R(S)` must
  be correct **everywhere**. Today one accumulating resource per generation
  supplies that; per-slot resources remove it, and the client must supply it
  instead. This is what buffer age is for.
- **Composed** blits only the damage rect out of the client and into the
  **screen**, which is itself the accumulator and retains everything outside
  that rect. So the client's resource only has to be correct *inside the damage
  rect* — which it trivially is, having just been painted and transferred.
  Composition therefore needs no age at all, before or after C-3.

The conclusion is unchanged, because aurora is a Direct client on every boot —
but the scope matters: **C-3 keeps the screen as the composed accumulator**, and
the age contract exists to protect the scanout path.

**The correctness argument, by induction on a slot's resource.** Resource
*R(S)* holds exactly what has been transferred into it. If every present of
slot *S* transfers the union of damage since *S* was last presented, then
*R(S)* = *R(S)*<sub>last cycle</sub> ∪ (everything that changed since), which
is the whole current frame provided *R(S)* was correct last cycle. The base
case is the full-surface transfer on first use. Nothing else is required of the
compositor **for the guest-slot content**, because the staleness is entirely a
property of what the client itself wrote.

**Where the age comes from, and why not the CQE.** The vote's sketch put it on
the present completion. That is not available: a present is a 9P write over the
Loom ring, so its CQE is **kernel-owned** — `result` is the write's byte count
and `flags` carries `LOOM_CQE_*` semantics, with `struct loom_cqe`
`_Static_assert`-pinned at 16 bytes (`kernel/include/thylacine/loom.h:177-186`).
Putting a compositor payload there is a kernel ABI break, which is out of all
proportion to the need. Two alternatives were considered and rejected: a new
`TEV_AGE` event races the client's rotation (events are async to the present),
and a control word inside the weave is a client-visible layout change for
something the client can already compute.

**So the age is derived in `libtapestry`, which owns the rotation** —
`cur_slot` advances only in `submit_present` after that present's own CQE, so
the library knows exactly when each slot was last presented, and a failed
present does not rotate.

**The obligation this creates on the compositor, stated because leaving it
emergent is the mistake §4.5.8a is about.** A derived age is correct only if
the client learns of every event that invalidates a slot's host resource. So:

> **Invariant (C-2d):** tapestryd MUST NOT skip, drop, or filter a present's
> transfer without the client subsequently receiving a redraw request, and a
> redraw request invalidates **every** slot — the client repaints full for
> `nslots` consecutive presents, not one.

Both halves already have machinery — hidden surfaces skip their transfer
(`server.rs`, the composed arm) and regaining visibility emits the redraw
`CONFIGURE` fan (`reconcile`, the structural arm) — and a reweave resets
`cur_slot` to 0 with all slots undefined (`lib.rs:392`). What is new is that
these are now **load-bearing for correctness rather than for freshness**, and
therefore get a named invariant, a test, and a line in the reference. The
"repaint full for `nslots` presents, not one" half is the easy thing to get
wrong: one full repaint after unhide fixes one slot and leaves the other two
stale.

**Sequencing — split into C-2d-a (client) then C-2d-b (server).** The two are
not symmetric, which is what makes the split safe. Per-slot resources without
age would break every accumulator, so the server half can never go first. Age
without per-slot resources is *inert but harmless* — a client repainting the
union of the last `age` frames' damage produces byte-identical pixels on
today's accumulating single resource, and merely redraws more. So the client
half lands first, gated on "unregressed" rather than on any new behaviour, and
the server half lands into a tree where every client is already correct.

**Be exact about what C-2d-a can be verified to do: nothing observable.** Its
effect is invisible until C-2d-b removes the accumulator, so its gate is the
`ls-gfx*` pixel asserts staying green, plus the reasoning above. Claiming more
would be claiming a green boot proves a gate is wired.

**C-2d-b's prerequisite list — corrected, and the correction is the
instructive part.** The first pass grepped `present(Some\|present_rects` and
reported three clients. That is a match on **API shape**, not on the property
that matters, which is *damage smaller than the full surface*. Checked
properly:

- **`usr/aurora`** — a genuine accumulator. Done, C-2d-a.
- **`usr/tapestry-demo`** — a genuine accumulator, and the sharpest example in
  the tree: it paints the quadrant background **into slot 0 only**, at frame 0
  (`main.rs:107-113`), then every later frame draws just the plasma box into a
  *rotating* slot and presents only that rect. Slots 1 and 2 therefore never
  receive the background at all — they hold the alloc-time zeros. Today that is
  invisible because the one host resource retains frame 0. Under per-slot
  resources two frames in three would show black around the plasma. **Owed.**
- **`usr/tapestry-battery`** — **needs nothing.** Every present is
  `present(None)`, and the one `present_rects` (`main.rs:480`) tiles the whole
  surface with two rects after writing every pixel, so its damage union is
  full-frame. Its own header says so: *"The battery presents FULL-FRAME only
  … so it never trips the #56 patchwork latch."*

So the list is one client, not two — and the over-count came from the exact
failure mode this project keeps re-learning: *a pattern that matches the wrong
thing returns a confident wrong answer, never an error.* `present_rects`
covering the full surface is not partial damage.

**Also owed at C-2d-b, from self-auditing C-2d-a: aurora has a `CONFIGURE` path
that bypasses `handle_configure`, and therefore bypasses invalidation.** The
sub-floor arm (`main.rs:800`) declines a degenerate resize offer by marking
every row dirty *without* calling `handle_configure`, so `invalidate_slots`
never runs on it. It is benign today — that arm corresponds to the compositor
cropping rather than skipping transfers, and each slot's margins were painted on
its own age-0 first use — but it is the wrong shape: **invalidation lives inside
a library call the client is free to route around.** Either invalidate on that
arm too, or expose invalidation independently of `handle_configure`. Worth
noting *why* it is benign, because the reasoning is the load-bearing part: a
damage-only present spans rows, never the surface margins, so margins are
correct in every slot only because age 0 routes that slot's first use through
the full-frame branch, which BG-fills the whole surface. Remove the age-0 route
and margins rot in slots 1..n-1.

##### 4.5.8c C-2d-b IS LANDED AND UNVERIFIED — no gate can see it fail (2026-08-17; CLOSED the same day by §4.5.8d, kept as the record of what was missing)

**The sabotage passed, and that is the finding.** With per-slot resources live,
aurora's age handling was disabled — `stale_slot = false`, `back = 0`, i.e.
exactly the pre-C-2d-a client against a server that no longer accumulates — and
**`ls-gfx` still reported PASS.**

So the two gates that looked like verification are not. `ls-gfx` asserts the
frame *looks like a console* (`screendump -c`) and that dumps *differ* after a
command; neither notices a three-frames-stale background around fresh rows. And
`ls-gfx-panes` drives `tapestry-battery`, which presents **full-frame only** and
therefore never exercises the accumulator path at all. Between them they cover
everything about the compositor except the property C-2d changes.

This is the "a green boot proves the gate did not fire" trap one level up: the
implementation is landed, the existing gates are green, and **there is no
evidence the rendering is correct** — only evidence that these gates cannot
tell. It is stated here rather than discovered later.

**What the missing gate must do:** paint a region, damage only a *different*
region, rotate through all `WEAVE_SLOTS`, then sample a pixel in the region that
was painted several frames earlier and not since. A correct client repaints the
union and the pixel holds; a broken one shows what that slot held `nslots`
presents ago.

**It must run in DIRECT scanout, which rules out the obvious vehicle.** The
first draft of this paragraph said to give `tapestry-battery` a damage-only
stage. That cannot work, and the reason is §4.5.8b's own scoping: **Composed
blits only the damage rect into the screen, and the screen is the accumulator**,
so every pixel outside the damage rect comes from the screen's history and a
stale client slot is *invisible*. The battery runs in Composed (two panes) —
a battery-based test would be green against a completely broken client.

So the vehicle has to satisfy Direct's conditions — one visible leaf, sole
visible surface, display-sized — which in practice means **aurora**, painting
the console. Shape: paint the top of the screen, issue several commands that
damage only the bottom, then assert the top region.

**And that detector is inherently probabilistic, which its author must handle
rather than discover.** The scanout shows whichever slot was presented last, so
a broken client renders a bad top only ~`(nslots-1)/nslots` of the time; a
single sample passes a broken build one time in three. Require the region to be
correct across N consecutive dumps taken over a period spanning many presents
(the cursor blink guarantees presents keep happening), and state N's false-pass
rate rather than leaving it implied.

`ls-gfx-panes` already has the sampling machinery (`screendump -P` +
`ppm-sample.py`), so the instrument exists; what is owed is the scenario.
**Validate it by re-running the same sabotage — `stale_slot = false`,
`back = 0` in aurora — and requiring it to go RED.**

Until that exists, treat C-2d as **implemented, not verified**, and note the
focused audit is owed too (`usr/tapestryd` is an I-40 audit-trigger surface and
this changes the live scanout path).

**C-2d-b contains a design sub-problem that is NOT a mechanical edit: HOLD ×
per-slot × the scanout rebind.** Found by starting the refactor and stopping at
it (2026-08-17); recorded rather than decided in a hurry, because it changes a
surface the battery's hold legs cover.

`TPRESENT_HOLD` defers a present's device-visible flush until `release`, and
`Held::Direct(Rect)` accumulates the deferred region as a **rect union**
(`server.rs:6918`). That union is well-defined only while every held present
lands on **one** host resource. It does not survive per-slot resources: each
present rotates the slot, so two held presents sit on *different* resources,
and there is no single resource for the union to be flushed against.

It compounds with the scanout rebind. In Direct mode the scanout must name the
presented slot's resource, so a present now implies a `SET_SCANOUT` — but a
*held* present's whole contract is that nothing becomes device-visible yet, so
the rebind has to defer too, and then "which resource is bound" and "which
resources have unflushed regions" come apart.

Shape that looks right, for whoever picks this up: replace the single
`Held::Direct(Rect)` with **at most `WEAVE_SLOTS` (slot, rect) entries** —
bounded by construction, since a client cannot hold more presents than it has
slots — and flush each on release, rebinding the scanout last to the most
recent slot. Rejected on sight: keeping one union and flushing "the current
slot", which silently drops the other slots' held regions; and superseding an
older hold with a newer one, which loses a flush the client was promised.

Note the composed arm is unaffected — `Held::Composed` is a SCREEN-space
region, and the screen is one resource regardless.

**C-2d-b also owes a decision on `res_stale`.** Today it means "this client
resource has no valid content" and forces a full-surface transfer of the current
slot on a direct switch — which pushes an accumulator's *stale slot memory* to
the host, and is survivable only because the switch also emits the redraw
`CONFIGURE` that makes the client repaint next frame. Per-slot resources make it
per-slot, and the age contract arguably subsumes it; keeping both is belt and
braces, dropping it needs the redraw emission to be proven unconditional. Decide
it explicitly rather than porting it by reflex.

##### 4.5.8d The gate exists, both sabotages go red, and C-2d is VERIFIED (2026-08-17)

**`tools/interactive/ls-gfx-age.exp`** (LS-CI, HVF) with its instrument
`tools/interactive/gfx_region.py` — the shape §4.5.8c specified, with the two
things it left to the author decided:

- **Vehicle**: aurora, in Direct scanout, per §4.5.8c. The scenario reads the
  grid off aurora's own `console up WxH cells (... cell cwxch)` bringup line
  and works in **cells** (rows 6..rows-3, cols 2..cols/2), so it survives a
  font or mode change and stays clear of row 0, of the bottom rows, and of
  every margin.
- **Fill → positive control → clear → negative leg.** Three `yes … | head -n
  200` runs scroll glyphs through every row of every slot; then the SAME
  instrument that will assert "no text" first asserts "text" on four
  slot-rotated dumps (a negative assertion with no positive twin is satisfied
  by a broken fixture — aux#215); then `clear` (`ESC[2J`) blanks every cell in
  one all-rows present into ONE slot; then eight rounds of keystrokes (each a
  row-0-only redraw, i.e. a present into the NEXT slot) + a dump, each of which
  must show the region **exactly** background — `off == 0`, every pixel read,
  no stride, because a subsampled zero proves nothing about the pixels it
  skipped.
- **The slot phase is driven, not waited for.** The screen shows the slot
  presented LAST, so one dump samples one slot, and a broken client has 1 or 2
  stale slots of 3 (no age handling: 2; an off-by-one in the union: exactly 1).
  Rounds type 1,1,2,1,1,2,1,1 keys: for ANY constant number of blink presents
  per round the cumulative advance visits all three residues mod 3 within the
  eight rounds (b=0: 1,2,1,2,0,2,0,1; b=1: 2,1,1,0,2,2,1,0; b=2: 0,0,1,1,1,2,2,2),
  so every slot is sampled and either class is caught deterministically. Only
  if the blink count varies mid-leg does that degrade to the independence
  bound — (1/3)^8 = 1.5e-4 for the no-age class, (2/3)^8 = 3.9% for the
  one-stale-slot class — which is why the constant-rate coverage, not the
  bound, is the load-bearing claim, and why the 1,1,2 pattern exists at all
  (a plain 1-per-round pattern never visits residue 2 under b=0 and would pass
  the off-by-one class every time).

**Measured, 2026-08-17, HVF, 128×36 cells / cell 10×22 / region x 20..640
y 132..726 (368 280 px):**

| build | positive control | negative leg | verdict |
|---|---|---|---|
| fixed aurora (below) | 63 882/368 280 non-bg on 4/4 dumps (17.3%, identical — every slot holds the same fill) | **0/368 280 on 8/8** dumps | GREEN, 43 s |
| **S1**: `stale_slot = false`, `back = 0` (no age handling — the §4.5.8c sabotage) | 4/4 (the fill reached every slot even without widening) | RED at rounds **2, 1, 2** across three attempts (63 882 stale px) | RED 3/3 |
| **S2**: `back` off by one (`age-2`; exactly one stale slot) | 4/4 | RED at rounds **2, 5, 2** | RED 3/3 |
| restore | 4/4 | 0/368 280 on 8/8 | GREEN, 43 s |

S2's attempt that needed five rounds is the 1,1,2 pattern earning its keep: four
dumps landed on the two good slots before the fifth reached the stale one.

**Building the gate found a defect in C-2d-a, and the gate is why it matters.**
`931bf15a` recorded the **widened** repaint range as the damage history ("the
WIDENED range, not the originally-dirty one: this is what actually reached the
slot"). That is correct and never converges: the union answers "what changed
since slot X was last presented", and what changed between two presents is the
dirty span — the widening only says how much of it THIS slot had to catch up
on. Recording the widened range instead makes one full-rows entry (any scroll)
re-enter every later union, so every present after it repaints all rows
forever: the damage path was dead, and aurora was repainting the whole grid on
every cursor blink since C-2d-a landed. Fixed to record the dirty span; a
full-rows entry now falls out of the window after `nslots` presents. Two
consequences worth stating: (a) S2 is a sabotage only against the fixed
recording — under the widened one an off-by-one was **masked**, since any
`back ≥ 1` propagates the full-rows entry; the old code had slack precisely
because it had no damage path; (b) the tight recording is now guarded by this
gate, which is the order these things should land in.

**What this gate does NOT cover, so nobody reads more into it:** the composed
path (by §4.5.8c's own scoping the screen is the accumulator there — C-3's
property, C-3's gate); the hidden→visible half of the C-2d invariant (a
compositor that skips a hidden surface's transfer and must fan the redraw
`CONFIGURE` on unhide — aurora is never hidden here); and tapestryd's per-slot
refactor as such — the gate proves the client+server pair renders correctly on
the live Direct path, not that every branch of `f86177b6` is right. **The
focused audit round on `usr/tapestryd` (an I-40 trigger surface) is still owed.**

#### 4.5.9 The composed path is CAPABILITY-GATED, and the CPU path is PERMANENT (measured 2026-08-16, before C-2 wrote a line)

**Measured, not inferred.** The primary dev loop reports, in the boot log of the
run that gated the extinction fix:

```
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0
```

`tools/run-vm.sh` defaults `gpu_dev` to **`virtio-gpu-pci`** — a device with no
GL — and `gpu.rs` negotiates `VIRTIO_GPU_F_VIRGL` and records the result. The
`-gl` models refuse to realise without a Linux GL host, which is why they run on
thyla-pi and not on the M2. So **`CTX_CREATE`, `RESOURCE_CREATE_3D` and
`SUBMIT_3D` are unavailable on the default dev loop**, and every mechanism §4.5
describes is unavailable with them.

Three consequences, and the third corrects this document:

1. **C-2 and C-3 cannot be verified on the dev loop.** A compositor context and
   a 3D screen require `virgl=1`, so the arc's functional verification belongs
   on **thyla-pi** (KVM, real V3D, `virtio-gpu-gl-pci` + `egl-headless`). The
   dev loop can still verify that the *fallback* is unregressed — which is the
   larger share of the risk, since that is the path everything else boots on.
   **That host is PROVEN, not assumed** (2026-08-16,
   `WARP_HOST=thyla-pi WARP_ACCEL=kvm tools/warp-host.sh capset`): `tapestryd:
   gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2`, capset id=2
   max_version=2, `CAPSET GATE: VERIFIED`. The two readings — `virgl=0` here,
   `virgl=1` there — are this section's whole argument, one line each.
2. **The composed path must be capability-GATED at runtime**, keyed on the
   negotiated feature bit rather than on a build flag. A tapestryd that assumed
   GL would take the console dark on the default device, which is the
   configuration the whole aurora/console stack boots under.
3. **The CPU-composed path is PERMANENT, and "C-4 retire the readback path"
   must be read as "stop *taking* it where GPU composition is available" — never
   as deleting it.** This is not a preference. It is forced twice over: by the
   plain `virtio-gpu` device that is the default here, and more fundamentally by
   bare metal, where there is no virtio-gpu at all and virgl is a
   *virtualization* transport with nothing to negotiate. The universal path is
   the CPU one; GPU composition is the accelerated path *where a GPU seam
   exists*. §4.4's own framing already said the Direct/Composed split is
   Fuchsia's `DisplayCompositor` pattern "with the fallback's GPU half unbuilt";
   what was missing is that the fallback does not go away when its GPU half is
   built.

**The cost this carries, stated rather than discovered later:** tapestryd will
carry TWO composition paths for the foreseeable life of the system, and they
must stay behaviourally identical from the outside — `ls-gfx*` byte-identical
across both, or the gate that proves one is silent about the other. That is a
standing maintenance and audit burden, and it is the price of running on
hardware that does not all have a GPU seam. It also sharpens §4.5.5's
capset-neutrality obligation: the model must now stay neutral across *three*
fills (CPU, virgl, Venus), not two.

**Follow-through — C-2 verified on that host, both arms (2026-08-16).**
Consequence 1 said where the verification belongs; this records it happening,
and one thing it cost. C-2a/C-2b shipped with the 3D arm having never executed,
because `alloc_screen` runs only under `Scanout::Composed` and **no verb that
boots the GL device ever entered that state**: `capset`, `prove` and `tri` each
drive at most one display-sized surface, which §4.5.1's mode machine resolves to
**Direct**, scanning out the client's own resource and bypassing the screen
entirely. A capability-gated path needs a driver that reaches it, and the
absence of one is invisible in a green boot — that is this section's practical
corollary, and it generalises past graphics: *a gate whose precondition nothing
constructs is indistinguishable from a gate that works.*

`tools/warp/composed-screen.exp` supplies the driver by running
`/bin/tapestry-battery` — a ramfs-native client that brings up TWO surfaces,
which is the cheapest thing `reconcile()` resolves to Composed and needs neither
GL nor the pool, so the only GL object in play is the compositor's own screen.
**The control is the device**, and the two legs disagree on one host with one
variable changed:

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

The second line is not a formality. A GL-only leg would pass identically against
a tapestryd that ignored the negotiated bit and always minted 3D, so the non-GL
leg is what makes the first line mean anything — and a measured *disagreement*
is stronger than two agreeing greens. The gate keeps the two claims separate
(posture matches the device; screen arm matches the posture) so a host that
silently lost its GL cannot satisfy the second by making both sides equally
wrong. `tools/warp-host.sh composed` runs both legs and requires both.

#### 4.5.10 C-2c — the attach is a compositor-side IMPORT, bounded by hosting; there is no client verb (decided 2026-08-17)

**What C-2c is.** P1b (§4.5.4) proved that a `CTX_ATTACH_RESOURCE` of a client's
resource into another context is what PERMITS a cross-context blit — without it
vrend refuses with `Illegal resource`. C-3's composition blits run in
`COMPOSITOR_CTX` and read every visible surface's resource, so before C-3 can
blit, something must attach those resources to the compositor's context. GPU-
DESIGN has called that "the attach verb" and "the I-45 authority-conferral
point" since 2026-08-13 and left two questions to this chunk: **who may request
the attach, and bounded by what?**

**The research collapses the fork, so this is a decision reported, not a vote
requested.** Three shapes were on the table:

1. **A client-facing ctl verb** (`compose-attach <res>` or similar): the client
   asks that its resource be attached to the compositor's context. **Rejected.**
   It hands a client a way to *name* a resource for a context it does not own,
   which is exactly the cross-context naming I-45's guest-exposure half forbids
   (§8: "no cross-context resource naming"); and it is unnecessary — the client
   has already expressed the only consent that matters by *presenting into a
   hosted surface*.
2. **Attach at blit time, per frame** (attach → blit → detach). Same authority
   as (3), strictly worse: an attach/detach round trip per surface per frame on
   the controlq, and a lifetime hazard, since a detach racing a fenced blit that
   still names the resource is the I-45 "buffers live until the last in-flight
   submission retires" clause violated by construction. **Rejected.**
3. **Attach on host, by the compositor, for surfaces it hosts; detach on
   unhost / retire, after quiesce.** The compositor imports the resources of the
   surfaces it composes into its own context, exactly once per resource
   lifetime, and releases them when the surface (or the BO) goes. **Chosen.**

**Prior art, which is unanimous on shape (3).** Wayland compositors import each
`wl_buffer` (an EGLImage / DMA-BUF) into the compositor's own GL/Vulkan context
when the client attaches it to a surface — the import *is* the attach, the
authority *is* the client's `wl_surface.attach` + `commit`, the compositor holds
its reference while it composes and returns the buffer with
`wl_buffer.release`. Fuchsia's Scenic takes a sysmem `BufferCollection` import
token — a capability the client hands over — and imports into its own Vulkan
device; the client never names Scenic's images. Genode's nitpicker composes
from client framebuffer dataspaces handed to it through the session; the client
paints, nitpicker owns the screen. Plan 9's rio draws every window from the
image the client drew into via `/dev/draw`; the client never touches the
screen image. In every one of them **the client's act of handing the buffer to
the compositor is the whole grant, the compositor performs the import into its
own rendering context, and the reverse direction — a client reaching the
compositor's or a peer's buffer — does not exist as a verb.** That is shape
(3), and it is what I-45's guest-exposure half already says in the negative.

**The rule, stated so it can be audited:**

- **Who:** only tapestryd, into `COMPOSITOR_CTX`, and only when `comp_ctx` is
  live (§4.5.9 — on a `virgl=0` device there is no compositor context and no
  attach; the CPU path is untouched).
- **What:** the resources a composition blit can name — for a software surface,
  all `WEAVE_SLOTS` per-slot resources of a generation (§4.5.8: the presented
  slot varies per frame, and attaching once per generation beats attaching per
  present); for a GL-adopted surface (Warp-4 `glsrc` + `present-to`), the
  consented BO's `res_id`.
- **When:** at import — `alloc_weave` right after the per-slot mint, and the
  adoption pairing when it becomes active — never lazily inside the blit path.
  An attach failure is NOT fatal to the surface: it is recorded per resource
  (`comp_attached`), the surface keeps working on the CPU/2D arms, and C-3
  simply does not blit from a resource that is not attached (it falls back to
  the readback for that surface, exactly as a `virgl=0` host does for all).
- **Bounded by:** hosting. A resource is attached to the compositor's context
  iff its surface is one tapestryd hosts; the attach confers nothing to the
  client (its own context's view is unchanged) and nothing to any peer.
- **Revoked:** `release_gen` (a generation dies) and the BO's own death
  (`wbo` destroy / ctx retire) detach BEFORE `resource_unref`, and — once C-3
  exists — only after the last fenced blit naming the resource has retired
  (I-45's in-flight clause; C-1's spec already carries the drain). At C-2c
  there are no blits, so detach-before-unref is the whole ordering; C-3 must
  add the fence wait in the same commit as the first blit, not after.

**What C-2c can be verified to do, exactly — and this paragraph was rewritten
before it was ever true, which is the point of §4.5.4c.** The first draft said
"the host ACCEPTING the attach: a per-generation `comp-attach 3/3` line is the
conjunction of response-checked round trips virglrenderer answered OK — the
same standard the C-2b screen line meets". §4.5.4c falsified that standard the
same hour: `CTX_ATTACH_RESOURCE`'s OK response attests nothing about the
renderer, so an attach-count line would be a gate that cannot fail. **C-2c's
gate is therefore P1b's two arms, in-guest**: after the compositor attaches a
hosted surface's slot resource, blit a small box of it into the screen in
`COMPOSITOR_CTX` and read the screen back (`TRANSFER_FROM_HOST_3D`) — pixels
match with the attach; the no-attach control arm (a build flag or a ctl toggle
that skips the import) must read the renderer's refusal as unchanged pixels,
i.e. RED. Which means **C-2c lands together with the first blit witness rather
than before it**, and C-3 grows from that witness instead of introducing the
blit cold. The device control still applies on top: on `virtio-gpu-pci` there
is no compositor context, no import, and the surface is unaffected — a
GL-only leg would pass against a tapestryd that attached unconditionally and
broke the CPU path.

##### 4.5.10a C-2c AS BUILT — the import at `alloc_weave`, the witness through the compositor's own sentinel (landed 2026-08-17)

**Two refinements to the gate paragraph above, both in the direction of a
witness that touches less.** (1) The readback target is not the screen but a
compositor-owned 1×1 sentinel resource — the compositor context's own #240
mark/sentinel pair (`Comp.comp_probe`, built by `warp_probe_build(COMPOSITOR_
CTX)` the moment the context is minted). Copying slot → sentinel instead of
slot → screen tests exactly the direction C-3 will use (the slot as a copy
*source*), needs no save/restore of screen pixels, and asks no question about
the screen's texel coordinates (a 1×1 target has one texel). (2) The site is
**import time** — the end of `alloc_weave`, for `create` and the resize ack
alike — not composed entry. The stated reason for composed entry was that the
screen may not exist yet at import; with the sentinel as the target that
reason is gone, and import time is where `comp_attached` is decided anyway,
so the witness and the record it justifies are one step. Both hold what the
paragraph above requires: pixels through the compositor context, or nothing.

**Per generation, in order** (`comp_import_slots`, ~16 synchronous controlq
round trips on the compositor's sync slot, so ordered by construction):
`CTX_ATTACH_RESOURCE(COMPOSITOR_CTX, slot_i)` for every slot (a device-level
`Err` detaches the earlier ones and reports `attach failed (device)`); then
the **health copy** — repaint the mark, poison the sentinel with a fresh token,
`RESOURCE_COPY_REGION` mark → sentinel *inside* `COMPOSITOR_CTX`, read the
sentinel back: it holds the mark iff the context executed a command buffer
just now; then per slot the **witness** — seed two DISTINCT tokens into the
slot's guest pixels (0,0) and (0,h−1), `TRANSFER_TO_HOST_2D` each (the present
path's own transfer), zero both guest pixels, poison the sentinel, copy slot
box (0,0,1,1) → sentinel in `COMPOSITOR_CTX`, read the sentinel back, compare
RGB against both tokens. Two rows because a slot is a `Y_0_TOP` resource and
the sentinel is not, and which texel a copy box at y=0 names on such a source
— row 0 through the texel-exact copy-image path, row h−1 through the FBO path,
which measures `Y_0_TOP` boxes from the bottom — is the renderer's to answer;
either token witnesses the import, and WHICH came back is printed (`copy read
texel row R`), so C-3's blit boxes start from a measured convention. The
seeds ride the guest pixels for the transfers only and are legal because **no
client mapping of this weave exists yet** — the Tweft that maps it is
answered after `alloc_weave` returns — so the client maps the zeroed weave it
is owed. The HOST copy keeps the tokens at those two texels, unobservably: in
Composed mode slot host copies are never scanned out, and every Direct-mode
present of a never-presented slot carries full damage (age 0, §4.5.8b), which
overwrites them before the slot is first bound. (The slot resources are
`B8G8R8A8` like the sentinel — `resource_create_2d` mints format 1 — so the
copy is same-format; RGB is compared because alpha is not part of the claim.)
The say line is one per generation — `comp-attach surface N res A..B:
witnessed 3/3 (copy read texel row R)` /
`REFUSED (slot i copy did not land)` / `SKIPPED (compositor ctx unhealthy)` /
`SKIPPED (no witness probe)` / `attach failed (device, slot i)` — and
`comp_attached` is true only on `witnessed`. On a `virgl=0` device the posture
line carries `comp-attach: skipped (no compositor ctx)` once and no per-
surface line ever prints; on a GL device the instrument reports on its OWN
line after the posture anchor — `comp-attach witness armed (probe res M,S)`,
or `UNAVAILABLE (probe build failed)`, in which case every import reads
SKIPPED and nothing becomes a blit source (fail closed). Its own line, and
printed AFTER the anchor, because the first measured run put the probe mint
(device round trips) between "ctx up" and the posture line and the anchor
came out torn byte-wise by the kernel's `proc: orphan` burst at warden's exit
— the console TX ring is byte-atomic, not line-atomic, so any two concurrent
writers tear each other (the `#55b` class; a system defect recorded in the
journal, not fixed here). Census: `comp-attach witnessed W refused R` in the
global warp ctl.

**Why the health copy runs first, and what it measures.** A copy that names
a resource the renderer does not hold in the context reports
`VIRGL_ERROR_CTX_ILLEGAL_RESOURCE`, and vrend then refuses every later
command buffer on that context (§4.5.4a's latch — whose reach the sabotage
below measured further: the screen's 3D mint, attached to the context AFTER
the latch, failed its transfer round trip too, so §4.5.4a's "transfers still
work on a latched context" is established for the probe pair attached before
the latch and does not extend to a resource attached after it). So a
witness that failed for a genuine reason would leave the compositor context
dead for the process lifetime, and every later generation's REFUSED would be
a consequence, not a finding. Running the health copy before the slots makes
the first line attributable to *that* import and every later one read
`SKIPPED (compositor ctx unhealthy)` — the measured state. This is also why
`comp_attached` fails closed and why C-3 must never blit from a resource
without it: the cost of one wrong blit is the whole GPU composition path,
silently, forever. And it is why the witness runs where the compositor can
afford the answer — a rare structural event, ~16 round trips — never per
frame.

**The GL adoption's import.** `present-to <n> <bo>` is the ctx handing its
buffer to the compositor, so it is where the BO is imported: attach → the
health copy → a **change** witness (the BO's host texel (0,0) is the client's
own rendering, unknown to us; poison the sentinel, copy BO (0,0) → sentinel,
read back — a value other than the poison means the copy landed; two rounds
with two distinct poisons make it exact, since the client's texel can equal
at most one). The BO is only read, one texel of it, and its backing is never
touched. Recorded on the BO (`WarpBo.comp_imported`), so every death path
revokes it before the unref: `present-to off`, a replaced consent, the
consented surface's retire (`comp_release_consents_for`), and the BO's own
retire (`wbo_retire`), which detaches from `COMPOSITOR_CTX` before its
`RESOURCE_UNREF`. Say line: `comp-attach ctx C bo B res R -> surface N:
witnessed | REFUSED | SKIPPED | attach failed`.

**Revocation of the slot imports** is `release_gen` (a displaced generation)
and `retire` step (4): `CTX_DETACH_RESOURCE(COMPOSITOR_CTX, slot_i)` before
`DETACH_BACKING` + `RESOURCE_UNREF`, unconditionally under a live compositor
context — a detach of a never-imported resource is a lookup miss at the
renderer, not a context error, so the sabotage-skipped attaches and the
device-refused ones need no separate bookkeeping to be released.

**Gate.** `tools/warp-host.sh composed` (both legs, `composed-screen.exp`)
grew a third claim: GL leg — ≥ 2 per-surface lines (the battery's two
surfaces) all read `witnessed n/n`, none REFUSED/SKIPPED (a witnessed line
implies the instrument was armed; the anchor line carries no second claim);
2D leg — the posture line declares the import skipped
and no per-surface line prints (the control). Verb terms six and seven:
`WARP-COMPOSED ATTACH: witnessed K surfaces` on GL, `skipped (no compositor
ctx)` without. `glq-virgl.exp` (the `quake` verb) gates the ctl census at its
tail — `refused` must be 0 and `witnessed ≥ 1` on the GL device — which
covers the BO import through the SDL shim's real `present-to`. Sabotage that
discriminates: skip the slot attaches in `comp_import_slots` (the path under
test, nothing else) — the health copy stays green, the first slot's copy is
`Illegal resource`, the line reads REFUSED, the scenario fails.

**Measured on thyla-pi (KVM, V3D 4.2), 2026-08-17.** *Clean* (`warp-host.sh
composed`, GL leg): `comp-attach witness armed (probe res 65,66)`; aurora
`surface 0 res 67..69: witnessed 3/3 (copy read texel row 799)`; the battery
`surface 1 res 70..72: witnessed 3/3 (row 799)`; screen `res 73 3D (compositor
ctx)` + `res 73 bound`; aurora's reweave `surface 0 res 74..76: witnessed 3/3
(row 797)` (h = 798 now); across the run every generation import witnessed —
`surface 2 res 77..79 (row 399)`, `80..82 (row 397)`, `surface 0 res 83..85
(row 797)` — **8/8, and the row is always h−1**: on this host the compositor's
`RESOURCE_COPY_REGION` from a `Y_0_TOP` source goes through the FBO copy path
(the box measured from the bottom, `height0 − y − height`), NOT the texel-exact
copy-image path, so a compositor blit box at y names texel row h−1−y. That is
a measured convention C-3's blit boxes inherit. `WARP-COMPOSED ATTACH:
witnessed 2 surfaces (copy read texel rows: 799 797)`, scenario PASS; 2D leg
`composed path = CPU (virgl=0); comp-attach: skipped (no compositor ctx)`, no
per-surface line, `ATTACH: skipped`, PASS; verb `C-2b/C-2c COMPOSED-SCREEN
GATE: VERIFIED`, rc 0 on all seven terms. *Sabotage* (skip the slot attaches
in `comp_import_slots`, nothing else): the first import `surface 0 res
67..69: REFUSED (slot 0 copy did not land)` — the health copy passed, the
unattached slot's copy did not — and then **every later import `SKIPPED
(compositor ctx unhealthy)`** (five of them): the latch, measured rather than
inferred; **and the screen's own 3D mint read `screen res 73 2D (1280x800) --
3D refused: renderer round trip`** — its sentinel round trip through the now-
latched context failed and the §4.5.4c fallback ran for real: the display kept
working on the 2D/CPU arm while the GPU composition path was loudly gone. The
verb's C-2b terms and the C-2c term all RED (rc 1); the 2D leg unaffected. One
variable, two verdicts. **The instrument needed a control of its own first**:
the single-row seed (guest row 0 only) read REFUSED on the CLEAN build — the
copy was landing, from row h−1 — which is why the witness seeds two rows with
distinct tokens and reports which came back; and the gate script cost three
more Pi cycles (a say-line format change under an anchored regexp; three
`-re` arms whose ORDER beat buffer position and discarded the screen/composed
pair; then one ordered pattern that matched PARTIAL lines because it had no
line terminator — three GL-leg hangs ending on the battery's later FAIL while
an offline replay of the same log passed) before the anchored single-pattern
form went green. *`quake`* (the BO import through the SDL shim's real
`present-to`, KVM/V3D): `comp-attach ctx 1 bo 1 res 79 -> surface 1:
witnessed`, then `scanout direct 1 GL res 79`, the timedemo at 44.7 fps, and
the census after the game died `GLQ-VIRGL COMP-ATTACH: witnessed 5 refused 0`
(three console generations + the game's surface + its BO), `WARP-4 GATE:
VERIFIED` — after a first quake run failed CLOSED on a C-2d-b leftover: the
`scanout direct N (WxH)` regexps of `glq-virgl`/`glq-decomp`/`glq-wedge-probe`
(five, three files) had been broken since `f86177b6` renamed the line to
`scanout direct N slot S (WxH)`, the first time any of them ran after it; the
`slot S` token is optional in all five now.

**What C-2c does NOT establish.** Nothing about composed pixels: the screen
is still CPU-filled; C-3 owns the first composition blit and grows from this
witness (`comp_copy_px` is the encoding it will generalize). Nothing about
the in-flight clause: at C-2c there are no fenced blits, so detach-before-
unref is the whole ordering, and C-3 must add the fence wait in the commit
that adds the first blit.

#### 4.5.11 C-3 AS BUILT — composition by blit, on the compositor's sync slot, under measured conventions (landed 2026-08-17)

**What C-3 is.** The Composed present stops filling the screen on the CPU
where the GPU can compose it. On a GL host, for a surface whose current
generation is imported into `COMPOSITOR_CTX` (C-2c, `comp_attached`) and a
3D screen, a present is: TRANSFER_TO_HOST_2D of its damage rects into the
**presented slot's own resource** (per slot since C-2d-b — the same transfer
the direct arm issues, no slot base), then one `VIRGL_CCMD_BLIT` per compose
op slot → screen inside `COMPOSITOR_CTX`, then `RESOURCE_FLUSH` of the union
of destination rects. The screen BUFFER is not touched; the pixels exist only
in the screen resource. A GL adoption's consented BO (Warp-4 `present-to`)
whose import was witnessed composes by ONE blit BO → screen — no readback,
no CPU pass, no upload: per-frame guest↔host pixel traffic for a windowed GL
client is zero, which is §4.5.1's whole point. Everything else — a `virgl=0`
host, an unimported generation, a hidden surface, a latched compositor
context — takes the CPU path exactly as before (§4.5.9: permanent, and
identical from outside).

**Chrome stays CPU-painted, uploaded on damage, on BOTH paths.** The screen
buffer holds the chrome (background, 1-px frames, the D7 strips). A
structural repaint paints it whole and uploads the whole buffer — content
blanks and panes heal through the CONFIGURE fan, exactly the pre-C-3
behaviour — but a focus-only repaint now uploads only the frame and strip
RECTS it painted (`paint_borders`/`paint_strips` return them): on the GPU
path the buffer does not hold client pixels, so the whole-buffer push that
used to serve focus changes would have blanked every pane. On the CPU path
the buffer mirrors the host, so the rect push is the same pixels; one code
path, two identical outcomes. The uploads are TRANSFER_TO_HOST_2D on both
screen kinds — the command names a resource, not a kind, and the 3D screen's
structural repaints have always landed through it; the C-2b special case
that re-uploaded the whole frame per rect on a 3D screen is gone with the
CPU fill it served.

**Why the blits ride the SYNC slot, and what that buys and costs.** §4.5.2
sketched "all blits for a frame in ONE fenced `submit_3d`; on fence
completion `SET_SCANOUT` + `RESOURCE_FLUSH`" — a pipelined compositor. C-3
lands the synchronous refinement of it: `submit_3d_sync` on the compositor
context, each response before the next command, inside the present
dispatch. It buys the I-40 property by construction — the present is still
one dispatch, `ComposeBlit`/`ComposeComplete` close together, the in-flight
blit set is empty at every retire decision point, so detach-before-unref
(C-2c) remains the whole ordering and no drain code exists that could be
wrong; and it costs one controlq round trip per present (~µs on KVM: QEMU
answers when virglrenderer has decoded and issued the blit, it does not wait
for the GPU), against the ~4 MB readback + CPU pass + ~4 MB upload it
replaces for a GL client. What it does NOT give is GL-completion ordering
across contexts: the slot fill runs on vrend's context 0, the blit on the
compositor's, and GL orders those only in practice (single-queue V3D). That
residual is P2 proper — measured 0/500 with the probe proven able to see a
stale read (§4.5.4) and modelled by `buggy_blit_during_fill` — and its
failure is a transient stale frame corrected by the next present, never a
lifetime hazard. Closing it needs a fence the guest can wait on: a
FENCE-flagged command on this same slot would make `.step` wait for GL
completion with no new lane machinery, at the cost of QEMU's fence-poll
latency per present; that, and the fenced pipelined form with a real drain,
are the C-4+ evolution the spec is already cut for. Both were weighed and
neither was needed to make composition correct.

**The screen is minted `Y_0_TOP`, and here is why that was a defect and not
a preference.** C-2b minted the 3D screen flags 0 — the GL-native
convention, row 0 at the bottom, shown UNFLIPPED at `SET_SCANOUT` — and
filled it top-down from the CPU by TRANSFER_TO_HOST_3D. Every 2D resource
QEMU creates carries `Y_0_TOP` and is FLIPPED at scanout; that pairing is
what shows a Linux guest's fbcon upright on `virtio-gpu-gl` under
egl-headless, and the flags-0/unflipped pairing is what shows its Weston
desktop upright. A top-down fill of a flags-0 resource therefore displays
inverted on a GL display — the C-2b composed screen on thyla-pi, from the
day it landed, and nothing could see it: #195 (no host pixel capture on the
GL host) plus a gate that read a say line. C-3's `Y_0_TOP` screen makes the
3D screen's display convention the 2D screen's, whose uprightness the
`ls-gfx` pixel gates measure on every LS-CI run; makes slot → screen blits a
same-convention pair; and makes the CPU-painted chrome reach the 3D screen
exactly as it reaches the 2D one. The display half of that argument is
still an anchor, not a measurement — the flip a GL display applies to a
`Y_0_TOP` scanout is QEMU's code, verified by every Linux guest and by
nothing of ours — and it is named as such below.

**Conventions are MEASURED at bring-up, never assumed (`BlitConv`) — per
source SHAPE and per SIZE CLASS, and the second axis was found by the first
Pi run, not designed in.** The C-2c witness found that on thyla-pi/V3D a
`RESOURCE_COPY_REGION` from a `Y_0_TOP` source names texel row h−1 for a
y=0 box, and that the texel-exact copy-image path of another host would
answer row 0. A blit box is a request in the renderer's coordinates, and
which edge those coordinates count from is the renderer's to answer per
resource kind — and, it turns out, per request shape: virglrenderer routes an
UNSCALED same-format nearest RGBA blit to the texel-exact copy-image path and
a SCALED one to `glBlitFramebuffer`, and the two paths hold opposite
conventions for a `Y_0_TOP` pair whose transfers invert rows (guest row r ↔
GL row h−1−r): copy-image wants the boxes flipped on both sides (`y' = h − y
− box_h`), the blit path applies that flip itself and wants the raw boxes.
The first draft of the probe measured one unscaled request, derived one
convention, and applied it to everything; the battery's panes — A
letterboxed 1280×800 → 638×398, B 640×400 → 636×398, both SCALED — composed
vertically swapped, and the pixel oracle read blue at A's centre on the very
first probe. So `comp_measure_conventions` asks, once, after the compositor
context and its witness probe come up, for each of (slot, BO) × (unscaled,
scaled ×2): three seeded probes — a 1×4 slot kind (`resource_create_2d`, the
`Y_0_TOP` QEMU stamps), a 1×4 BO kind (3D create, flags 0), a 1×16 screen
kind (3D create, `Y_0_TOP`) — and for each request under test a FRESH
throwaway context (`CONV_PROBE_CTX_BASE`+, destroyed after), because a
request the renderer refuses latches the context it ran on and the probe
deliberately tries requests whose acceptance is the question. Per (shape,
class): try the request variants in order — plain positive boxes, a
negative source height, a negative destination height (the gallium flip
idiom Mesa itself sends for a flipped `glBlitFramebuffer`) — with source
rows 0..2 (asymmetric, so a source flip shows as rows {2,3} coming instead
of {0,1}) into destination row 1 raw (a destination flip shows as the run
landing at 16−1−h instead of 1); take the first whose landing has the ORDER
the shape needs (a slot lands straight; a BO, whose GL row H−1 is its visual
top, lands mirrored); read the flips off where it landed and which rows it
carried; then CONFIRM the derived convention with corrected boxes at an
asymmetric offset (source rows 1..4 → destination row 3, ×2 for the scaled
class): exact rows, wanted order, nothing else touched. Every landing is
SAID as a 16-character row map (`blit-conv slot S plain: rows .0011.......…
-> run at row 1 src rows 0..2 straight`; `… confirm (plain sf0 df0): rows
...112233....... -> CONFIRMED`) so one boot log answers what a host does even
where the decode did not anticipate it; anything the decode cannot place or
confirm FAILS CLOSED for that (shape, class) — that class composes the CPU
way — and a host where nothing can be established reports `composed pixels =
CPU (blit conventions not established)`. The compose path picks the class by
the op's box sizes (the renderer's own predicate) and issues the request
through one builder (`blit_request`), so what was measured is what is sent.
BO composition is further restricted to the probe's own format
(`B8G8R8A8`); another format is not something the probe measured — readback
arm. The posture line: `composed pixels = GPU (blit conv: slot U plain sf1
df1, S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0)` on thyla-pi.

**The compositor verifies its own context, per composed frame, and latches
off.** After the first GPU-composed present of a tick the compositor runs
its #240 health copy (mark → sentinel through `COMPOSITOR_CTX`, the C-2c
instrument) — the §4.5.4b cadence, per composed frame and never per present.
A failed copy means vrend has latched the compositor's stream (§4.5.4a):
`comp_gpu_latch` turns GPU composition OFF, sticky, says so, and forces a
structural repaint (chrome + the redraw CONFIGURE fan) so every pane heals
through the CPU path; the present that discovered it is composed the CPU
way in the same dispatch, so no frame is lost to the discovery. Readable as
`composed-gpu-dead` and the `composed gpu G cpu C` census in the tapestry
global ctl.

**`res_stale`, decided explicitly (per §4.5.8c's demand).** A GPU-composed
present leaves the slot's host copy holding exactly what was transferred:
valid in full iff the present's damage covered the surface — the direct
arm's own rule — so `res_stale[slot] = !covers_full` there; a damage-only
present leaves it partial and a later direct switch expands its first
transfer as before. The CPU-composed arm still marks every slot stale (it
transferred nothing).

**HOLD (test-mode).** A held GPU-composed present does its pixel work now —
transfer + blit — and defers only the flush; a held CPU-composed present
defers upload + flush. `Held::Composed` therefore carries two screen-space
regions, `cpu` and `gpu`, released as upload+flush and flush respectively —
uploading the buffer over a GPU-composed region would paint stale bytes over
the blit. Same posture as `Held::Direct`, whose transfers already land in
the (scanned-out) resource before release. A GL display that redraws its
scanout texture on its own refresh could show a held blit before release;
QEMU's egl-headless renders only on flush and no hold gate runs on a GL
display, so this is recorded, not worked around.

**The pixel oracle, since the display cannot be captured.** `probe-screen X
Y` (tapestry global ctl; test-mode, ungated like the other determinism verbs
because the in-guest battery is not the renderer, rate-limited per tick)
makes the compositor read texel (X,Y) of the SCREEN back and say it: on the
3D screen by TRANSFER_FROM_HOST_3D through `COMPOSITOR_CTX` — the only place
a GPU-composed pixel exists — landed at the pixel's own offset in the buffer
(idempotent where the buffer mirrors the host, a don't-care the next
structural repaint rewrites where it does not); on the 2D screen the buffer
is what was transferred. `tapestryd: screen-probe (X,Y) = #rrggbb via
readback|backing [scanout S; composed gpu G cpu C]`. The battery probes its
own sample points at every pixel stage, plus two new legs: `multirect-v` (B
split TOP/BOTTOM green over yellow — the vertical asymmetry a mirrored or
displaced blit box cannot fake, which a solid fill and a left/right split
never show) and `tab-cycled ready` (A hidden by the tab, revealed by the
cycle, presented red, probed — the C-2d redraw contract on the composed
path). `tools/warp-host.sh composed` grew terms eight and nine: 9/9 probes
exact `via readback` with ≥ 1 GPU-composed present on the GL leg (a build
whose GPU path silently routed everything to the CPU one composes CORRECT
pixels, and only the census tells that apart), 9/9 exact `via backing` with
0 on the non-GL leg — the same coordinates and colors on both, the first
pixel witness that the two composition paths agree from outside.

**Measured on thyla-pi (KVM, V3D 4.2), 2026-08-17.** *Run 1* — one
convention (measured unscaled: both boxes flipped) applied to every blit:
`slot confirm: rows [0, T1, T2, T3, 0…]` CONFIRMED on the probe, then the
battery's panes composed vertically swapped — `screen-probe (960,200) =
#0000ff` for A's red, `LS-CI FAIL` on the first probe; the 2D leg of the same
run `9 probes via backing ok`, PASS (the CPU path and the oracle were right;
the GL convention was wrong for the scaled class). *Run 2* — the per-class
probe: `slot U plain: rows .............23. -> run at row 13 src rows 2..4
straight` → `confirm (plain sf1 df1): ...123.......... CONFIRMED`; `slot S
plain: .0011........... -> run at row 1 src rows 0..2 straight` → `confirm
(plain sf0 df0): ...112233....... CONFIRMED`; `bo U plain: .............10.
-> run at row 13 mirrored` → `confirm (plain sf0 df1): ...321.......... CONFIRMED`;
`bo S plain: .0011........... straight` (rejected for a BO), `bo S src-neg:
.1100........... mirrored` → `confirm (src-neg sf0 df0): ...332211.......
CONFIRMED`; posture `composed pixels = GPU (blit conv: slot U plain sf1 df1,
S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0)`; then A red
`(960,200)`, B blue `(960,600)`, multirect `(800,600)` green / `(1119,600)`
yellow, multirect-v `(960,500)` green / `(960,699)` yellow, tab strips
`(800,2) #3a3a44` / `(1120,2) #7a9ecc`, tab-cycled A red `(960,402)` — `9
probes via readback ok (composed gpu 35 cpu 0)`, PASS. *Run 3* (the final
binary, both legs): GL `9 probes via readback ok (composed gpu 34 cpu 0)`, 2D
`9 probes via backing ok (composed gpu 0 cpu 27)`, verb `C-2b/C-2c/C-3
COMPOSED-SCREEN GATE: VERIFIED` (nine terms). *Sabotage S1* (the blit never
submitted, everything else of the GPU path intact): `(960,200) = #101014` —
the pane background — with `composed gpu 10`, RED on the first probe.
*Sabotage S2* (every present routed to the CPU path): all nine pixels exact
`via readback` — the CPU upload into the 3D screen composes right too — but
`composed gpu 0 cpu 31`, RED on the census term, the discriminator a
correct-pixels sabotage needs. Run 1 is the third sabotage, the natural one.
*`quake`* (the standing GL gate): `WARP-4 GATE: VERIFIED`, 969 frames at
44.9 fps, the BO import witnessed, and the BO composed arm's first live
execution (`surface 1 composed via GPU blit (BO res 82 -> screen res 76)` in
the Composed window before the direct switch). *`decomp gl`* (§4.5.1's own
instrument): composed **36.9 fps (26.3 s)** against the **25.4 fps (38.1 s)**
of 2026-08-10 on the same host and demo, the direct arm identical at 44.4 fps
both days — the composed present's cost fell from 16.8 ms to 4.6 ms per
frame, the windowed-GL overhead from 1.75× to 1.20×. What remains in the 4.6
ms (blit + flush round trips, the per-tick health copy, egl-headless's own
display readback) is C-4's to decompose, not to guess.

**What C-3 does NOT establish, stated so nobody over-reads the gate.** The
DISPLAY orientation of the 3D screen — the oracle reads the resource, and
`Y_0_TOP` at scanout is QEMU's flip, anchored on Linux guests, not measured
here (a VNC framebuffer grab on the GL host is the instrument that could;
#195's residue). GL-completion ordering across contexts (P2, measured not
proven; the fenced form is the remedy). The BO composed arm's pixels: no
gate on the Pi drives a GL client into Composed with a known frame — its
mirror convention is measured by the probe on a seeded flags-0 resource,
its live path by `decomp gl` (which enters Composed by zoom toggling) as a
smoke, not a pixel oracle. And the composed path's scaled sampling: GL
nearest and the CPU loop's floor arithmetic can differ by one source texel
at scale boundaries, invisible on the battery's solid fills.

#### 4.5.12 C-4 AS BUILT — the residual decomposed, and the health verify taken off the GPU queue (landed 2026-08-17)

**What C-4 was asked.** §4.5.11 closed with 4.6 ms of composed-present cost
per frame left over the direct arm and the instruction that it was "C-4's
to decompose, not to guess." C-4 built the instrument, measured, and found
that the residual was not what the closing paragraph of §4.5.11 listed
first — the sync round trips — but the health verify, and that the number
itself was mostly a property of the measurement host's display backend.

**The instrument: a present-path cost census.** tapestryd times every
synchronous device step the present path issues, where it issues it, and
every present dispatch whole, attributed to the arm it took (`Cost` in
`server.rs`: `present-direct-gl` / `present-direct-2d` /
`present-composed-bo` / `present-composed-slot` / `present-composed-cpu` /
`present-other`; `xfer`, `blit`, `health` (+ `health-issue`, `health-read`),
`flush`, `flush-direct`, `scanout`, `readback`, `cpu`, `push`), cumulative
since boot as `cost <kind> <n> <sum_us> <max_us>` lines in the tapestry
global ctl. Guest-side wall of a sync step INCLUDES the host's work on it,
because each `.step` waits its response, and it includes whatever sits
ahead of it in the controlq — so a step that drains the GPU shows the drain,
and a step queued behind the client's frame decode shows that wait.
`glq-decomp.exp` snapshots the census before and after each leg and prints
the delta as `GLQ-DECOMP COST-<dev>-<leg>`, beside the fps it already
measured — one run, both numbers.

**The second axis: the display backend is part of the figure.** Under
`-display egl-headless` every `RESOURCE_FLUSH` is a full-frame
`glReadPixels` into QEMU's console surface (`egl_fb_read`); the direct arm's
whole present is that flush, and it measured **17.0 ms** of the 22.5 ms
frame. That is a cost of the instrument — a display nobody looks at,
reading back 4 MB per frame — not of the guest, and it shapes both arms:
whichever step drains the GPU first pays the frame's GPU time, the flush
pays the rest. So `tools/run-vm.sh` grew `THYLACINE_DISPLAY=dbus-gl`
(`-display dbus,p2p=on,gl=on`: the same render-node EGL context, a flush that
updates no listener) and `warp-host.sh decomp` takes `WARP_DISPLAY=dbus-gl`;
the .exp prints `GLQ-DECOMP DISPLAY-gl: <lane>` with the figures. Nothing
can look at that display; it is the lane for the guest's own present costs
and only that. Under it the direct present is **2.7 ms** — and that 2.7 ms
is not the flush's own work but the FIFO wait behind the client's frame
decode already queued when the present arrives (the composed arm's blit
pays the same wait, which is why `blit` reads 1.3–3 ms).

**What the census found (thyla-pi, KVM, V3D 4.2, 2026-08-17; C-3 as landed
at `7296bf07`).** egl-headless: composed 36.9 fps against direct 44.8, the
composed-BO present **20.7 ms = blit 1.44 + health 8.34 + flush 11.12**
(the health copy ran on 1062 of 1093 blitted presents — once per tick at
60 Hz is once per present at 37 fps); direct 17.0 = its flush. dbus-gl:
composed **62.8** against direct **113.2**, the composed present **9.62 ms =
blit 1.63 + health 8.92 + flush 0.12**; direct 2.73. **The health verify
was the residual**: `comp_ctx_health` uploaded the mark and a token into two
1×1 TEXTURES, copied one to the other inside the compositor context, and
read the sentinel back — and on a tiled renderer every texture transfer is
a BLIT JOB (Mesa's v3d prefers blit-based texture transfers), appended to
the one in-order GPU queue behind every frame the client has in flight, and
the readback then WAITS for that job: a `glFinish` over the client's queue,
per tick, which is exactly what the direct arm's `glFlush`-only swap
avoids. On egl-headless it was masked in the total (the flush drained what
the health tick had not); on dbus-gl it was the whole gap.

**Two steps, both measured.** *Deferred read* (issue the copy now, read it
`HEALTH_PERIOD` = 4 ticks later, issue the next only after the read): on
dbus-gl composed went 62.8 → 84.5 fps, but `health-read` still cost ~15 ms
per working call — because the readback of a texture is itself a blit into
a staging buffer, enqueued behind whatever the client has queued at READ
time; deferral changed when the drain happened, not whether. *Buffer pair*
(`warp_hprobe_build`: the health mark/sentinel minted as `PIPE_BUFFER`
resources, `R8_UNORM`, 4 bytes copied by `RESOURCE_COPY_REGION` — buffer
transfers and buffer copies are CPU-side there, no GPU job at any step):
`health-issue` 0.43 ms + `health-read` 0.19 ms per period, i.e. **0.17 ms
per present**, and composed reached **92.8 fps against direct 113.0 — 1.22×,
1.9 ms/frame**, the composed-BO present **3.18 ms** (blit ~2.9 incl. the FIFO
wait the direct flush also pays, flush 0.14, health 0.17) against the direct
present's 2.67 (the final binary, with the issue-step control below: **93.1
vs 112.7 fps**, composed present 3.48, health 0.21 per present, direct
present 2.45 — the same picture). The remaining ~1.4 ms is not in the server: the compose
blit's own GPU time and vrend's `util_blitter` setup on the host thread the
client's decode shares. The texture pair (`comp_probe`) stays, because the
C-2c import witnesses copy slot TEXTURES into its sentinel; the health verify
falls back to it where the buffer pair cannot be minted (correct, and
slower — say line `comp-health verify on the TEXTURE pair`). **The verify
carries its own positive control** (added at the self-audit): the verdict
"the sentinel holds the mark" is satisfied by a token upload that never
reached the host — the previous copy's mark would still be there — so the
issue step reads the sentinel back after poisoning it and requires the token
before it asks for the copy (one more CPU-side round trip per period on the
buffer pair; a second drain on the texture fallback, which is the slow path
anyway); a control that fails is UNKNOWN, and UNKNOWN latches, like every
other errored step.

**What the deferral costs, decided.** The verdict lags a latch by at most
two periods (~130 ms at 60 Hz): a latched compositor context shows stale
composed panes for that long, then the CPU path heals them — freeze-and-
report on a 130 ms clock instead of a 16 ms one. §4.5.4b's "once per
composed frame" was priced for the fenced form, where the verify would ride
a fence wait already paid; on the sync form it cost a drain per frame. The
compositor's context latches only on our own defect or a host reset — never
by a client's hand (contexts are separate) — so the window is a soundness
non-event and a debuggability delay only. Fail-closed is unchanged: an
errored step is a latch. The census over-counts `composed gpu` by at most
2·PERIOD presents around a latch (blits vrend dropped before the read saw
it).

**egl-headless after C-4: 37.5 vs 44.4 fps (final binary 37.6 vs 44.8),
unchanged, and that is the correct result.** The health cost fell to ~0.2
ms per call, and the flush rose from 11.1 to 18.4–18.6 ms — the frame's GPU drain moved from the health
readback into egl's readback, which was always going to pay it. The 4.2 ms
that remain on that lane are the compose blit's issue (1.3) plus the extra
GPU work the display readback then waits for; they belong to the backend.
The figure the arc quotes from now on is the dbus-gl one, and every figure
names its lane.

**What C-4 did NOT do, and why.** The fenced pipelined form (§4.5.11's
"C-4+ evolution": blit on the fenced lane, flush riding fence completion,
a real drain before retire per `DrainedOfBlits`) is not built: the sync
round trips were not what was left. The blit stays on the sync slot; I-40's
by-construction shape (`ComposeBlit`/`ComposeComplete` in one dispatch, the
in-flight set empty at every retire) is untouched, and `drain_skipped`
remains the spec's counterexample for whoever does build it. "Retire the
readback where GL exists" (§4.5.9's reading): on a GL host the readback
fallback (`transfer_from_3d_sync` of the frame) is taken only when the BO
arm cannot be — an unwitnessed import, a format other than the probe's
`B8G8R8A8` (the SDL shim's `OSMESA_BGRA` front buffer IS that format, so the
client population takes the blit), a latched context — and both decomp
legs record zero `readback` and zero `present-composed-cpu` on the GL host.
It is retired in the only sense §4.5.9 permits: not taken where the GPU can
compose. The CPU path is untouched and permanent.

**C-5, the audit (2026-08-17), and one word of §4.5.12 it corrected.** The
owed I-40/I-45 round ran on Fable 5 (see `memory/audit_c5_closed_list.md`):
0 P0 / 0 P1 / 1 P2 / 2 P3, plus one self-audit P3, all fixed. The P2 was
this section's own premise: "the compositor's context latches only on our
own defect or a host reset, never by a client's hand" was FALSE — the C-2c
BO witness copied ANY consented BO's texel into the compositor's B8G8R8A8
texture sentinel, and a BO of another shape (a buffer, another format, a
mip, an array) is a copy the renderer may refuse, which latches the SHARED
context for the process lifetime: a client-reachable, permanent degradation
of every client's composition to the CPU path (bounded — no crash, no leak,
no cross-client pixel — but a lever). Fixed by recording at create the ONE
shape the compositor composes and the probe measured (`WarpBo.composable`:
flags-0 `PIPE_TEXTURE_2D` or `PIPE_TEXTURE_RECT` — the OSMesa frontend's
framebuffer target, i.e. every SDL/OSMesa client's presented BO; the first
cut of the predicate said 2D only and the `quake` gate went red on
`witnessed 4 refused 1` — `B8G8R8A8_UNORM`, one layer, one level,
unsampled) and importing/blitting only that; every other BO takes the
readback arm, where it was going anyway. The premise now reads: the
compositor's context latches only on our own defect or a host reset, and
the only client-supplied objects it ever names are `composable` ones. The
same gate excludes a client BO minted `Y_0_TOP` (which would compose
mirrored under the flags-0 convention — P3). The remaining two P3s: a
`res_stale` flag left stale on the GPU arm's failed-blit return, and a held
CPU-composed region released after a structural repaint painting chrome
bytes over the pane the new layout put under it (dropped at the repaint
now, the `set_mode` rule).

**Lessons, the reusable part.** A measurement can be of the instrument:
the display backend that made the GL host measurable also priced every
frame at ~17 ms, and both arms' figures inherited it — a second lane, not a
finer probe, is what separates the two (the #214 rule again). A texture is a
GPU object on a tiled renderer: touching four bytes of one from the CPU is a
job in the queue behind everything else, so a "tiny" transfer is not tiny —
it is a barrier. And "once per frame" cadences must be priced against the
mechanism they ride: free on a fence already waited for, a full drain on a
sync slot.

**What the C-0d Fable round added to this section (2026-08-17; §4.5.4b's
tail has the findings).** Two of them are C-4's own residue. (1) The lesson
above was applied to the *compositor's* pair and not to the *client's*: the
per-ctx #240 probe stayed a texture pair, so every client `verify` was still
the drain C-4 had just measured — and one client's verify paid for another
client's queue, which no per-ctx admission gate can see. Every ctx's probe
is a buffer pair now, minted before the texture fallback, through the same
helpers as the health pair (F1). (2) The readback fallback this section
"retired in the only sense §4.5.9 permits" is exactly the sync full-frame
readback whose wait is the client's own queue length, on the console's
dispatch thread, and it is taken for every BO the blit arm cannot compose
(F2). Its remedy is the pipelined form the paragraph above declined to
build for the *blit* — the readback needs it first, bounded: **Warp-C C-6,
§4.5.13** (design note, then the impl, then the Pi gates; a follow-up round
on the C-0d fixes + C-6 closes the dirty round). Until it lands the
exposure is stated, not fixed, in `149-warp.md` and at the arm.

#### 4.5.13 C-6 — the readback arm off the console's dispatch, and what a readback costs under QEMU/virgl (designed 2026-08-18; RESERVED, lands at C-6)

**The finding this answers.** C-0d Fable F2 (§4.5.4b's tail): the composed-GL
present's readback fallback issues `TRANSFER_FROM_HOST_3D` of the whole frame
under the client's ctx on the compositor's **synchronous** slot, so the
console's dispatch thread waits for the readback's response — and the
response is written only after the bytes land, i.e. after the frame is
rendered, i.e. after everything the client has queued ahead of it, a length
the client chooses (its fence throttle admits `WARP_CTX_FENCE_MAX` = 8
submits in flight, each a frame of arbitrary GPU cost). `fence_poisoned`
cannot guard it (the poison is produced by `reap_abandoned` on the serve
loop, which is the loop that is blocked), and `submit_and_wait`'s 500 ms
`SUBMIT_DEADLINE_MS` — meant for a DEAD device — can trip on a merely BUSY one
if event-ful stale wakes arrive during the stall, latching `dead` and losing
the GPU for the console's lifetime (the #31 class). The arm runs for every BO
the blit arm cannot compose: not `composable`, an unwitnessed import, a
latched compositor context, no 3D screen.

**What a readback costs, mechanically — read from QEMU v10.0.0
`hw/display/virtio-gpu-virgl.c` and from virglrenderer 1.1.0
`src/vrend_renderer.c` (the exact package thyla-pi runs,
`libvirglrenderer1 1.1.0-2`; the Debian orig tarball, read on the Pi
2026-08-18), the same kind of reading §4.5.4c rests on.** QEMU processes the
controlq **inline, serially, on its main loop** — the file has no render
thread; `virgl_cmd_transfer_from_host_3d` is `VIRTIO_GPU_FILL_CMD` + one
call to `virgl_renderer_transfer_read_iov`, and the tail of
`virtio_gpu_virgl_process_cmd` answers a non-fenced command with
`virtio_gpu_ctrl_response_nodata` immediately and a fenced one only via
`virgl_renderer_create_fence` — the `FLAG_FENCE` bit changes WHEN THE
RESPONSE IS WRITTEN, never when the transfer runs. vrend executes the read
**at decode time, synchronously**, on the resource's own context
(`vrend_renderer_transfer_internal`: `vrend_hw_switch_context(ctx)`, then
`vrend_renderer_transfer_send_iov`): a GL buffer is
`glMapBufferRange(res->target, box->x, box->width, GL_MAP_READ_BIT)` +
`vrend_write_to_iovec`; a texture is `vrend_transfer_send_readpixels`
(`glReadPixels`) with `glGetTexImage` / the read-only path as fallbacks —
and every one of those returns only when the jobs that write the resource
have completed, which on V3D's single in-order hardware queue means every job
queued before them (C-4 measured exactly this: `health-read` ~15 ms on a 1×1
texture behind a client's queue). Three consequences, each load-bearing for
the design:

1. **A readback of a busy resource stalls the DEVICE, not just the caller.**
   For its duration nothing behind it on the controlq is processed — every
   other client's commands, the compositor's own sync steps, and QEMU's
   display refresh (the main loop is blocked in GL). Fencing the readback
   does not shorten that stall by a microsecond; it only frees the *guest*
   thread that issued it.
2. **A sync step queued behind a stalled readback inherits the stall.** So a
   guest that fences its readbacks and then issues an unrelated sync command
   has moved the wait, not removed it — and `submit_and_wait`'s deadline
   commentary ("pending fences ahead of this chain cannot delay its
   retirement, the device writes a non-fenced response at PROCESSING time")
   was written for fenced SUBMITs, whose processing is a decode. A fenced
   READBACK's processing is the GL wait. The claim is false for it.
3. **Any client already holds this lever.** A client's own `transfer_from`
   of its own busy BO (the fenced verb every winsys has) stalls the device
   for its own queue length, and with it the console's next sync step and
   the display. The compositor's arm is the SAME class on the console path
   — F2 found the compositor doing to itself what a client can do to it.
   This is filed as its own item, **F2b**, below.

So the honest goal is not "make the readback free" — under QEMU/virgl a
readback of an in-flight frame is a device stall of that frame's backlog by
construction — but three narrower things: (a) the console's dispatch thread
never blocks on a client-chosen duration; (b) the compositor never latches
`dead` because a device was busy rather than dead; (c) the compositor's OWN
contribution to device stalls is bounded and coalesced, never a queue.

**The three forms weighed.**

- *Bounded synchronous wait* (issue on the sync slot, give up after B ms):
  REJECTED. The command is already in the device's queue; giving up on the
  response leaves a sync chain unretired that `drain()` will attribute
  later — the ring protocol has one sync slot and no notion of an abandoned
  sync chain — and the very next sync step waits behind the stalled
  readback anyway (consequence 2). Bounds the wrong thing.
- *Gate the arm on quiescence* (`fences-in-flight == 0`, else compose from
  stale pages): REJECTED at the Fable close and again here. A single-
  buffered client (every OSMesa/SDL client presents its front buffer)
  rendering at its throttle depth never quiesces; the readback arm would
  compose it once and then never — the §4.5.9 CPU safety net collapsed to
  a still image for exactly the clients it exists for. Deferring "until
  in-flight ≤ 1" fails the same way for any client faster than the
  compositor's tick.
- **CHOSEN: the fenced readback with DEFERRED present completion, one in
  flight, latest-wins.** The pipelined form §4.5.11/§4.5.12 already cut for
  in `tapestry_present.tla` (`ComposeBlit` → `[inblit]` → `ComposeComplete`,
  `DrainedOfBlits` on retire), applied to the readback arm first because it
  is the arm that blocks.

**As designed.**

- **Issue.** The composed-GL present's `!done` arm issues `TRANSFER_FROM_
  HOST_3D` of the frame on the FENCED lane (`gpu.transfer_3d(to_host =
  false, ...)`, the client's `dev_ctx` in the header — the resource is
  attached there), tagged **compositor-owned** (`FenceTag.ctx_pub = 0`,
  the id `wctx_mint` never mints), records a per-surface pending readback
  `{fence_id, ctx_pub, bo_pub, res_id, va, w, h, gen}`, replies to the
  tpresent, and returns. The dispatch never waits. `Cost::Readback` now
  times the ISSUE; a new `Cost::ReadbackWait` accumulates issue-to-retire
  wall per completed readback, so the census can see the stall the device
  paid without the console having paid it.
- **Complete.** `warp_service_fences` routes compositor-owned completions
  to `comp_readback_retired(tag)`: re-validate the surface (alive, same
  `gen`, still hosted/visible, still adopted to the same ctx/BO), then
  `blit_composed_pixels(n, …, Some(va))` + `screen_push` exactly as today.
  A surface that moved on since (retired, resized, re-adopted) drops the
  frame — a stale composition is worse than none. `composed_cpu` counts at
  completion.
- **One in flight per surface, latest wins.** A present arriving while the
  surface's readback is pending sets `rb_again` and returns; on completion,
  `rb_again` issues one fresh readback of whatever the BO holds now. So the
  compositor never queues readbacks behind each other and a client's present
  rate cannot pile device stalls: at most one compositor readback per
  surface is ever outstanding, and the FRAME it reads is always the newest.
- **One reserved fenced slot.** The lane keeps `FENCED_SLOTS` = 16 with the
  per-ctx share `WARP_CTX_FENCE_MAX` = 8; two clients at their share fill
  it, so the compositor's readback must not compete on the pool. One slot
  is reserved for compositor-owned chains (clients see 15 in
  `alloc_fenced_slot`; the shares are unchanged). If it is busy (another
  surface's readback in flight), the present marks the surface `rb_wanted`
  and the completion of the busy one issues the next in FIFO order — a
  compositor-wide bound of ONE readback in flight, which loses nothing
  against a device that would execute them serially anyway.
- **Retire safety (I-7 / I-40).** The DMA target is the client's BO backing,
  so the BO and its ctx must not free while the readback is in flight. The
  readback is counted in the owning ctx's `fences_in_flight` (the counter
  every quiesce predicate — `wctx_retire`, `warp_pump_retires`,
  `wbo_destroy`'s leak posture — already reads) and additionally in a new
  `WarpCtx.comp_rb_in_flight`, which the ADMISSION gate subtracts
  (`warp_fenced_admit`: `fences_in_flight - comp_rb_in_flight >= share`),
  so the client's share is not shortened by our readback and its
  `fence_signaled` ledger (#210) never sees a fence it did not issue. A
  compositor readback that never retires is abandoned by the same reaper at
  `FENCE_ABANDON_MS`, poisons the client's ctx exactly like a client fence
  would (the device may still write that backing — the leak posture is the
  right one), and the pending record is dropped. `verify` while our
  readback is in flight answers `E_AGAIN` (the ctx has device work
  outstanding on its resources — true), which is the one client-visible
  change and is documented.
- **The deadline is made honest (consequence 2).** While ANY readback —
  compositor-owned or a client's `transfer_from` — is in flight, the sync
  slot's stale-wake deadline is `FENCE_ABANDON_MS` (30 s), not
  `SUBMIT_DEADLINE_MS` (500 ms): a device stalled behind a legitimate
  readback is busy, and a false `dead` latch is the #31 loss the deadline
  exists to prevent, while 30 s is the same bound every fenced chain
  already carries. `Controlq` learns which in-flight fenced slots are
  readbacks (a bit set at `transfer_3d(to_host = false)`); the wait loop
  reads it. This half is F2b's guest-side mitigation and lands with C-6.
- **Spec — LANDED FIRST (2026-08-18, before any C-6 code).**
  `tapestry_present.tla` carries the readback shape behind the same
  `ALLOW_COMPOSE` switch: `ComposeReadbackIssue(g)` (a fenced device WRITE
  into the client BO's pages: `inread[g]`, at most one per generation) →
  `ComposeReadbackComplete(g)` (the fence retires; the CPU compose reads
  those pages), `NoTornReadback` (`InRead(g) => backed[g]`, the graver twin
  of `NoTornCompose`: a device WRITING freed pages), and
  `DrainedOfReadbacks(g)` on `ServerRelease` + `Free`, omitted under
  `BUGGY_READBACK_FREE` (`tapestry_present_buggy_readback_free.cfg`: TLC
  violates `NoTornReadback` in 11 states — … `ClunkMap` →
  `ComposeReadbackIssue` → `Destroy` → `ServerRelease` → `Free`). Additive
  by measurement, as C-1 was: with the switch off the six direct-path cfgs
  reproduce **5413** distinct states exactly; the composed clean cfgs grow to
  94680 and stay green with liveness. `check-tapestry.sh`: ALL 12 CFGS AS
  CLAIMED. No `FillLanded` guard on Issue (the device serializes the read —
  verified in vrend 1.1.0) and no `attached` (the client's own ctx), both
  deliberate and recorded in the module header.
- **Gates.** `warp-host.sh decomp` and `quake` on thyla-pi (the blit arm
  must be untouched: `composed gpu ≥ 1`, `readback` still 0 there); a
  NEW `warp-prove` leg on the Pi that forces the readback arm (a
  non-`composable` BO — a `Y_0_TOP`-minted 2D — presented through a GL
  adoption while the same ctx keeps 8 heavy submits in flight) and asserts
  (i) the console's OTHER surface keeps presenting inside its budget
  (`present-other` latency, the compositor's dispatch not blocked), (ii)
  `Cost::ReadbackWait` records the stall the device paid, (iii) exactly one
  compositor readback in flight over the leg. `check-tapestry.sh` for the
  spec.

**F2b — filed, not fixed here: the QEMU/virgl serial-processing stall is a
client-triggerable lever on the whole box.** A client's own `transfer_from`
of its own busy resource stalls QEMU's main loop for its queue's GPU time —
every other client's device work, the compositor's sync steps and the
display refresh with it — and it can repeat that indefinitely. Nothing in
the guest can shorten a stall the host executes synchronously; the guest can
only (1) not add to it (this section), (2) not mistake it for death (the
deadline half above), and (3) MEASURE it: a `warp-prove` leg that has client
A read back its busy BO while surface B presents, reporting B's present
latency — owed with C-6's gate. Where it stops being ours to accept: Venus
(§12, Warp-6) moves transfers to VkCommandBuffer copies the client waits on
its own fences for, and v3d-native (Warp-7) puts the queue in our hands.
Until then it sits with the host-side exposures §9.2 documents as TRUSTED —
recorded HERE because "trusted host" must not read as "no client can reach
it".

**What this does NOT change.** The blit arm (the composed path every
composable client takes on the GL host) stays on the sync slot exactly as
§4.5.11/§4.5.12 left it — its SUBMIT_3D response is written at decode time,
so it never inherits a stall of its own making; the flush after it is the
display backend's cost. The 2D software-surface path is untouched. The
readback arm's PIXELS are unchanged (same transfer, same compose); only WHEN
the console waits changes: never inside the present that issues it.

**AS-BUILT (C-6b, 2026-08-18; `usr/tapestryd/src/{server,gpu}.rs`,
`usr/warp-prove` `readback`, `tools/warp/warp-readback.exp`,
`tools/warp-host.sh readback`).** Three refinements the design's letter did
not have, each found by starting the code and each recorded here so the
next reader audits the tree, not the note:

1. **The tag carries the CLIENT's `ctx_pub` plus explicit `readback` and
   `comp` bits — not `ctx_pub = 0`.** The driver's abandonment bookkeeping
   keys on the tag's ctx (`fslot_poison_ctx`, `FenceVindication.ctx_pub`,
   `ctx_has_poisoned_slot`), and 0 is `warp_ctx_vindicate`'s no-slot
   sentinel: a compositor readback abandoned under ctx 0 and later retired
   would push a vindication for ctx 0, which `position(p == 0)` matches to
   an arbitrary un-condemned slot → `ctx_destroy` of a live host context.
   And the client's OWN vindication must wait for our abandoned readback of
   its BO (round-4 F1: one late retire proves nothing about the rest), which
   only holds if the slot is attributed to the client. So the marker is a
   bit; the attribution is the client's; the pump routes on the bit and
   still poisons / decrements the right ctx. Nothing else in the design
   moves.
2. **One in flight compositor-wide, not merely per surface.** The reserved
   slot IS the bound (`COMP_FSLOT` = FENCED_SLOTS − 1; clients allocate
   first-fit over the other 15, `lane_exhausted` and `fenced-free` read the
   client pool), so `Comp.comp_rb` is a single record and `rb_wanted` is one
   FIFO of surface incarnations (gen-pinned; no duplicates; latest wins at
   issue time). Every event that can free the slot — a completion, a
   vindication — ends in `comp_rb_pump`. A poisoned reserved slot parks
   every readback-arm surface on stale frames until the late retire; the
   blit arm and the 2D paths do not notice.
3. **What (a) means under QEMU/virgl, exactly.** The dispatch never waits
   *inside the present that issues the readback*, and the per-present
   multiplication is gone (one in flight, coalesced). But consequence 2 is
   not repealed by fencing: while the readback executes, QEMU's main loop is
   inside GL, so the NEXT sync step the console issues — any surface's
   transfer/flush, a health read, a chrome push — inherits the remaining
   stall, and the serve loop is blocked in that step for its duration. What
   the console keeps during the stall is everything that needs no device
   step: the adopting client's own presents (coalesced), every ctl read and
   write, input, events. That is why the deadline half is not optional and
   why the gate below measures the adopting surface's presents and ctl
   reads, not a bystander's presents — those are F2b's number.

The gate as built (`warp-prove readback`, self-gating: `C6-READBACK DONE`
iff every verdict arm passed, else `INCOMPLETE(<arm>)`; the host verb
requires the four PASS terms + the scenario's own pass line): a hosted
tapestry surface A adopts a `Y_0_TOP`-minted (non-`composable`) 512×512 BO;
**ARM** — a present on an idle queue ISSUES a compositor readback and it
LANDS (`comp-rb`; `composed gpu` moving instead is `INCOMPLETE(bo-composable)`);
**DEEP** — with 8 heavy submits queued (80 clear PAIRS each: the BO to
an index-encoded colour, then a 2× scratch partner, alternating
framebuffers so mesa v3d cannot fold them — every clear a full-surface
store on the CLIENT's GL context) the round's MEAN `readback-wait` ≥ 100 ms
(which entails `max` ≥ 100 ms, so at least one readback in the round paid it;
the gate cannot claim EVERY readback did, and since the F8 correction it no
longer tries -- a per-round max is not derivable, because `Cost.max_ns` is a
global running maximum that is never reset) — the
positive control that the queue was deep, without which LIVE is satisfied
by a light one; the leg also prints the queue's own fence timeline and
**which clear index the readback observed** (the BLUE byte of the pixel it
landed), so "the readback waited for the queue" is a pixel, not a
duration; **LIVE** — while that readback is in flight, A's own presents
(every 5 ms, coalesced) and warp ctl reads are each answered inside 50 ms
(under the pre-C-6 arm the FIRST present takes the whole wait); **DEADLINE**
— 7 heavy submits + the CLIENT's own fenced `transfer_from` of its busy BO,
then 10 presents of a bystander surface B queued behind that stall: every
one SUCCEEDS and the engine is alive after (busy read as busy — the 500 ms
deadline would latch `dead` on it, where stale wakes arrive during the
stall); **F2B** — B's present latency behind that stall, max / mean,
REPORTED (the number Venus / v3d remove and no guest change can); **CLEAN**
— `poisoned 0`, `rb-slot` not poisoned after teardown.

*Why the load is clears and not blits — two Pi runs' worth of finding,
recorded because it changes what "a deep queue" means on this host.* Run 1
queued 800 1:1 NEAREST full-frame blits (ping-pong BO ↔ scratch): they
"retired" in 16 ms — `vrend_renderer_blit` takes the `glCopyImageSubData`
shortcut for a 1:1 same-format RGBA NEAREST blit (1.1.0), and 1.6 GB of
copies cannot finish in 16 ms on a Pi: not GPU work the readback waits on.
Run 2 made them SCALED (512² ↔ 1024²): 8 submits retired in 1335 ms — real
work — yet the compositor readback of the same BO waited **84 ms** and the
client's own readback stalled the bystander by ≤ 149 ms. A scaled blit goes
through `vrend_renderer_blit_int` → the BLITTER, which owns its **own GL
context** (`vrend_blitter.c`); the client-context fences and a
client-context `glReadPixels` are not ordered behind another context's
work, so the queue was deep and the readback did not wait for it. The DEEP
control (readback-wait ≥ 100 ms) failed both runs exactly as it should —
LIVE would have passed on a light queue both times. A real client's draws
land on its own context; so do these clears.

**Measured on thyla-pi (KVM, V3D 4.2, virglrenderer 1.1.0, egl-headless
lane), the final artifact — `WARP-C C-6 GATE: VERIFIED`.** ARM: idle-queue
present → `comp-rb issued 0→1 landed 0→1`. DEEP/LIVE, three constructed
rounds of 4 submits × 300 draws × 6 full-screen triangles (1200 draws,
~1.06–1.08 s of V3D per queue; the four 24 KiB Twrites take 15–224 ms to
send): the issuing present **0 / 0 / 0 ms**; the compositor readback waited
**805 / 1005 / 1005 ms** and observed draw **1199 of 1200** every round (the
BLUE byte of the pixel it landed); the fences retired as a burst at
1053–1076 ms. FLIGHT REPORT (data): the adopting surface's *later* presents
during the flight max **1012 ms** — every round's census showed
`slot-presents +1`: the console renderer's cursor-blink present landed in
the window, and on egl-headless its `RESOURCE_FLUSH` is the display
backend's `glReadPixels` of the screen, queued behind the compositor's blit,
behind the client's draws on V3D's one FIFO — so the single-threaded loop
waited there for everyone (consequence 2 with the C-4 flush-readback lane
cost on top; the same mechanism made two earlier rounds "unconstructed":
the SENDS landed behind that flush and the readback met a drained queue —
the leg now detects and retries those, never judges them). Warp ctl reads
in the same windows max 20 ms. DEADLINE: 4 submits + the client's own
`transfer_from` (~1.19 s to retire), 10 bystander presents behind it all
succeeded, engine alive, `poisoned 0`. F2B REPORT: the bystander's present
latency behind the client's readback max **267 ms** (run 7: 1034 ms) mean
39 ms — the number Venus / v3d remove. Sabotage S1 (the issuing present made
to WAIT for the readback — the pre-C-6 arm): DEEP PASS, **LIVE FAIL** with
the issuing present at 269 / 969 / 1017 ms → `INCOMPLETE(live)`, gate FAIL —
the arm discriminates exactly the defect. The blit arm untouched: `quake`
44.2 fps with `comp-rb issued 0`; `decomp gl` composed gpu 1106 cpu 0 with
`readback 0` and `readback-wait 0`.

**The deadline half is NOT discriminated by this gate, and the honest reading
is stronger than "untested" (round F7 [P2] / main#253).** The certifying run
measured `F2B max 267 ms` against `SUBMIT_DEADLINE_MS = 500`, so **a build with
the widening deleted would have produced an identical PASS**: no
`submit_and_wait` accumulated 500 ms of stale-wake time, so nothing could have
latched `dead` either way. A control that cannot fail proves nothing, and this
one was recorded as "a sabotage not run" when it is really "the arm never
reached the threshold it would discriminate at".

Worse for the mechanism's own premise: the deadline is evaluated **only at a
stale wake**, and the stall it exists for — a synchronous host
`TRANSFER_FROM_HOST_3D` on QEMU's serial main loop — writes no used entries
and therefore raises no interrupts. So whether the widening is load-bearing
**at all** depends on INTx sharing with the other virtio-pci functions, which
has never been measured or stated. The widening is still the right shape (a
busy device must not be latched dead), but "correct by construction" is doing
work here that a measurement should do.

What closes it, in order: (a) deepen `RB_DEADLINE_SUBMITS` until the bystander
latency exceeds `2 * SUBMIT_DEADLINE_MS`, so the threshold is genuinely
crossed; (b) run the widening sabotage (pin `deadline_ms = SUBMIT_DEADLINE_MS`)
and require the gate to go RED; (c) state here what raises the stale wakes
during the stall — and if nothing does on this host, say that the widening is
INERT here and name the lane where it is not.

### 4.5.14 The owed round on C-6b (2026-08-18, OPUS fallback): 1 P0 / 3 P1 / 3 P2 / 3 P3

The C-0d Fable close was dirty, so a follow-up was owed on its fixes plus C-6b.
Fable was out of credits (the spawn died mid-run), so it ran on Opus 5 — and
family diversity is **inverted, not forfeited** here: C-6b was Fable-authored,
so an Opus prosecutor is genuinely cross-lineage against it. Full findings +
dispositions: `memory/audit_c6b_opus_closed_list.md`.

**The lesson, and it is about AS-BUILT 1 specifically.** The deviation was
recorded, argued, and correct for the reason it was taken — the tag must carry
the CLIENT's `ctx_pub` because 0 is the vindication sentinel and the client's
own vindication must wait for our poisoned slot. What was never enumerated is
its **cost**: *every* mechanism keyed on a tag's ctx now reaches the
compositor's reserved slot. Two of them are shipped, client-drivable levers
(`warp-hold` / `warp-abandon`) whose safety argument (#178: "the worst a client
can do is wedge its own ctx") was written when "your ctx's fences" meant only
your own. **A deviation is sound for the reason it was taken and dangerous
everywhere else that reads the same field.** Prosecuting a documented deviation
as a design change — not as a footnote — is what found it.

Three findings are corrections to claims this document or the code asserts:
the composed readback's "the PIXELS are unchanged — same transfer, same
compose" (F2: the fenced form dropped the synchronous arm's `.is_ok()` gate and
composed on an ERROR retire), "`fence-signaled` never counts it" (F3: true on
the completion arm, false on the vindication arm), and `rb_wanted`'s "bounded
by MAX_SURFACES" (F6: the dedup key included a monotonic `gen`). And one — F1
[P0] — is **pre-existing** from the Warp-4 synchronous arm and in none of the
five preambles: a client-declared BO backing has no lower bound against its
declared geometry, so a 512x512 BO declared with 4096 bytes made the compositor
read 1 MiB out of a 4 KiB mapping. Attribution is not ownership; it is fixed
here, at the read gate (`gl_adoption`, the exact bound) and at the door
(`wbo_create`, keyed on the one format whose bytes-per-texel can be asserted —
a general floor would reject legitimate compressed textures).

## 5. Placement — where the server lives, per backend

The seam is identical on both; the process topology is not, and both are forced:

**QEMU: tapestryd hosts the GPU service.** One virtio-gpu function carries both
scanout and 3D, and warden binding is one exclusive claimant per function.
Splitting them would mean either two owners of one device (impossible) or a
present hop through a second process (a regression against the zero-copy
property the Tapestry arc exists to demonstrate). tapestryd already owns the
device, the scanout, and the present protocol.

**RPi: a separate `v3d` leaf driver process.** The 3D core (`brcm,2712-v3d`, hub
at `0x10_0200_0000`) and the display pipeline (`brcm,bcm2712-vc6` — HVS,
pixelvalves, HDMI) are *different devices with different registers and different
interrupts*. Two warden-bound drivers, each with its own narrow I-34 allowance,
is the MENAGERIE model working as designed. The compositor becomes a *client* of
the GPU service rather than its host — which is exactly Fuchsia's arrangement
(Scenic is an ordinary Magma client) and Genode's (the GPU multiplexer owns the
platform resources and re-exports a narrowed platform service to the display
driver).

**The seam does not change across that move.** That is the charter's bar, and it
is why the service is defined as a tree in a namespace rather than as
"tapestryd's internal module": on the Pi, the tree is served by a different
process, mounted at the same path, and clients cannot tell.

---

## 6. Kernel deltas

Exactly two, both extensions of existing, audited mechanisms. Neither is a new
subsystem.

### 6.1 A GPU-buffer Burrow subtype

The weave subtype (`SYS_DMA_CREATE_WEAVE`, `KObj_DMA.weave`) exists because a
client must be able to map a buffer the *device* also touches, without that
mapping conveying hardware authority. Its safety argument was: *the device only
DMA-**reads** pixels from it.*

**A GPU buffer breaks that argument** — a render target is device-*written*. So
the subtype needs a distinct kind with its own argument, and the argument is
§2.1's: the buffer is reachable by the GPU only through the GPU's own
translation, and what the GPU may reach is bounded by the context's address
space, which the trusted server programs. The client's cacheable RW mapping still
conveys zero hardware authority.

Realization: a new kernel-minted kind alongside `weave`, admitted by the same
`SYS_WEFT_SHARE` path, budgeted by the same `Proc.shared_map_pages` axis, revoked
by the same reaper. **ABI addition — user signoff required** (the G-2 precedent:
the weave subtype was signed off before it was built).

### 6.2 Host-visible memory mapping (Venus only)

Venus's `HOST_VISIBLE` memory is a subrange of a **PCI BAR** mapped into the
client's address space — not RAM. The kernel must: discover the shared-memory
capability (`cfg_type = 8`, shmid 1), let the device owner allocate offsets
within it, map a subrange into a client VA at the cache attribute the host
dictates (`CACHED` / `UNCACHED` / `WC` — honored *exactly*; on ARM64 the stage-2
attribute forces the weaker of the pair, and the configurations that work in the
field are the ones where guest and host agree), and unmap before the device-side
`UNMAP_BLOB`.

This is the same *shape* as §6.1 — an owner-minted, client-mappable, revocable,
budgeted object — with MMIO pages behind it instead of RAM. **The seam carries the
shape now; the implementation lands in the Venus chunk.** Also required and
currently broken: `PciDev::claim`'s eager map-every-BAR policy (§3).

---

## 7. What we are NOT building

Recorded so the arc does not drift into them:

- **No command-stream validation.** §2.1. It is not practical, both reference
  systems refuse it, and virglrenderer's own history shows what maintaining a
  hand-written validator over a large IR costs.
- **No DRM ioctl emulation.** Reusing Mesa's `virgl_drm_winsys` unmodified would
  mean emulating GEM handles, FLINK global names, PRIME dma-buf export/import,
  `drmGetVersion` (which hard-rejects `version_major != 0`), and the DRI loader's
  device enumeration — a strictly *larger* surface than writing the winsys, most
  of it solving a cross-process buffer-sharing problem our capability model
  already solves differently. Write the winsys (~1–1.5 kLOC).
- **No gfxstream.** It would preserve the virtio transport work and discard all
  the Mesa work, with no continuity from our Gallium/OSMesa stack.
- **No DRM native contexts.** They forward a *specific host GPU's* uAPI, so the
  guest must match the host's hardware — a non-starter for a portable OS, and
  QEMU refuses them without async fencing anyway.
- **No blob resources in the first chunk.** Skipping them costs exactly one
  thing: `ARB_buffer_storage` persistent-coherent mapping, which caps us at **GL
  4.3 instead of 4.6**. Everything else works. Blobs arrive with Venus, which
  requires them.

---

## 8. I-45 (proposed) — GPU authority is bounded by the context

> **I-45. GPU work reaches only what its context owns.** A submission executes
> only against buffers attached to the submitting context, bounded by
> address-translation hardware the trusted server programs — never by inspection
> of the command stream. A context's buffers live until the last client unmap
> *and* the last in-flight submission naming them retires (the I-7/#847 dual
> count extended across the device-side reference). Context teardown — including
> client death — quiesces that context's work without disturbing other contexts'
> results, and releases its GPU address space only after quiesce. A fault
> attributable to a context is fatal to that context and to nothing else.

**Enforcement is per-backend, and the honest statement differs by backend** —
this is deliberate, and follows the I-20/I-40 staged-enumeration precedent
(RESERVED → ENFORCED per half):

- **virgl / Venus (QEMU):** enforced host-side by virglrenderer's per-context
  object tables and, for Venus, an isolated per-context render-server process. We
  bound the *guest* exposure (one context per client, no cross-context resource
  naming, submit-time capability pin) and **document that the host is trusted** —
  see §9.2. Reserved-not-enforced on the host axis, stated plainly rather than
  claimed.
- **v3d (RPi):** this is where the invariant becomes ours to keep, and where the
  design makes a choice Linux did not (§2.5). See fork **F3**.

Composes I-1 (per-Proc namespace), I-5 (hardware handles non-transferable), I-7
(#847 dual count), I-12 (W^X — GPU command buffers are never CPU-executable;
note that hardware GL needs **no `CAP_JIT`**, since shaders compile to GPU ISA,
so I-42 is not in this path at all), I-32 (the buffer budget axis), I-34 (the
driver's own allowance), I-37 (the Weft share discipline), I-40 (the weave
lifetime rules this extends).

---

## 9. Two risks that must be named, not buried

### 9.1 The dev loop has no local host

§2.6: virgl cannot run on this Mac without a patched QEMU, a patched libepoxy, an
ANGLE EGL implementation, and a QEMU source patch adding a macOS branch to
`egl_init()` that does not exist upstream. Venus additionally requires a Linux
host outright.

Everything up to and including "the guest driver is written and the winsys
compiles" can be done locally. **Nothing can be *run* locally.** This must be
settled before code starts, not discovered after — hence fork **F1**.

### 9.2 virglrenderer's OpenGL path is not hardened against a hostile guest

Stated explicitly because silence would be a claim of its own:

The GL path runs **in-process inside QEMU, unsandboxed** — the render server is
Venus-only by construction (virglrenderer's `meson.build` only builds it
`with_venus`, and the server refuses to initialize without `NO_VIRGL`). It parses
~60 kLOC of hand-written guest-facing C, including a **TGSI text parser** on
attacker-controlled strings, then hands generated GLSL to the host's GL compiler.
There is no `SECURITY.md`, no threat model, no OSS-Fuzz coverage (verified: not a
project, and neither the QEMU nor crosvm OSS-Fuzz projects cover it
transitively). The in-tree CVE regression suite has **not executed since 2019** —
`tests/meson.build` registers the wrong target through a stale loop variable.
Blackhat Asia 2020 demonstrated full guest-to-host RCE; a VM-escape-class CVE
(2025-2509) reached shipping ChromeOS. Red Hat ships QEMU with
`--disable-virglrenderer` and states the flaws do not affect RHEL because it is
not shipped.

**For our posture this is acceptable and should be said so:** we are the guest
*and* we control the host, on a development machine. virgl's failure mode is
"hostile guest escapes to host," which is not our threat model. It becomes
load-bearing the moment Thylacine is shipped as an untrusted guest, or runs
untrusted guest code with GL access — at which point this decision must be
revisited. Recorded here so that revisit is possible.

---

## 10. The forks (the user's vote)

### F1 — the dev-loop / CI substrate **[RESOLVED 2026-08-07 — see below]**

> **RESOLVED by measurement, not by vote.** The user proposed a local Parallels
> Linux VM; it was stood up and `tools/gl-host-probe.sh` **PASSES at rung 6** —
> QEMU 10.0.11 realises `-device virtio-gpu-gl` on `-display egl-headless`,
> Debian 13 trixie ARM64. This is option (e), better than every option below:
> it reaches Venus (a Linux host, which macOS structurally cannot be), it has a
> **visible screen**, it costs nothing recurring, and the host GL turns out to be
> **`virgl (Apple M2 (Compat))`** — Parallels' own 3D acceleration is virgl-based,
> so there is a **real GPU at the bottom** rather than llvmpipe. Setup detail +
> the two non-obvious blockers: `docs/GPU-HOST-SETUP.md` §4.1. The GCP leg below
> stays recorded as the fallback if the VM is ever unavailable.

<details><summary>The original option set (superseded)</summary>

| | Option | Cost | Gets us |
|---|---|---|---|
| **a** | **Linux CI leg on GCP** (recommended) | a `thyla-keep` cycle to stand up; no local `run-vm` GL | virgl **and** Venus, matching Mesa's own CI shape; TCG guest, host llvmpipe/lavapipe |
| b | Patched QEMU + ANGLE locally | days of unverified yak-shaving; upstream has no macOS branch | local `run-vm` GL for virgl only; Venus still impossible |
| c | crosvm instead of QEMU | new host VMM to learn; our harness is QEMU-shaped | exactly Mesa's CI configuration |
| d | (a) now, (b) opportunistically later | — | unblocks immediately, keeps the local loop as a nice-to-have |

Recommendation was **(a)**, with (d)'s posture — superseded by the measured
result above.

</details>

### F2 — blob/hostmem scope in the first chunk

| | Option | Consequence |
|---|---|---|
| **a** | **Skip blobs; GL 4.3** (recommended) | smallest first light; no BAR-mapping kernel work; loses only `ARB_buffer_storage` |
| b | Blobs in chunk 1 | GL 4.6 + Venus-ready sooner; pulls the §6.2 kernel delta and the `BarTooLarge` fix forward |

**DECIDED: (a).** Blobs are a tier, not an entry requirement, and (b)
front-loads the one kernel delta whose shape we most want informed by a working
GL path. The `PciError::BarTooLarge` bug gets **fixed** in chunk 1 regardless —
it is a live defect (§3), enqueued as its own task.

**RE-AFFIRMED for Warp-C (user vote 2026-08-13).** GPU composition (§4.5)
re-posed this fork, because a capset-neutral (blob/dmabuf) sharing substrate is
what would let ONE compositor mechanism span virgl and Venus — a virgl context
cannot blit a Venus resource (§4.5.5). The vote is again **virgl-first, blobs
with Venus at Warp-6**: Venus is a genuine SEAM and not a dependency of the
composition chunk (composition works fully without it), so the
chunk-completeness rule does not force it; and **#166 means hostmem is inert
under HVF `-cpu host` on the M2 dev host**, so a blob-first Warp-C could not be
exercised on the primary dev loop at all. The binding obligation the vote
carries is recorded at §4.5.5: Warp-C's model must stay capset-neutral so
Warp-6 extends the mechanism without reshaping the model.

### F3 — the v3d isolation posture, and I-45's ambition

| | Option | Consequence |
|---|---|---|
| a | Match Linux: one address space, document the gap | cheapest; I-45 is reserved-not-enforced on hardware forever |
| **b** | **Per-client GMP enforcement** (recommended) | ~128 KB granularity, one table reload per submit, VA allocator must be region-aligned per client; Mesa's simulator is the working reference; **nobody has done this** |
| c | Per-client page tables | 4 MB of contiguous PTEs per client + `PT_PA_BASE` rewrite per switch — the cost upstream calls "expensive" |

**DECIDED: (b), staged RESERVED->ENFORCED.** This is the fork with the
most at stake: **whichever we choose, the seam must carry per-client identity down
to submit from day one**, because retrofitting identity through an
"addresses-are-global" seam *is* the rework the charter forbids. (b) also makes
I-45 a genuine claim on hardware rather than a documented gap, and it is a real
novel-angle candidate — Linux has carried "this is not yet implemented" since
2018.

### F4 — NOVEL.md entry?

The synthesis has three things no shipping system has together: a **Magma-class
GPU seam that is ring-native from day one** (Magma's own RFC-0198 names
io_uring-style submission as *future work*; we have Loom today), a **GPU service
that is a file server** in the Plan 9 idiom (per-Proc namespace *is* the access
control), and **per-client GMP enforcement on v3d** if F3=(b). Vote: record as a
post-v1.0 NOVEL candidate. **DECIDED: yes.**

---

## 11. Naming — **Warp** (ratified 2026-08-07)

The graphics family is a weaving vocabulary: **Loom** (the ring transport),
**Weft** (the thread carried across), **Tapestry** (the picture), **weave** (the
framebuffer Burrow), **Kaua** (the text weave).

**Warp** — the GPU seam. Chosen by the user over the proposed "Jacquard", and
better than it, for a reason the proposal missed: the name carries **two exact
meanings at once**.

- **In weaving**, the warp is the set of tensioned lengthwise threads through
  which the weft is drawn. It is the structural half of a pair whose other half
  we have already named — **Weft** is the capability network dataplane, and its
  own ratification text calls it "the crosswise thread." Warp completes that
  pair rather than inventing a new metaphor.
- **In GPU vocabulary**, a *warp* is the SIMT execution group — NVIDIA's term
  (AMD: wavefront) for threads advancing in lockstep. It is one of the few words
  a graphics programmer will already read correctly on sight.

A name that is simultaneously native to the project's metaphor and native to the
domain is rare; that is the case for it.

**One prior usage, and its resolution.** `NOVEL.md` §Weft and
`NET-THROUGHPUT.md` describe Weft as "the crosswise thread woven through the
**Loom warp**" — i.e. earlier prose treats the warp as *Loom's* structure. That
is descriptive flavour, not a named component: nothing in the tree is *called*
Warp, so the identifier is unclaimed and the GPU seam takes it. The two
sentences should be reworded when next touched (to "through the warp" without
binding it to Loom); they are not load-bearing and no code refers to them. The
overload with graphics' *other* warps (cursor warping in SDL, image warping) is
noted and judged harmless — those are verbs in unrelated contexts.

Arc prefix: **Warp-0 … Warp-n**. Service tree: `/dev/warp`.

Not renamed, deliberately: `winsys`, `BO`, `fence`, `capset`, `context` — these
are Mesa/virtio-facing surfaces where the expected name is the communicative one
(the same argument that keeps `mmu_enable` and `dtb_init`).

---

## 12. The arc ladder

Chunk boundaries, each independently landable and gated. Names assume F-votes;
scope shifts with them.

| Chunk | Scope | Gate |
|---|---|---|
| **Warp-0** | this document + signoff | scripture commit |
| **Warp-1** | the host substrate (F1): CI leg, QEMU/crosvm plumbing, `virtio-gpu-gl` reachable, capset probe from tapestryd — **LANDED @`db050b21`** (gate met on thyla-gl: the 1384-byte VIRGL2 blob in-guest, `docs/phase7-status.md` row; llvmpipe baseline = the honest 2.4–5.9 fps band, #168 open) | `GET_CAPSET_INFO`/`GET_CAPSET` returns a `virgl_caps` blob in-guest |
| **Warp-2** | the seam: the `/dev/warp` tree, contexts, the GPU-BO subtype (§6.1) + its kernel gate, `SUBMIT_3D` routing, fence CQEs; **fix `BarTooLarge`** — **LANDED** (2a `ce70a3a9` #166 + shm discovery; 2b `e2accc2e` GPU-BO + both admission gates; 2c `2a3ab4f3` the tree; 2d `16d425cb` the attributed-completion controlq + the fenced lane; 2e the `/warp-prove` gate binary; as-built `docs/reference/149-warp.md`) | contexts create/destroy; a hand-built command stream round-trips — **the gate runs via `tools/warp-host.sh prove`** |
| **Warp-3** | `virgl_thylacine_winsys` (18 slots) + the client library; unmodified Mesa virgl driver — **LANDED** (3a `ef6af62c` seam capacity 128 + the `fence-signaled`/`bo-cap` ctl promotion; 3b `8b8ca40d` fork patch 0006: the winsys + `warp_client` + `virgl-prove`; 3c `eb62f97c` the builder cycle; `c64ddbe4` the #191 build-id cacheless fallback, port finding 4; as-built `docs/reference/149-warp.md`) | a triangle, in-guest, on the GPU — **met: `GL_RENDERER = virgl (Apple M2 (Compat))`, via `tools/warp-host.sh tri`** |
| **Warp-4** | present integration: `SET_SCANOUT` of a 3D resource (Direct), readback fallback (Composed) — **LANDED** (4a+4b `ec2bd8ad` the mutual-adoption protocol, both halves + fork patch 0007; 4c the gate: the launcher auto-detect + `tools/warp-host.sh quake` — both arms in one run, the `scanout direct N GL res R` switch live on thyla-gl; as-built `docs/reference/149-warp.md` §Warp-4) | **GLQuake on virgl** — **measured: ~3 fps aggregate at 1280×800 (the mechanism gate is met). PREMISE CORRECTED 2026-08-10: the 192.8 anchor was macOS/HVF and does not transfer (its own Warp-1 row said so); the same-host llvmpipe band is 2.4–5.9 fps at 640×480, so ~3 fps at 3.3× the pixels sits INSIDE the software band — #196 now asks stall-vs-compute (the `decomp` verb: per-arm unpaced figures + qemu CPU attribution), not "why 20–25× under"**. Findings ledger: #195 (GL-host capture), #197 (console ^C owner-only), #198 (demo-end context break), #199 (caught notes never interrupt blocking syscalls) |
| **Warp-5** | the focused audit; I-45 enumerated; reference docs | Fable-5-max round closed |
| **Warp-C** | **GPU composition (§4.5; designed 2026-08-13, RESERVED)** — the compositor's own virgl context; the screen becomes a host-side 3D resource; per-frame `VIRGL_CCMD_BLIT` composition replacing the readback; chrome becomes a damage-uploaded texture; the I-40 drain. Sub-chunks: **C-0** the two gating probes (§4.5.4 P1 cross-context blit with a pixel-asserting positive control, P2 cross-context ordering) — *nothing structural lands until both pass* — plus **C-0d**, the #240 detector (§4.5.4b: the sentinel stamp behind a sticky `stream-rejected` on the ctx ctl), which is a PREREQUISITE of P1b rather than a parallel nicety: a WITH-attach retry that silently does nothing is unreadable while a refusal reports success; **C-1 LANDED 2026-08-16** the spec extension (async present + drain; `drain_skipped` + a P2 ordering counterexample **per direction**, since the exclusion is symmetric) BEFORE impl — TLC-green, additive by measurement (the six pre-existing cfgs reproduce 5413 exactly with `ALLOW_COMPOSE = FALSE`), and it surfaced a C-2/C-3 obligation the prose had not: **the D1 recycle gate does not survive the composed path unchanged**, because tapestryd runs ONE host resource per surface and a present's terminal CQE stops meaning "free" once the compositor is a second reader; **C-2** compositor ctx + 3D screen (**owes the attach verb — P1b's authority-conferral point — and the blit/fill exclusion C-1 named, whose mechanism is decided at §4.5.8: one host resource PER SLOT for software surfaces, a fence for GL ones**); **C-3** blit composition + chrome-as-texture; **C-4** retire the readback path **where GL is available** (never delete it -- 4.5.9: `virgl=0` on the default dev device and there is no virtio-gpu at all on bare metal, so the CPU path is the UNIVERSAL one and permanent); **C-5** focused audit (an I-40 surface + a new cross-context authority path). **LANDED C-2a/2b/2c/2d + C-3 + C-4 (2026-08-16/17; §4.5.9–4.5.12)**; **C-5 CLOSED `27207c78`** (2026-08-17, 0 P0 + 0 P1 + 1 P2 + 2 P3 + 1 self-audit P3, all fixed) -- this row read "C-5 owed" for a day after it closed, which is the arc-state marker a fresh session reads first; **C-6** (the fenced DEFERRED compositor readback, §4.5.13, added after this ladder was written) **LANDED `24e6753d`** with its round closed `c8c83348` (1 P0 + 3 P1 + 3 P2 + 3 P3, DIRTY) and the owed follow-up closed CLEAN `93f660ed` (0 P0 + 1 P1 + 1 P2 + 3 P3). **The Warp-C arc is therefore COMPLETE and Warp-6 is next.** | **the composed path reaches direct-path parity** — i.e. the #215 43% is gone at 1280×800, measured by the same two-method protocol, with `ls-gfx*` byte-identical and tearing-freedom held under P2 stress. **Standing at C-4 (thyla-pi, V3D, `decomp gl`)**: the 43% (1.75×) is gone — composed/direct **1.22× on the no-readback `dbus-gl` lane (93.1 vs 112.7 fps, 1.9 ms/frame; ~0.5 ms of it server-side)** and 1.19× on egl-headless (37.6 vs 44.8, the backend's readback drain); `ls-gfx*` byte-identical (LS-CI 36/36 through C-3 and C-4); P2 measured 0/500 (C-0), the fenced form unbuilt. Full parity is not claimed: the residual is the compose blit's own GPU time + vrend's blitter setup, outside the server |
| **Warp-6** | Venus: hostmem mapping (§6.2), `vn_renderer_thylacine`, blobs — **now also owes the §4.5.5 generalization: the blob-mediated capset-neutral blit, extending Warp-C's mechanism without reshaping its model** | a Vulkan prover in-guest |
| **Warp-7+** | RPi: the v3d leaf driver on the MENAGERIE substrate — own charter, F3's posture built in | own gate |

Parallel and independent (the charter says so explicitly): **lavapipe** —
software Vulkan on the gallium+ORC stack we already run. Expected zero new kernel
work (I-42 JIT Burrows + torpor + weft cover it); deltas are a build flag
(`-Dvulkan-drivers=swrast`), a loader-less ICD entry, and
`VK_EXT_headless_surface` + the existing present glue.

---

## 13. The HW-GL exit bar (ratified 2026-08-12)

**The claim "hardware GL works" gets a measured bar, not a vibe.** On real
GPU silicon, HW GL must far outperform the software renderer — and the way to
hold ourselves to that without overfitting to one game is a **four-point
grid** measured on the reference HW host (thyla-pi — CLAUDE.md "The thyla-pi
host"; permanent):

| Leg | Stack | Where |
|---|---|---|
| native-SW | tyrquake + Mesa llvmpipe, Pi Debian, no virt | `warp-host.sh native-bench` |
| native-HW | tyrquake + Mesa V3D (real GPU), Pi Debian, no virt | `warp-host.sh native-bench` |
| guest-SW | tyrquake + llvmpipe in-guest (KVM) | the existing `decomp 2d` |
| guest-HW | tyrquake + virgl-on-V3D in-guest (KVM), **Direct arm** | the existing `quake` / `decomp gl` |

Same tyrquake source, same pak data, same timedemo, same resolution
(640×480 reference; 1280×800 optional second row), pacing/vsync OFF on every
leg (`vblank_mode=0` native; the unpaced arm in-guest).

**The anchor precondition** (validates the grid before any ratio is read):
native-SW and guest-SW agree within **20%**. llvmpipe is CPU-bound and KVM is
near-native, so if the SW legs disagree the grid is measuring some other tax
(host contention, thermal, a broken leg) and the run is VOID. This anchor is
what makes the ratio comparison isolate exactly one quantity: what OUR virt
GL stack costs on the HW leg (guest Mesa → virtio ring → tapestryd/
virglrenderer → V3D, versus native Mesa → V3D).

**The bar** (exit criteria for the arc's HW-GL performance claim; the #215
north star):

1. **Ratio**: guest-HW/guest-SW ≥ **0.5×** of native-HW/native-SW.
   Aspirational: ≥ 0.8×. (Exact parity is unattainable in principle — virgl
   adds real per-submit serialization; published virgl-vs-native experience
   is ~50–80% for well-behaved workloads, worse for draw-call-chatty ones,
   and GLQuake is 1996-chatty.)
2. **Absolute**: guest-HW ≥ **0.4×** native-HW at the same resolution.

**Measurement discipline**:
- The bar is measured on the **Direct arm** only. Composed carries a
  structural extra cost (the sync transfer + blit — tiler-hostile by
  construction on V3D) and is tracked as a separate figure, never the bar.
- **Thermal guard**: `vcgencmd get_throttled` must read clean (0x0) before
  and after each native leg, else the run is re-taken — a throttled native
  baseline silently flatters the guest ratio.
- **Present-cost honesty**: native legs run present-inclusive via KMSDRM
  where the host has a usable display path; if only a surfaceless context is
  available, the figures say so (both native legs share the omission, so the
  native ratio stays valid, but the absolute comparison weakens — note it).
- Swap-clean rule as everywhere: any leg with pswpin/pswpout movement is
  DISCARDED.

**Compass, not target** (the standing rule): tyrquake is the *proxy
workload*. The bar bounds the virt stack's overhead; it is not a mandate to
tune for one game. If a future workload class (Venus/Vulkan at Warp-6)
matters, it gets its own grid of the same shape, not a widened bar here.

**The native leg reads a display-less GPU host through surfaceless EGL + an
FBO** (`tools/warp/native-gl-bench.c`, run via `warp-host.sh native-bench`).
This is not a convenience — every windowed/canvas path (SDL, kmsdrm, glmark2
`--off-screen`) demands a native display connection the headless Pi does not
have, and `EGL_PLATFORM_SURFACELESS_MESA` + an FBO is both the *only* route
proven to reach real V3D there AND the faithful match to the guest's
render-to-FBO model (virglrenderer never touches a window). The bench is
deliberately **draw-call-heavy** (800 small textured/blended draws/frame,
`glFinish` per frame to mirror the guest's fence-per-submit) so its ratio
speaks to the per-submit axis. Caveat, recorded not hidden: the native leg is
this microbench while the guest legs are tyrquake — so the ratio comparison
is same-HW:SW-axis but cross-*workload*; native tyrquake awaits a display
path or a surfaceless vid backend. The guest ratio itself is same-workload
(tyrquake both legs), so it is a valid measurement on its own.

**First measurement (2026-08-12, thyla-pi, thermal-clean):**
- native HW (V3D 4.2.14) = **222.8 fps / 178 K draws·s⁻¹**; native SW
  (llvmpipe) = **20.2 fps / 16 K draws·s⁻¹** → **native HW/SW = 11.0×**
  (reproduced 11.13×). **The silicon is exonerated**: real V3D is 11× the
  software renderer, exactly as hardware GL should be.
- guest HW/SW = 0.6 / 16.2 = **0.037×** (same tyrquake both legs).
- So in-guest, hardware GL is **27× SLOWER** than software; on bare metal the
  same GPU is **11× FASTER**. The virt stack converts an 11× win into a 27×
  loss. Per-draw: native V3D sustains 178 K draws·s⁻¹, the guest ~600 (0.6 fps
  × ~1000 draws/frame) — a **~300× per-draw collapse** that localizes #215 to
  **per-submit serialization** in the guest→virtio→tapestryd→virgl path, NOT
  fill rate, shaders, S3TC, or the GPU. The bar (guest ≥ 5.5×) is missed by
  ~150×; closing it is a submit-pipelining problem (the #204 client throttle
  + deeper in-flight depth), not a silicon one.

---

## Cross-references

- `docs/TAPESTRY.md` §18 — the surface lifecycle, present protocol, weave share
- `docs/reference/139-tapestryd.md` — the compositor as built
- `docs/reference/138-gpud.md` — why the GPU owner must be resident
- `docs/reference/125-weft.md` — the share substrate this extends
- `docs/reference/142-sdl-port.md` — the SW-GL path that stays the fallback tier
- `docs/MENAGERIE.md` §4, §12 — the allowance model; the RPi substrate
- `docs/ARCHITECTURE.md` §28 — I-40, I-37, I-34, I-32, I-12; I-45 lands here
- `docs/LLVM-DESIGN.md` §8, `docs/JIT-ON-WX-DESIGN.md` — I-42, and why hardware
  GL does not need it
