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

---

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
| **Warp-6** | Venus: hostmem mapping (§6.2), `vn_renderer_thylacine`, blobs | a Vulkan prover in-guest |
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

Status at ratification: guest-HW/guest-SW ≈ **0.04×** (0.6 vs 16.2 fps,
2026-08-11, #215 open with the #213 storm confound) — roughly two orders of
magnitude below the bar. The native legs have not yet been measured; the
`native-bench` arm lands next and its first run doubles as the #215
discriminator battery (native V3D `GL_VERSION`/extensions vs the guest
capset; the `MESA_DEBUG=silent` A/B rides the same session).

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
