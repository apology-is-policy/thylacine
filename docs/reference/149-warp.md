# 149 — Warp: the `/dev/warp` GPU seam (contexts, GPU-BOs, the fenced lane)

**Status**: as-built at Warp-2 (sub-chunks 2a–2e; the seam kernel arc of
`docs/GPU-DESIGN.md` §12 row 2). Landing commits: `ce70a3a9` (2a, #166),
`e2accc2e` (2b), `2a3ab4f3` (2c), `16d425cb` (2d), `0a5a7f6c` (2e prover).
**Five audit rounds rewrote most of the mechanism this file documents, so
they are landing commits too**: `1451c3aa` (r1), `d3ce5f3e` (r2),
`ccc9bae2` (r3), `86cbf393` (r4), and the r5 close. Every round but the
last found a defect *in the previous round's fix*, and every finding was a
bound or a reclamation whose composition went unchecked — read that as the
standing hazard of this file's subject matter, not as history.
Consumers today: the `/warp-prove` gate binary; next: the Mesa virgl winsys
(Warp-3).

## Purpose

Warp is the GPU service seam: tapestryd (the compositor, which owns the
virtio-gpu function) serves a **second 9P tree** exposing rendering
**contexts**, **buffer objects** (GPU-BOs), an opaque **submit** lane, and a
**fence** completion stream. The tree realizes GPU-DESIGN §4: authority is
namespace visibility (I-1/I-28), the connection is the client identity (§12
F3), submissions are unparsed byte ranges (§2.1 — the host renderer owns 3D
validity), and fences are labels on completions the virtqueue already
delivers (§4.3).

Two doors, one tree:

- **`/srv/warp`** — the REAL client door. Each open attaches a fresh srvconn
  → a fresh `Conn` in tapestryd → its own identity. Conn death retires
  everything the conn minted. Clients (the prover, the Warp-3 winsys) use
  this.
- **`/dev/warp`** — a per-client mount POINT only. joey deliberately does
  NOT mount it (round-1 audit F1): a shared mount is ONE server-side
  connection, and since `owner_conn` gates every resolve, a global mount
  would have let any Proc drive any other's rendering context, read its
  buffers back, or destroy them — and left every later Proc unable to get
  a context at all. A client that wants the tree in its namespace mounts
  `/srv/warp` there itself, which is what makes "per-Proc by
  construction" true. joey's boot probe reads `ctl` over its own
  short-lived connection and closes it, holding no standing warp conn.

## The tree

```
ctl                          # "virgl <0|1>\ncapsets N\ncapset <id> <ver> <len>\nctxs <live>\npoisoned <n>\n"
                             # + "bo-cap <n>\nfence-lane <n>\nbo-peak <n>\nbo-bytes-peak <n>\n"
                             #   (#204: the per-ctx BO capacity, the per-ctx fenced share the client
                             #   adopts as its throttle depth, and the global backed-BO high-water
                             #   census on BOTH axes -- count AND bytes; bo-peak 26 against a 1024
                             #   cap with thousands of refusals proved the BYTE cap
                             #   [WARP_CTX_BACKING_MAX] is what saturates: few-but-large backings)
                             # + "create-refused-noctx <n>\ndiag-noctx-arms <n>\n"
                             #   (#198: create3d refusals that never resolved a ctx -- the ctl-parse
                             #   and ctl-no-record arms -- plus the OR of the WDIAG_* arm bits taken
                             #   with no ctx to charge them to. A refusal counted HERE and not in the
                             #   per-ctx "create-refused" is the signature of a chokepoint ABOVE the
                             #   ctx, which is what made the fid ceiling invisible on both endpoints)
                             # + "probe-parked <n>\nprobe-freed <n>\nverify-unknown <n>\n"
                             #   (C-0d audit F3/F5. The first pair is the #240 probe graveyard's
                             #   monotonic ledger: a wedged ctx destroy PARKS its probe backings,
                             #   the vindication FREES them, and parked-without-freed is precisely
                             #   the permanent handle leak F3 found. Monotonic on purpose -- a live
                             #   gauge reading 0 is equally satisfied by "the path never ran" [#184].
                             #   `verify-unknown` counts every probe that RAN and reached no
                             #   verdict, across all ctxs -- NOT a ctx that has no probe to ask
                             #   (round-2 F9). The per-arm console lines are one-shot per ctx
                             #   [they were a ~480 line/s storm at the per-frame cadence], so this
                             #   is the only surviving evidence of the RATE)
                             # + "probe-texture <n>\n"
                             #   (C-0d Fable round F1: ctx mints whose #240 probe fell back to the
                             #   TEXTURE pair because the BUFFER pair could not be minted, monotonic
                             #   since boot. On such a ctx a verify's transfers and readback are
                             #   blit jobs behind whatever the DEVICE has queued -- the exposure the
                             #   buffer pair removes -- so nonzero here says some ctx carried it)
                             # + "rb-issued <n> rb-landed <n> rb-dropped <n> rb-coalesced <n> rb-abandoned <n> rb-slot <0|1|2>\n"
                             #   (Warp-C C-6: the compositor readback arm's census -- readbacks
                             #   issued on the reserved fenced slot; rb-landed = composed at
                             #   completion; rb-dropped = the surface moved on between issue and
                             #   retire, the engine died, OR the device REFUSED the transfer
                             #   (round F2 -- the tag carries the response verdict, so an errored
                             #   readback is never composed from); rb-coalesced = presents that
                             #   enqueued behind an in-flight / poisoned slot instead of issuing;
                             #   rb-abandoned = never retired in FENCE_ABANDON_MS [the client ctx
                             #   poisoned]; rb-slot = the reserved slot free / busy / poisoned.
                             #   EVERY key is `rb-`-prefixed since main#247: the first cut shipped
                             #   four of six bare while claiming all were prefixed, and parse_field
                             #   returns the first whole-token hit -- a bare `issued` elsewhere in
                             #   the file would feed the gate's verdict arms the wrong counter
                             #   without erroring. The stall each COMPLETED readback paid is
                             #   `cost readback-wait` on the TAPESTRY ctl -- abandoned ones are no
                             #   longer charged to it, round F9)
                             # test-mode ONLY adds: "abandoned <n>\nfenced-free <n>\n"
                             #   (fenced-free counts the CLIENT pool -- FENCED_SLOTS - 1 since C-6;
                             #   the reserved compositor slot is never a client's)
caps                         # the RETAINED preferred capset blob, raw
ctx/
  new                        # open+read mints a ctx -> "<pub_id>\n" (ONE per conn; I-45)
  <id>/
    ctl                      # write: "capset <n>" | "rings <1..64>" | "destroy" | "verify"
                             # read: "<id>\npoisoned <0|1>\nleaked-count <n>\nleaked-bytes <n>\n"
                             #   + "stream-rejected <0|1>\nrejected-at <n>\nverify-seq <n>\n"
                             #   + "verify-ok <n>\n"
                             #     (#240 / C-0d: `verify` runs ONE health probe now -- cadence is
                             #     the client's. `stream-rejected` is STICKY and means the HOST
                             #     refused this ctx's commands while every fence retired normally;
                             #     the remedy is recreate, never retry. NOT `poisoned`, whose cause
                             #     is a chain that never retired. `rejected-at` names the VERIFY
                             #     that caught it, so the offending stream lies in
                             #     (previous verify, rejected-at] -- a window, never a command.
                             #     `verify-seq` counts probes ADMITTED -- it is incremented before
                             #     any device I/O, so it advances on the UNKNOWN arms too;
                             #     `verify-ok` counts those that reached a HEALTHY verdict and is
                             #     the ONE a reader must require to move, since a bare
                             #     `stream-rejected 0` is equally satisfied by "healthy",
                             #     "never asked" and "asked, answer unknown" [#184, audit F2].
                             #     `verify` is REFUSED with EAGAIN while this ctx has fenced work
                             #     outstanding or the ctx is poisoned: the probe rides the
                             #     synchronous slot, and past SUBMIT_DEADLINE_MS the engine latches
                             #     dead [audit F7]. WHAT THAT GATE BOUNDS, exactly [C-0d Fable
                             #     round F1]: it reads only this ctx's fence gauges, so it bounds
                             #     waits on this ctx's OWN queue and nothing else. On the BUFFER
                             #     probe pair every ctx gets when its mint succeeds, that is the
                             #     whole exposure -- buffer transfers and copies are CPU-side on a
                             #     tiled renderer, and the one way a GPU job lands on the probe's
                             #     resources is this client copying over them itself [audit F1's
                             #     measured attack], which its own gauges see. On the TEXTURE
                             #     fallback [`probe-texture` on the global ctl] each step is a blit
                             #     job behind whatever the DEVICE has queued, other clients' frames
                             #     included, and no per-ctx gauge sees those)
                             #   + "fences-in-flight <n>\nfence-signaled <n>\n" (promoted at Warp-3;
                             #     since C-6 `fences-in-flight` ALSO counts a compositor readback of
                             #     this ctx's adopted BO while it is in flight -- device work IS
                             #     outstanding on the ctx's resources -- while `fence-signaled`
                             #     never counts it: the client counts fences it ISSUED. That was
                             #     stated here BEFORE it was true on every path: the completion arm
                             #     guarded on `!tag.comp`, but a VINDICATION is produced after
                             #     abandonment has taken the tag, so a late-retiring compositor
                             #     readback credited the client -- whose ctx the tag names -- with a
                             #     fence it never issued. `warp_fence_wait` returns on
                             #     `signaled >= seq`, so one ahead means every wait returns ONE
                             #     FENCE EARLY for the ctx's life: the client may reuse a buffer the
                             #     GPU is still writing. Round F3 / main#242 put the bit on
                             #     `FenceVindication` too, sourced from the slot index)
                             #   + "bo-live <n>\nbo-peak <n>\nbo-bytes <n>\nbo-bytes-peak <n>\n"
                             #     (#204 census: backed now / high-water, count + bytes axes)
                             #   + "diag-arms <bits>\ncreate-refused <n>\n" (#198, appended LAST:
                             #     the OR of the WDIAG_* one-shot arm bits taken on this ctx and the
                             #     total create3d refusals charged to it. The arm bits name WHICH
                             #     silent path a refusal took -- WDIAG_RECORD_VANISHED = 16 is the
                             #     BO record consumed by a failed create3d [#218])
    submit                   # write: one Twrite = one atomic opaque CCMD submission (fenced)
    fence                    # read: the completion stream -- newest signaled fence id,
                             #       one record per read, PARKS when nothing unreported
    bo/
      new                    # open+read mints a BO record -> "<pub_id>\n"
                             #   (a create3d that does not return OK CONSUMES
                             #    the record -- #218; see Error paths)
      <id>/
        ctl                  # "create3d <target> <format> <bind> <w> <h> <d> <array>
                             #     <last_level> <samples> <flags> <size>"
                             # "transfer_to  <level> <x> <y> <z> <w> <h> <d> <offset> <stride> <layer_stride>"
                             # "transfer_from ..." (same tail) | "destroy"
        map                  # the Tweft map fid: t_weft_map(fd) -> client VA of the backing
        info                 # "res <id> size <n> stride 0 offset 0 w <w> h <h>\n"
```

Qid space: `WARP_FLAG = 1<<42` disjoins warp from the tapestry tree on the
shared `Conn` type; `WARP_CTX = 1<<39` / `WARP_BO = 1<<38` split levels; the
public id rides bits 8+ and the file kind the low byte. Public ids are
**monotonic, never reused** (the pane discipline — a stale fid resolves to
nothing; no generation machinery). The DEVICE ctx id is `slot+1`
(virglrenderer's id space is bounded; reused only after the synchronous
`CTX_DESTROY`). Resource ids come from `Comp.res_seq` — one device-global
space shared with the 2D surface resources.

## Kernel substrate (Warp-2a/2b)

**GPU-BO subtype** (`kernel/dma_handle.c`): `SYS_DMA_CREATE_GPU_BO = 106`
mints a `KObj_DMA` with `gpu_bo = true` — create-immutable, mutually
exclusive with `weave` by construction (`dma_create_body` takes a subtype
enum). Same gates as the weave: `CAP_HW_CREATE` + the I-34 allowance
`HW_RES_DMA` permit/commit pair; envelope `KOBJ_DMA_GPU_BO_MAX_SIZE` =
64 MiB. The safety argument is distinct and recorded on the field: a weave
is device-READ (pixels out); a GPU-BO is device-WRITTEN (render target /
readback), bounded by GPU-side translation only the trusted owner programs —
the client's RW cacheable mapping conveys zero hardware authority either
way (I-5).

**Share admission** widened at BOTH gates — `burrow_share_into` (claim) and
`sys_weft_share_for_proc` (register, `kernel/syscall.c` ~5191) — the two
MUST widen together (the Warp-2b test went red at the register gate when
only the claim side was widened). `WEFT_BIND_GPU_BO = 2`;
`weft_kind_maponly()` covers both map-only kinds on the clunk-unmap and
orphan-reaper legs (`g_maponly_bindings`), so a dead GPU service's stale
client BO mappings force-reclaim exactly like weaves. The client claim path
charges `shared_map_pages` (I-32) like the weave.

**Shared-memory discovery** (#166, `kernel/pci_handle.c`): `pci_walk_caps`
parses `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` (cfg_type 8, a `virtio_pci_cap64`
— 64-bit offset/length halves, `cap.id` = shmid) into `KObj_PCI.shm[2]`;
hostile layouts (bad/unassigned BAR, OOB extent in the non-wrapping 64-bit
form) reject the claim. `t_pci_info` grew 208 → 256 with `t_pci_shm[2]` at
offset 208 (asserts pinned in the kernel + both userspace mirrors).
Userspace `PciDev::claim` claims-but-does-not-map a BAR larger than the
1 MiB VA stride (the hostmem class); `shm_region(shmid)` returns the
device-PA extent. Mapping a subrange is the §6.2 Venus-chunk delta.

## The fenced lane (Warp-2d)

`usr/tapestryd/src/gpu.rs`. The G-1/G-5 controlq was single-in-flight
(descriptors 0/1, a `seq` cursor). Warp-2d rebuilds completion around
**used-ENTRY attribution**: `publish(head)` appends an avail entry;
`drain()` consumes used entries by the ENTRY's id (the head descriptor names
the slot). Slot 0 is the sync chain (all 2D + ctx/resource commands,
unchanged semantics); fenced slot `i` (0..16) owns the fixed descriptor pair
`(2+2i, 3+2i)` with its request buffer and response header in a SECOND DMA
region (`FLANE_DMA_SIZE` = 16×36 KiB + a response page at `GPU_FLANE_VA`),
allocated **only when VIRGL negotiates** — a 2D boot allocates nothing and
the audited two-page sync ring is byte-identical. `FENCED_SLOTS` went 4 → 16
at #204 (the per-ctx share 2 → 8): the depth-2 throttle, faithfully mirrored
client-side, serialized every GL frame against full guest→host→retire round
trips (#215). 16 is this layout's ceiling — exactly one response page at
`FRESP_STRIDE` (16 × 0x100 = PAGE), with 2 + 2×16 descriptor pairs carved
from the controlq, itself widened 16 → 64 (its QEMU device maximum; the
cursorq stays at its own maximum of 16, split out as `CURSORQ_SIZE`).
`FREQ_LEN` shrank 64 → 36 KiB in the same change: the lane is ONE plain
`SYS_DMA_CREATE` and 16×64 KiB + the response page overshot the kernel's
1 MiB per-buffer cap (`KOBJ_DMA_MAX_SIZE`) by one page — the allocation
failed and the warden restart-looped a console-less tapestryd. Caught by
the GL-host capset gate, and only there: a 2D boot never allocates the
lane, so the default suite is structurally blind to flane sizing. 36 KiB
still swallows anything the byte seam can deliver (one Twrite = one
submission; msize bounds the payload at ~32 KiB, `fenced_begin` refuses
larger cleanly); Mesa's 256 KiB `VIRGL_MAX_CMDBUF` only matters to the
Loom bulk path, which will carry its own sizing. Compile asserts now pin
both bounds (`FLANE_DMA_SIZE <= 1 MiB`, `FREQ_LEN >= 32 KiB + hdr`).

- **Presents stay wait-for-mine.** `submit_and_wait` drains fenced
  completions while waiting for ITS entry, so the stage-0 I-40 argument —
  the 2D in-flight set is empty inside every dispatch — is untouched.
- **G-5 discipline carries verbatim**: the used ring is the only completion
  authority; the ISR read is level hygiene per wake; the spin-poll nets the
  used.idx store-propagation window; the wall-clock deadline (500 ms of
  event-ful non-progress) applies to sync chains ONLY. A fence legitimately
  takes as long as its GL work; in-order controlq processing keeps the sync
  deadline honest under a fence backlog (non-fenced responses are written at
  processing time).
- **Fence ids are globally monotone** (`Gpu.fence_next`): QEMU's virgl fence
  walk retires every queued fence with id <= the signaled value, so
  independent per-ctx sequences would cross-release. Attribution stays
  per-slot, so the driver itself assumes no signal order.
- **A fenced ERROR response still retires its fence** (logged): a fence that
  can never signal is a client wedge — the completion is the retirement.
- **A full lane refuses** (`Again` → E_AGAIN), never blocks: the serve loop
  is the console (#31/#125).
- **One ctx may hold at most half the lane** (`WARP_CTX_FENCE_MAX =
  FENCED_SLOTS / 2`, round-5 F4). `alloc_fenced_slot` is first-fit over a
  PROCESS-WIDE pool and nothing capped a single ctx, so one unprivileged
  client could take all four slots — starving every other client for as
  long as its chains ran, then, at the abandonment deadline, poisoning all
  four at once and killing 3D for the whole box (`lane_exhausted` → the
  do-not-retry `E_IO`). Half leaves room for a second client always, and
  still admits the submit+transfer pair one client needs in flight
  together — which is exactly what `/warp-prove` does, so the number is
  load-bearing for the gate.
- Commands: `SUBMIT_3D` 0x0207 (header + size + inline stream; one Twrite =
  one atomic submission, iounit-bounded until the Loom-carried §4.1 path),
  `TRANSFER_TO/FROM_HOST_3D` 0x0205/0x0206 (fence-bearing by design — a
  readback's completion is exactly what the client waits for).

**The fence file** (`server.rs`): per-ctx bookkeeping
(`fences_in_flight` / `fence_signaled` / `fence_reported`). Since the
#210 fix, `fence_signaled` is a **dense per-ctx completion count** (`+= 1`
per non-abandoned retirement, FIFO within the single ring), NOT the
device-global fence id: the winsys counts fenced ops it *issued* and
compares against `fence-signaled`, and the pre-fix global id put the two
in different number spaces — any ctx minted after prior fenced work (the
SECOND GL client of a boot) saw `signaled >> issued`, its unsigned
in-flight throttle `issued - signaled` wrapped, and the client parked on
the fence file for a record only its own blocked submission could produce
(#210: deterministic second-launch deadlock, misread as "unpaced wedges"
because every wedge observation was a second launch — the probe ran its
paced leg first). The record's *content* is now the count too; the winsys
never parses it (the read is a doorbell, the counter is the authority). A
read returns one record when `signaled > reported` (records coalesce —
count N retires everything <= N) and PARKS otherwise (`PendingFence`, the
FK_EVENT netd leg) with all four cancel sites mirrored: clunk, Tversion
reset, conn teardown, Tflush. A dead ctx EOFs the stream — and so does a
**poisoned** one, unconditionally (round-5 F2). That EOF used to be
conditional on `fence_signaled <= fence_reported`, which the client could
suppress itself: the retire count is monotone, so any later submission
that completed left `signaled > reported` and the read returned that
record — which, under the coalescing rule above, *asserts the abandoned
fence completed*. A poisoned ctx also refuses new submissions and transfers with
`E_IO`: the poison is the ctx's terminal state, and the client must destroy
and re-mint (a vindication clears it and the stream resumes). The serve loop
(`main.rs`) pumps `warp_service_fences()` per pass and clamps the poll
timeout to 1 ms while fenced chains are in flight (the GPU IRQ is not
pollable; `irq.wait` exists only inside a sync submit).

**Teardown is DEFERRED, never blocking.** (Round-1 F5 replaced a 2 s
in-dispatch drain a client could multiply into minutes of frozen console.)
A retire of a quiesced ctx/BO completes immediately; otherwise the object
is marked `retiring` — instantly unresolvable to every client, and
excluded from `ctl`'s `ctxs` count and both readdir ladders — and
`warp_pump_retires` (run per serve-loop pass from `warp_service_fences`)
finishes it when the last fence lands.

Termination is the driver's. A fenced slot unretired for
`FENCE_ABANDON_MS` (30 s) is **abandoned**: released from the pool, its
descriptor pair poisoned (re-usable only once the device proves it is
done by retiring the chain late), and its owning ctx marked
`fence_poisoned`. A dead engine abandons every occupied slot at once.
Either way `fences_in_flight` always reaches 0, so no object can be
stranded in `retiring` (round-2 F5 — the first version of this argument
was false, because the abandonment sat behind the engine-dead
early-return).

A poisoned ctx **leaks** rather than frees: handle + mapping stay pinned
for the Proc's life, because an abandoned chain may still DMA the
backing. Leak-on-wedge, never UAF. Four consequences the code enforces:

- **A leaked backing is PARKED, not dropped** (round-5 F1). Its `WarpBo` —
  `dma_fd` deliberately still valid — moves into a per-ctx-slot graveyard,
  and the vindication that recovers the slot frees it from there, because
  the same device proof licenses both. Before this, vindication recovered
  the slot but never the pages: `wctx_finish` was the one `wbo_retire`
  caller of three that discarded the returned byte count, and it also
  `take()`s the ctx that `leaked_bytes` lives in — so each recovered slot
  handed the client a fresh `WARP_CTX_BACKING_MAX` while 64 MiB stayed
  gone. Per-ctx `leaked_bytes` still bounds a *live* ctx; the graveyard is
  what makes the bound survive the ctx.
- The graveyard **cannot overflow** (round-6 F1). Round 5 sized it by the
  16-wide `bos[]` and condemned a slot whose graveyard overflowed, calling
  the condemnation permanent — but `bos[]` slots are *reused*, so a
  poisoned-yet-live ctx (nothing gates BO mint or build on `fence_poisoned`)
  could mint/build/destroy in a loop and park far more than 16 over its
  life, bounded only by bytes: at the minimum accepted size of `PAGE` that
  is 16384 backings inside the 64 MiB budget. Each surplus record was
  dropped by value, and `WarpBo` has no `Drop`, so every drop leaked a
  kernel handle *and* a mapping. Worse, the condemnation was not permanent:
  the clean-retire path cleared the flag, so the slot recycled and the whole
  cycle re-armed for one abandoned fence per turn. The bound is now a
  creation-time cap on `leaked_count + live_backed` at
  `MAX_WARP_BOS_PER_CTX`, which admits at most one park per graveyard
  entry — so no record is ever dropped, the overflow flag is gone, and the
  ceiling `MAX_WARP_CTXS x WARP_CTX_BACKING_MAX` is true rather than
  asserted.
- A vindication that finds its ctx still **live** is a full reclamation
  point: `ctx_has_poisoned_slot` is the same device-done proof the slot
  recovery uses, so the parked backings are freed there and both leak
  counters reset. The uncharge is paired with the drop that actually frees.
  Without it a vindicated-but-healthy ctx stayed charged for memory it no
  longer held, and could be bricked at the count cap.
- **How reachable the wedge actually is** (#177, measured — this corrects a
  premise five audit rounds reasoned from). The rounds treated "a poisoned
  ctx churning backings" as freely client-reachable. It is not, on a
  *healthy* device: `submit_and_wait` calls `drain()` directly, so every
  synchronous controlq command is its own drain, and `create3d` issues
  four. The abandoned chain's late retire therefore lands on the very next
  command, vindicates the ctx, and the wedge evaporates before a second
  cycle can run — first observed when the #175 harness churned 17 rounds
  against a ctx it had correctly poisoned and watched heal instantly.
  Durably wedged therefore requires a device whose *fenced chain never
  retires* while the controlq still serves synchronous commands (a hung
  GL chain, not a dead engine — a dead engine fails `create3d`, so there
  is nothing to churn). That narrows the round-6 F1 exposure; it does not
  remove it, and the cap is what makes the narrow case bounded rather than
  unbounded. The harness models the stuck chain explicitly by deferring
  the late retire, which is why it can reach the cap at all.
- `WARP_CTX_FENCE_MAX = FENCED_SLOTS / 2` carries a build-time floor
  (`assert!(WARP_CTX_FENCE_MAX >= 2)`, round-6 F2). The share is a division,
  so it degenerates silently: at `FENCED_SLOTS = 1` the cap is 0 and every
  submit and transfer returns `E_AGAIN` forever — a dead 3D seam with no
  signal. `gpu.rs` pinned only the ceiling.
- A ctx **slot** retired while poisoned is itself poisoned, so its
  `dev_ctx` id is not re-minted while the device may still execute that
  context's stream. The slot returns to the pool only once the driver holds
  NO poisoned slot for that ctx (round-4 F1) **and `ctx_destroy` actually
  returned `Ok`** (round-4 F3, made a check rather than an assertion by
  round-5 F3 — `ctx_destroy` is fallible on a *healthy* engine, and the
  un-poison used to run regardless). The clean-retire path condemns its
  slot on the same refusal.
- `ctl`'s `poisoned` count reports how many slots are held back
  (round-2 F8), including the permanently condemned ones.
- A parked fence read on that ctx gets EOF, since the fence it waits on can
  never signal (round-2 F7, made unconditional by round-5 F2).

The retire order inside `wbo_retire` is R2-F5: weft_unshare FIRST (a
racing Tweft claim fails closed), then device detach (DETACH_BACKING +
CTX_DETACH + RESOURCE_UNREF), then — unless leaking — unmap + close (the
client's own mapping survives via the #847 dual count).

## The gate prover (Warp-2e)

`usr/warp-prove` (`/warp-prove` in ramfs): connects to `/srv/warp` directly,
mints ctx + BO, `create3d`s a 64×64 B8G8R8A8 render target, maps it and
sentinel-fills the sample points (the readback assert must DISCRIMINATE),
submits a hand-built VIRGL stream (`CREATE_SUB_CTX` + `SET_SUB_CTX` +
`CREATE_OBJECT(SURFACE)` + `SET_FRAMEBUFFER_STATE` + `CLEAR` red;
virgl_protocol.h encodings pinned as constants), queues
`TRANSFER_FROM_HOST_3D`, rides the fence file until the pixels land, asserts
five samples read `0xffff0000`, destroys the ctx, and asserts the live
count returns to 0. Exit 0 PASS / 2 SKIP (2D device) / 1 FAIL. Driven on
the GL host by `tools/warp-host.sh prove` → `tools/warp/warp-prove.exp`.

Two further legs run after the clean path:

**The poisoned path (#175)** — drives a ctx into the wedge state, which the
clean path never reaches and which six consecutive audit rounds each found a
defect in. `warp-hold on` stops the drain so a submitted fence stays in
flight; `warp-abandon` forces the transition the 30 s clock would otherwise
make; the churn loop asserts BO creation is refused *by attempt 17* (the
discriminator — "eventually refused" is true pre-fix too, at the 16384-attempt
byte cap); the release then asserts vindication walks the ctx back out.

**The two-client path (#180)** — the CROSS-client properties, which through
round 8 had only ever been validated by reading. Both clients live in ONE
process: `SYS_OPEN` on a `/srv/<name>` leaf is a *connect*
(`devsrv_open_connect` → `srvconn_create`, kernel/devsrv.c), so each
`t_open("/srv/warp")` is its own connection, and tapestryd keys ownership on
`owner_conn`. Two roots are therefore two clients with none of the
nondeterminism a second process would introduce — and B being able to mint a
ctx at all is itself the proof the conns are distinct, since `wctx_mint`
allows one ctx per conn. The legs: B cannot arm the hold while A holds it, and
cannot release A's (round-8 F1 — `hold_ctx` was scoped in effect but *global
in storage*, so B's arm silently displaced A's); a held lane still retires a
second client's fence (the property all of the #178 scoping exists to
provide); a holder that *disconnects* returns its fenced slots; a holder that
*destroys its ctx with the conn still open* also returns them; and
`warp-abandon` poisons only the abandoning client's ctx.

**The abandon-scoping leg is a PAIR, and the first half alone proves nothing
about scoping (#188).** L5a arranges the abandoner as the holder and asserts
the `abandoned` delta rose and its own ctx went poisoned — that is the
positive claim: *the lever fires, and takes its own*. Its bystander, though,
is unheld, so by the time the abandon runs the bystander's fence has long
since retired and it owns no in-flight slot at all. `abandon_matching` walks
in-flight fence SLOTS; with none of the bystander's to walk, a **global**
abandon and a scoped one do the identical thing to it. Reverting the scoping
to global therefore passed L5a unchanged — proved by sabotage, not supposed.

L5b closes it by putting the victim's fence genuinely at risk. Only the
holder can pin a fence in flight (the hold is what defers the chain's
completion), and round-8 F1 makes the hold exclusive — L1 asserts a second
client's arm is refused — so the two roles cannot both hold. The roles are
therefore *inverted* relative to L5a: the **victim** holds, and the
**abandoner** runs unheld. That makes the `abandoned` counter itself the
sharpest discriminator, because an unheld abandoner owns nothing in flight:
a correctly-scoped abandon can only be a no-op, while a global one must
consume the victim's slot. L5b asserts the delta is **zero**, the victim's
ctx is unpoisoned, and the victim's `fences-in-flight` is still nonzero —
positive survival evidence rather than mere absence of poison, and sound as
a gauge reading *here* only because the hold pins the chain (contrast #184,
where the same gauge on an unheld client would have raced).

The last two legs look like one leg and are not (#185). Conn departure was
already handled by the round-7 placement of the hold release, on
`warp_retire_conn`. The round-8 F1 fix moved it to `wctx_retire` for the case
its own comment names — a client that holds, submits, then `destroy`s its ctx
*without* closing the conn, where the swallowed retire kept
`fences_in_flight` nonzero so the pump could never finish the ctx it had just
been told to retire. Reverting that move passes the disconnect leg and fails
only the destroy leg, which is what makes the pair worth keeping separate.

Three read-only ctl fields exist so those legs have a bounded,
discriminating observable. Two of them stopped being `test-mode` at
Warp-3 — the winsys throttles and asserts on them, and a production
client cannot depend on a test-mode field — so `fences-in-flight` and
`fence-signaled` are unconditional per-ctx ctl surface now; only
`fenced-free` remains gated:

- **`fenced-free`** (global ctl) — `ctxs` cannot witness a stranded slot,
  because `warp_live_ctxs` excludes `retiring` contexts by design, so a ctx
  wedged forever reads exactly like one that finished (round-5 F5, one level
  down). The slot count is the resource that actually leaks.
- **`fences-in-flight`** (per-ctx ctl) — the only other way to watch a fence
  land is to read the fence fd, and that read *parks*. A regression in the
  per-ctx hold scope would then hang the prover and surface as a boot timeout,
  which is the least diagnosable failure this harness can produce. Since
  Warp-C C-6 it also counts the COMPOSITOR's readback of the ctx's adopted
  BO while that is in flight (the tag carries the client's ctx_pub); the
  admission share subtracts it (`comp_rb_in_flight`), so a display-adopted
  client's throttle is unaffected, but a reader asserting `== 0` at
  quiescence must not have a present of its adopted BO in flight.
- **`fence-signaled`** (per-ctx ctl) — the monotonic retire counter, and the
  one the held-lane leg actually asserts on. `fences-in-flight` is a *gauge*,
  and a gauge reading 0 is satisfied by "B's fence retired" and by "B never
  queued anything" alike, so a no-op regression in the submit path would have
  kept that leg green while the lane was dead (#184). `fence_signaled` only
  advances in `warp_service_fences`, which a *swallowed* retire never reaches
  — so an increase is positive evidence that a fence belonging to a second,
  unheld client really completed while the first client held. It is also the
  only form of the assertion that does not race: the L2 slot-count guard is
  sound there because the holder pins the slot, but B is deliberately unheld
  and retires promptly, so any in-flight snapshot of B would be a coin flip.

## The #240 health probe (C-0d)

`warp_ctx_verify` (`server.rs`) answers one question: are this context's
submitted commands still executing? It has to be asked out-of-band, because
the submit itself cannot fail — see the #240 caveat.

Each ctx carries a `CtxProbe` of two server-owned resources minted at ctx
create: `mark`, repainted with `PROBE_MARK` at the top of every verify, and
`sentinel`. **The pair is minted as BUFFERS** (`warp_hprobe_build`:
`PIPE_BUFFER`, `R8_UNORM`, one page each, the same mint as the compositor's
C-4 health pair) **and falls back to two 1x1 B8G8R8A8 textures**
(`warp_probe_build`) only where the buffer mint fails — counted on the global
ctl as `probe-texture`. C-0d Fable round F1: the client probe stayed a
texture pair after C-4 had measured that on a tiled renderer every texture
transfer and readback is a blit job appended behind everything the *device*
has queued, so client A's `verify` blocked the console for as long as client
B's queue was deep — and the `verify` admission gate reads only A's own
gauges, so it could not see it. Buffer transfers and copies are CPU-side
there. One verify is seed / copy / read, in the form the pair's kind selects
(`probe_upload` / `probe_copy_region` / `probe_readback` — one definition,
shared with the compositor's health pair):

1. write `PROBE_MARK` into `mark`'s backing and a per-verify token into
   `sentinel`'s, `TRANSFER_TO_HOST_3D` each (a 4-byte box on a buffer, the
   1x1 texel on a texture);
2. `submit_3d_sync` one `VIRGL_CCMD_RESOURCE_COPY_REGION` (opcode **17**,
   13 payload dwords) copying `mark` -> `sentinel` — 4 BYTES wide on a
   buffer pair, 1 texel on a texture pair;
3. `TRANSFER_FROM_HOST_3D` the sentinel and read the backing.

Reading `PROBE_MARK` means the copy ran. Reading the token back means it did
not, which latches `stream_rejected`.

Three choices carry the design, each earned:

- **The copy is stateless.** `RESOURCE_COPY_REGION` names src and dst
  explicitly and touches no bound state. A `CLEAR` would need
  `SET_FRAMEBUFFER_STATE`, and virgl context state persists across command
  buffers while Mesa dirty-tracks its own binds — so a clear-based probe
  would silently repoint a client's framebuffer at our sentinel.
- **Steps 1 and 3 are virtio-gpu commands, not command-buffer ones**, so
  they keep working on a latched context. That asymmetry IS the detector.
- **The healthy path is silent and costs nothing extra.** Reading back
  `PROBE_MARK` already proves `mark` held it, so no separate mark check is
  needed; the mark is re-read only on the path that is about to claim a
  context is dead. And no log line on success — a `say!` per verify at the
  per-frame cadence this verb targets would be its own performance defect.

The probe's resources are deliberately NOT in `bos[]`: every client-facing
resolve walks that array, so membership would let a client name them
through the `bo/<id>/*` tree. **That bounds the RESOLVERS, and nothing
more** — audit F1, confirmed by measurement on real V3D: the submit stream
is unparsed and carries raw device-global resource ids, and `bo/<id>/info`
hands those ids out from one shared counter, so a client computes
`mark = its_first_res - 2` and copies over it with a plain
`RESOURCE_COPY_REGION`. The server read its mark back as `0xFF00FF00`, the
client's own green. The defence is therefore not unreachability but the
per-verify **repaint**: `mark` is re-uploaded at the top of every verify,
inside the same dispatch as the copy and the readback, so corruption cannot
outlive one probe. (The prover's `C0-F1` leg re-runs that attack against the
buffer pair from a BUFFER source of the probe's own shape — a texture->buffer
copy is not a legal copy, and the leg would have read DEFENDED for the wrong
reason — and carries its own POSITIVE control: after the attack it copies the
mark back into its own buffer and reads that back through the fenced verb,
printing `C0-F1 ATTACK LANDED -- the mark read back … as 0xff00ff00` (a
client can WRITE and READ the probe's resources) before DEFENDED may print;
an unlanded attack is INSTRUMENT and the arm counts as not-defended, so
`C0-REJECT DONE` cannot be reached on a vacuous defence.) A probe that fails to mint leaves `probe: None`, and the ctx still
serves — it just cannot be asked. Failing the ctx mint on a diagnostic's
failure would hand clients a new way to be denied a context.

**Resource floor (C-0d Fable round F3).** The probe's two page mappings ride
tapestryd's shared `weave_va_next` bump and are never rewound:
`warp_probe_undo_guest` detaches the mapping (pages and handle freed) but the
VA range stays consumed, so every ctx mint/destroy cycle burns 2 pages of the
VA window on top of the weave allocations #171 tracks — the same
monotonic-VA class, with a second, ctx-churn driver; #171's reclaim must
cover these pages. The ~186-day bound stands, composed with #171's.

Teardown follows the same leak posture as BOs, in both halves (audit F3
found it matched in NEITHER): the **device** refs go on every path, exactly
as `wbo_retire` releases them unconditionally; the **guest** backing is what
a wedge defers, because the device may still be mid-DMA into those pages.
Deferred, not abandoned — the two backings are parked in
`warp_ctx_leaked_probe[slot]` and freed at the vindication that proves the
device finished, the same reclamation point that frees the parked BOs and
un-poisons the slot. Before F3 they were dropped with a `say!` and no park,
so every wedge burned two kernel handles and two mappings permanently in the
process that *is* the console, at one wedge per `FENCE_ABANDON_MS`.

A parked probe **implies** a poisoned slot — that direction is load-bearing
(it is what makes the mint's skip-poisoned-slots sufficient), and it does
**not** converse (round-2 F7): a slot is poisoned with nothing parked when
the ctx never built a probe, and on both destroy-refused arms. So
`poisoned` does not report a parked probe; `probe-parked` / `probe-freed`
on the global ctl are the only ledger (`prove_probe_reclaim` asserts both
halves move, and a sabotage removing the reclaim turns it red).

## The Mesa winsys + the warp client (Warp-3)

The unmodified virgl gallium driver reaches the seam through a new winsys
in the Mesa fork (`usr/ports/mesa/patches/0006-*`, round-trip-pinned by
tree hash in `usr/ports/mesa/README.md`):
`src/gallium/winsys/virgl/thylacine/` = `virgl_thylacine_winsys.c` (the
vtable) over `warp_client.{c,h}` (the transport). Build wiring keys on
`cc.get_define('__thylacine__')` — the cross file deliberately claims
`system='linux'`, so the compiler define is the only honest meson-level
signal. The osmesa target carries llvmpipe *and* virgl and forks at
runtime on `GALLIUM_DRIVER=virpipe|virgl`, falling back loudly to the
software screen.

**The client** is blocking file I/O on the `/srv/warp` tree via raw
`<thyla/syscall.h>` wrappers — no libc dependency beyond what the OSMesa
target already carries. One `warp_conn` per screen: the tree root, the
per-ctx ctl/submit/fence fds, the fence counters, and a `wedged` latch.
Open is deliberately paranoid: it requires `virgl 1` from the global ctl,
requires the `bo-cap` field (absent means a pre-Warp-3 tapestryd — fail
clean, don't guess), and completes one fence-counter read before
reporting the connection usable.

**Fences are counted, not parsed.** The client counts fenced ops it
ISSUED (submits + transfers, FIFO per ring) against the seam's monotonic
per-ctx `fence-signaled` ctl counter; the fence FILE is only the blocking
primitive — its coalesced record content is ignored, and it is never read
with nothing in flight, so no park is unfillable. The in-flight throttle
depth is DISCOVERED at `warp_open` (global ctl `fence-lane` = the server's
per-ctx admission share, 8 as of #204; absent = a pre-#204 server, depth 2;
clamped to [1, 64]) — the compile-time mirror it replaces sat at 2 after
the server lane grew, which is what serialized every frame (#215). The
depth arithmetic rides `inflight_depth()`, whose #210 conservation clamp
survives a server ledger that runs `signaled` past `issued` (the u64
subtraction would otherwise wrap huge and latch the throttle forever —
the #210 wedge shape, client-side): clamp to 0, warn once, and let the
server's own `E_AGAIN` admission be the back-pressure. `E_AGAIN` parks on
the fence and retries bounded. Any hard failure LATCHES the connection — the virgl
driver ignores `submit_cmd`'s return, so a dropped command buffer must
never be silent — and every subsequent entry point fails fast with a
`warp:` line on stderr.

**The vtable** is vtest's 18 load-bearing slots minus every displaytarget
arm (the OSMesa frontend reads back through pipe transfers, verified in
`osmesa.c`'s flush path, so `flush_frontbuffer` stays NULL — guarded
upstream). `transfer_get` is synchronous (queue + fence-wait) because the
driver maps and reads on return — the same choice vtest's protocol-v2 arm
makes. Submits split at CCMD boundaries (dword0 = len<<16) into ≤ 24 KiB
writes so one Twrite stays one atomic submission under msize 32 KiB; a
single command wider than the bound latches loudly (the Loom bulk path,
GPU-DESIGN §4.1, is the successor). `get_caps` strips
`COPY_TRANSFER_BOTH_DIRECTIONS` (vtest mirror — keeps textures on real
host backings); `supports_fences=0` (no fence fds), `encoded_transfers=1`,
`coherent=0` (no blob mappings — the F2 fork). A `virgl_resource_cache`
rides `last_use_seq` vs the signaled counter for `is_busy`; one mutex
guards the vtable entry points with unlocked internals (the cache
callbacks run under it).

**The gate binary** is `virgl_prove.c` beside `osmesa_prove.c` in the
osmesa target: link proof on the builder, triangle prover in-guest
(`/clade/bin/virgl-prove`). Discrimination is structural twice over: it
never walks the CAP_JIT clearance (llvmpipe cannot rasterise without it,
so a silent fallback cannot paint), and GL_RENDERER must name virgl and
not bare llvmpipe. It sets `GALLIUM_DRIVER=virpipe` itself (#151: `ut`
cannot export). Exit 0/2/1 = PASS/SKIP/FAIL; blue clear + red triangle +
interior/exterior sample asserts. Driven by `tools/warp-host.sh tri` →
`tools/warp/virgl-prove.exp` (per-attempt fixture isolation; the verdict
requires BOTH the prover's own PASS line and the scenario pass line —
#186). First contact found #191: virgl's disk-cache keying `assert`ed a
build-id note the static link never carries; the fork now runs cacheless
when the note is absent (port finding 4 in the README).

## The present integration (Warp-4): mutual adoption

A GL client's frame reaches the DISPLAY without ever crossing back into
guest memory (fullscreen) or with one server-side readback (windowed),
through a **mutual adoption** between a tapestry surface and a warp ctx:

- **The surface half** (`surface ctl`, owner-gated): `glsrc <ctx_pub>` /
  `glsrc off` -- accept that ctx as the display source. Naming a ctx
  grants nothing by itself.
- **The ctx half** (`ctx ctl`, owner-gated): `present-to <surface>
  <bo_pub>` / `present-to off` -- consent to displaying this ctx's BO on
  that surface. The surface INCARNATION is pinned at write time (slot +
  gen), so slot reuse can never re-arm a stale consent against a future
  tenant. Pure naming: the BO must be this ctx's own and alive, the
  surface must exist; geometry gates ACTIVITY, never the verb, so a
  resize racing the handshake degrades to inactive instead of failing.
  The named surface's `gl_retarget` (and the `off`-arm `res_stale`) fire
  ONLY if that surface's `gl_src` already names this ctx (Warp-5 F1): the
  surface lives on a DIFFERENT connection than the ctx (a process holds a
  `/srv/warp` conn AND a `/srv/tapestry` conn), so "owner-gated by its own
  ctl" does not by itself stop a ctx from perturbing a stranger's surface
  -- the `gl_src == self` gate does. A not-yet-consenting surface is left
  untouched; its own later `glsrc` write drives its retarget.

Adoption is ACTIVE (`gl_adoption`, resolved fresh at every use -- nothing
cached, either side's death is inert) iff both halves name each other,
the BO is alive, and `bo.w/h == surface.w/h`. Present routing then
becomes, by scanout mode:

- **Direct** (fullscreen, the game case): the F16 pending switch binds
  `SET_SCANOUT(bo.res_id)`; each present is ONLY a `RESOURCE_FLUSH` --
  zero guest transfers. Ordering is structural: the client's SUBMIT_3D
  was queued on the one controlq before its tpresent arrived (the client
  serializes flush-then-present; the server is single-threaded), so the
  flush displays the completed frame. The client's swap downgrades
  glFinish to glFlush (the winsys's discovered fence throttle — depth 8
  since #204 — bounds run-ahead).
- **Composed** (windowed, the ladder's readback fallback -- taken for
  every adopted BO the GPU arm cannot compose: not `composable`, an
  unwitnessed import, a latched compositor context, no 3D screen). Since
  **Warp-C C-6** (GPU-DESIGN 4.5.13) a **FENCED `TRANSFER_FROM_HOST_3D`
  with a DEFERRED completion**: the present ISSUES the readback on the
  fenced lane's one **reserved slot** (`COMP_FSLOT` = FENCED_SLOTS - 1;
  clients allocate first-fit over the other 15, the per-ctx share is
  unchanged) under the CLIENT's dev_ctx (the resource is attached
  there), tagged `FenceTag { comp: true, readback: true, ctx_pub: <the
  client's> }`, records it (`Comp.comp_rb`: fence id + surface + gen +
  ctx/BO/resource/backing/geometry), replies to the tpresent, and
  returns -- the dispatch never waits for the frame. The fence pump
  (`warp_service_fences`) routes the retire to `comp_readback_retired`,
  which RE-VALIDATES the surface (alive, same gen, scanout still
  Composed, `gl_adoption` still resolving to the SAME ctx/BO/res/va/w/h)
  and only then `blit_composed_pixels(.., Some(va))` + `screen_push`,
  exactly the compose the synchronous arm did (letterbox/crop shared
  with the weave path; `res_stale` stays TRUE; no orientation flip --
  the transfer contract is gallium top-down); a surface that moved on
  drops the frame (`rb-dropped`), a stale composition being worse than
  none -- as does a readback the device REFUSED (round F2: `FenceTag.ok`
  carries the response verdict, which the fenced form had dropped when
  it left the synchronous `.is_ok()` gate behind; composing on an error
  paints whatever the backing held, and zeros on a fresh BO mean the
  pane BLANKS while the census records a landed frame). `composed cpu` counts at completion. **One in flight,
  latest wins:** a present arriving while the readback is in flight (or
  while the reserved slot is poisoned) enqueues the surface incarnation
  on `rb_wanted` (FIFO, **at most one entry per surface SLOT** with the
  latest generation overwriting in place -- round F6: the first cut
  deduped on `(slot, gen)` and claimed a MAX_SURFACES bound that `gen`,
  drawn from a monotonic counter, made false) and the completion /
  vindication pump issues ONE fresh readback of whatever the BO holds
  then -- so a client's present rate cannot pile readbacks. **Retire
  safety:** the readback is counted in the client ctx's
  `fences_in_flight` (every quiesce predicate -- `wctx_retire`,
  `warp_pump_retires`, `wbo_destroy`'s leak posture -- reads it, so the
  backing the device writes into cannot free under it; `gl_adoption`
  refuses a retiring BO/ctx at completion, so a destroy in flight drops
  the frame instead of reading a freed backing) AND in the new
  `comp_rb_in_flight`, which `warp_fenced_admit` SUBTRACTS (the client's
  share is not shortened by a fence it did not issue; its
  `fence-signaled` ledger never counts ours). Abandonment at
  `FENCE_ABANDON_MS` poisons the client's ctx exactly like a client
  fence would (the tag carries the CLIENT's ctx_pub -- 0 is
  `warp_ctx_vindicate`'s no-slot sentinel and would let a late retire
  `ctx_destroy` an unrelated live context; and the client's own
  vindication must wait for OUR slot too, round-4 F1) and the reserved
  slot stays poisoned until the device's late retire vindicates it --
  meanwhile every readback-arm surface parks on stale frames (the blit
  and 2D paths are untouched). **What the console still pays under
  QEMU/virgl (F2b):** the readback of a BUSY resource executes
  synchronously at decode on QEMU's serial main loop, so a sync step the
  console issues while it is in flight (any present's transfer/flush, a
  health read) inherits that stall -- C-6 removes the wait from the
  present that ISSUES it, the per-present multiplication (one in flight,
  coalesced), and the false dead-latch (below); it cannot remove the
  stall a host executes synchronously (Venus / v3d-native's to remove).
  **The deadline made honest:** while ANY readback -- a client's
  `transfer_from` or ours -- is in flight (`Controlq::readback_in_flight`,
  the `readback` bit on the tag), the sync slot's stale-wake deadline is
  `FENCE_ABANDON_MS` (30 s), not `SUBMIT_DEADLINE_MS` (500 ms), sticky
  for the wait once observed: busy is not dead. `Cost::Readback` now
  times the ISSUE; `Cost::ReadbackWait` (`cost readback-wait`) is the
  issue-to-retire wall per completed readback = the stall the device
  paid -- **an abandoned readback is not charged to it** (round F9: it
  measured a stall that never ended, a different quantity in the same
  units). `verify` while our readback is in flight answers `E_AGAIN`
  (the ctx's `fences_in_flight` counts it -- device work IS outstanding
  on its resources), the one client-visible change. Census: warp global
  ctl `rb-issued N rb-landed N rb-dropped N rb-coalesced N rb-abandoned
  N rb-slot S` (S: 0 free / 1 in flight / 2 poisoned; **every** key is
  `rb-`-prefixed since main#247). The
  synchronous form was C-0d Fable F2 [P1] (the response IS the
  completion, so the console waited on the client's queue length, and
  `fence_poisoned` could not guard it -- produced by `reap_abandoned` on
  the loop that was blocked). Gate: `tools/warp-host.sh readback`
  (`warp-prove readback` -> `tools/warp/warp-readback.exp`).
- HOLD is refused in every GL arm (`E_OPNOTSUPP`): its contract is a
  DEFERRED device-visible flush and the GL arms have no deferral.

`Comp.bound_res` records the DEVICE's currently bound resource (0 =
none), distinct from the mode-machine's intent -- they diverge across
soft-Off retarget windows. Every path that can free an adopted BO
(`wbo_destroy`, `wctx_retire` -- the same chokepoints as the fence-hold
release) first withdraws the consent, retargets the partner surface
through the uniform pending rule, and **evicts the device binding before
any `resource_unref`** (an unref of the scanned-out resource is the one
order the display cannot survive); surface retire runs the same eviction
keyed on `bound_res`. On adoption end the weave is marked stale and the
surface's next present restores a compositor-owned scanout.

The client side (fork patch 0007 + the SDL shim): `OSMesaThylacineDirect
(ctx, surface_id, *ctx_pub_out)` places the consent through the winsys's
own connection, returns the ctx pub for the shim's `glsrc`, and
suppresses the flush_front readback; `OSMesaThylacineDirectOff` restores
it. One per-process owner (the warp ctx is per-process): a second GL
context negotiating steals the consent and the loser's readback resumes.
The shim re-negotiates on EVERY bind -- a reweave reallocates the
framebuffer, and the consent names one BO. Both halves failing restores
the readback path explicitly: a half-negotiated state (readback
suppressed, no display source) would be a frozen pane.

## Performance characteristics

The #196 decomposition (2026-08-10, `tools/warp-host.sh decomp gl|2d` →
`tools/warp/glq-decomp.exp`): GLQuake timedemo1, 1280×800 windowed,
unpaced (the rp6 `/env` wrapper), one boot per device, two launches per
boot (Composed-only, then early-zoom Direct-only), each demo window
sampled for whole-guest qemu CPU as `/proc/<pid>/stat` utime+stime
interval deltas (Linux `ps %cpu` is a lifetime average and cannot
attribute a demo window). Host: thyla-gl, TCG `-smp 4` (400% available).
Raw sampler files: `build/cpu-{gl,2d}-{composed,direct}.txt`.

| leg | fps | guest CPU mean | swap |
|---|---|---|---|
| virgl Composed | 2.4 (969 frames, 397.7 s) | 170% | clean |
| virgl Direct | **wedged — #210** | 26% (idle floor) | — |
| llvmpipe Composed 1280×800 | 2.2 | 354% | pswpin +280, discarded |
| llvmpipe Direct 1280×800 | 2.4 | 359% | clean |

**Asterisk (#213, found after these figures landed)**: the virgl legs
rendered with a *failing texture stream* — `MAX_WARP_BOS_PER_CTX` (128)
is smaller than GLQuake's texture count, so creates past the cap
streamed `GL_OUT_OF_MEMORY` (1889 lines in the 4c gate log; invisible
frames — #195) and the game drew a reduced texture set. Directionally
this strengthens "no hidden stall" (a *lighter* virgl frame still only
matched llvmpipe) but weakens the like-for-like parity claim; re-measure
after the #213 cap fix. **Resolved at #204**: the cap is 1024 (the
`bo-peak` census fields are the witness the width is sized against —
read them after a real workload instead of re-guessing), so these
figures are superseded by the post-#204 re-measure.

### Where the hardware-GL frame actually goes (#215, 2026-08-13)

Measured on **thyla-pi under KVM on real V3D 4.2.14.0** — not the TCG
figures above, which the #204 fix superseded. Two independent methods,
different lanes and different pacing modes, agree to ~2%.

`glq-decomp gl`, unpaced, 1280×800, one boot, both arms swap-clean:

| arm | fps | wall ms/frame | qemu CPU ms/frame |
|---|---:|---:|---:|
| composed | 25.4 (969 frames, 38.1 s) | 39.32 | 44.4 |
| direct | **44.4** (969 frames, 21.8 s) | 22.50 | 30.8 |

**The composed present path costs 16.8 ms of a 39.3 ms frame — 43% of
the frame is spent getting the picture out, not drawing it.** Removing
it is a *measured* **1.75×**, not an extrapolation: the direct arm ran
at 44.4 fps.

It is work, not a stall. Raw `%CPU` inverts the conclusion and must be
normalised by frame rate first: composed spends **+16.8 ms wall and
+13.7 ms CPU** per frame, so 81% of the extra time is real CPU burn.
Direct's higher raw `%CPU` (136.7 vs 112.9) only reflects it pushing
1.75× more frames per second.

The quarry resolution sweep (paced, all legs mode-witnessed) fits
`hw-gl = 22.2 ms + 16.8 ns/px`, and the two terms land on the two arms:

- direct frame time 22.50 ms ↔ fitted fixed term 22.24 ms (1.2% apart)
- composed−direct 16.82 ms ↔ fitted pixel term at 1,024,000 px
  (16.8 ns/px = 17.18 ms) (2.1% apart)

So the sweep's apparent *resolution scaling was never fill or shading* —
it is this readback, proportional to pixels because it copies ~4 MB every
frame. That is also why hw-gl scaled much like the software rasterizer:
both move pixels, neither was shading-bound. The frame pacer is
exonerated at this resolution (the unpaced lane reproduces the paced
quarry leg to the decimal, 38.1 s/25.4 fps vs 38.2 s/25.4 fps).

The mechanism is explicit in `server.rs` ~5958: a composed GL present
calls `transfer_from_3d_sync` to pull the adopted frame **host→guest**,
`blit_composed_pixels` to compose it CPU-side, then pushes it back out —
a full round trip per frame. The direct arm (~5823) instead binds the
client's own 3D resource as the scanout, where "the frame is already
host-side, so there is no guest transfer at all". Composition is what
forces the round trip, and *anything else on screen forces composition*,
so **composed is the common case**. The synchronous choice is deliberate
and load-bearing — "synchronously, so the present stays one dispatch
(the I-40 premise)" — so removing it is a design question, not a patch.

Direct eligibility is narrow by construction (`reconcile`, ~1536): the
mode is `Direct(n)` only when there is **exactly one visible surface,
exactly one leaf, and it covers the full display**; every other
arrangement is `Composed`. So a fullscreen GL client already gets the
44.4 fps path for free, while *any* second surface on screen — the
console pane, aurora's tab bar — drops the frame to 25.4. That is the
practical shape of the 1.75×: it is not a latent optimisation for the
fullscreen case, it is the cost of windowing a GL client at all.

**The frame pacer is exonerated outright**, by a within-boot paired A/B
(13 legs: six unpaced/paced pairs at matched resolutions plus a drift
control, every leg mode- and pace-witnessed):

| leg | unpaced | paced | Δ |
|---|---:|---:|---:|
| sw@320×240 | 50.1 | 49.2 | −1.8% |
| sw@1280×800 | 32.2 | 32.5 | +0.9% |
| hw-gl@320×240 | 42.4 | 42.0 | −0.9% |
| hw-gl@640×480 | 35.5 | 35.6 | +0.3% |
| hw-gl@1024×768 | 27.8 | 27.8 | 0.0% |
| hw-gl@1280×800 | 25.4 | 25.2 | −0.8% |

Every pair is inside ±1.8%, at *low* resolution as well as high — so the
`IDLE_HZ=15` / hidden-surface concern does not bite here (the `#164`
`Comp::animating()` present-pressure rule holds). The drift control
closed at −1.7% across 13 legs, and the paced legs reproduce a separate
boot's sweep to within 1.2% on every row, so the lane is repeatable
boot-to-boot. Bench legs nonetheless default to unpaced (`abd47935`) —
matching every other lane and the pacer's own "benchmarks" note — with
`:paced` to opt back in.

The plumbing for a GPU-side composite already exists in part:
`Gpu::submit_3d` forwards an arbitrary VIRGL_CCMD stream, and
`ctx_create` / `resource_create_3d` / `ctx_attach_resource` are all
present. What is missing is an encoder — tapestryd forwards client
streams but authors none, so compositing host-side would mean emitting
(at minimum) a `VIRGL_CCMD_BLIT` per adopted surface into the scanout
resource. That is the Wayland/dmabuf and Fuchsia/Scenic answer (import
the client buffer as a texture, composite on the GPU); Plan 9's rio is
CPU-composited and does not speak to it.

Reading: the Composed-GL arm has **no per-frame stall** — it reaches the
4-thread llvmpipe control's exact fps using 2.1× less CPU with ~2.3
vCPUs idle. The bound is *serialized single-threaded guest work* (the
game thread's virgl protocol encode + the per-present synchronous
server-side `transfer_from_3d_sync` + blit), all TCG-amplified; on
native silicon both serial phases shrink by the TCG factor and the GPU
path wins outright. Per-frame op structure (the leg-(d) census): the
frame's cmdstream submits as fenced `Twrite` chunks ≤ 24 KiB split at
CCMD boundaries; the discovered in-flight throttle (depth 2 then; 8
since #204) parks via one blocking
fence-file read + one ctx-ctl snapshot re-read per wait; a present is
one tapestry RPC (Direct: server-side `RESOURCE_FLUSH` only; Composed
adds the client `glFinish` fence round-trip and the server's 4 MB sync
transfer + blit); fence-signal latency floors at the serve loop's 1 ms
fenced-chains poll clamp. The Direct arm's throughput is unmeasurable
while #210 stands; the Warp-4c aggregate (3.0 fps, paced, direct-
majority) bounds it ~25–40% above Composed.

### The composed residual decomposed (Warp-C C-4, 2026-08-17)

Measured on thyla-pi (KVM, V3D 4.2.14.0), `tools/warp-host.sh decomp gl`,
GLQuake timedemo1 1280×800 windowed unpaced, one boot per row, with
tapestryd's present-path cost census (GPU-DESIGN §4.5.12) — server-side
wall per present, decomposed by op. Every row names its display lane: under
`egl-headless` each flush is a full-frame host readback (~17 ms of every
direct frame — the instrument's cost, not the guest's); `dbus-gl` has none.

| lane | tapestryd | composed fps | direct fps | Δ ms/frame | composed-BO present | = blit + health + flush | direct present |
|---|---|---:|---:|---:|---:|---|---:|
| egl-headless | C-3 as landed (`7296bf07`) | 36.9 | 44.8 | 4.8 | 20.7 ms | 1.44 + 8.34 + 11.12 | 17.0 (flush 16.9) |
| egl-headless | + deferred read (texture pair) | 37.2 | 44.5 | 4.4 | 20.3 | 1.40 + 2.45 + 16.46 | 17.2 |
| egl-headless | + buffer health pair | 37.5 | 44.4 | 4.2 | 20.2 | 1.32 + 0.19 + 18.57 | 17.2 |
| egl-headless | + the issue-step poison control (**C-4 final**) | 37.6 | 44.8 | 4.3 | 20.1 | 1.39 + 0.24 + 18.37 | 17.0 |
| dbus-gl | C-3 as landed | 62.8 | 113.2 | 7.1 | 9.62 | 1.63 + 8.92 + 0.12 | 2.73 (flush 2.67) |
| dbus-gl | + deferred read (texture pair) | 84.5 | 112.4 | 2.9 | 4.95 | 3.06 + 3.67 + 0.14 | 2.68 |
| dbus-gl | + buffer health pair | 92.8 | 113.0 | 1.9 | 3.18 | ~2.9 + 0.17 + 0.14 | 2.67 |
| dbus-gl | + the issue-step poison control (**C-4 final**) | **93.1** | **112.7** | **1.9** | **3.48** | ~3.2 + 0.21 + 0.14 | 2.45 |

Readings. (1) The health verify was the residual: `comp_ctx_health` on two
1×1 TEXTURES ran once per tick (= once per present at these rates) and its
readback waited for a blit job queued behind every client frame in flight
— 8.3–8.9 ms per call on both lanes, a `glFinish` the direct arm's
`glFlush`-only swap never pays. Deferring the read by 4 ticks did not
remove it (a texture readback is itself a blit into staging, enqueued
behind whatever is queued at READ time: `health-read` still ~15 ms per
working call); minting the pair as `PIPE_BUFFER` resources did (buffer
transfers and copies are CPU-side on v3d: `health-issue` 0.43 ms +
`health-read` 0.19 ms per 4-tick period, 0.17 ms per present; with the
issue step's poison-readback control 0.58 + 0.20 per period, 0.21 ms per
present). (2) The
`blit` and `flush-direct` figures are mostly FIFO wait behind the client's
frame decode already in the controlq when the present arrives, not the
op's own work — the composed blit pays what the direct flush pays. (3) On
egl-headless the composed/direct gap is unchanged after C-4 because the
frame's GPU drain moved from the health readback into the flush's readback
(11.1 → 18.6 ms), which was always going to pay it; the 4.2 ms that remain
there are the backend's. (4) The residual on the no-readback lane is 1.9
ms/frame (1.22×), of which ~0.5 ms is server-side; the rest is the compose
blit's GPU time and vrend's blitter setup on the host thread the client's
decode shares. `present-composed-cpu` and `readback` read zero on both GL
legs: the BO arm carried every present. Raw logs:
`build/warp-decomp-gl{,-dbus}-c4-run{1,2,3,4}.out` (this session's names;
the verb writes `build/warp-decomp-gl[-dbus-gl].log`; run 4 = the final
binary, ramfs md5 `207d2039…`).

## Error paths

| Path | Verdict |
|---|---|
| any warp file on a 2D device (`virgl 0`) needing the device | `E_OPNOTSUPP` |
| ctx/BO resolve not owned by this conn, or dead | `E_NOENT` |
| second ctx mint on one conn; ctx-slot/BO exhaustion | `E_NOMEM` |
| `create3d` refused (size, implausible geometry, ctx backing cap) | `E_IO` — **and the mint record is CONSUMED** (#218): every non-OK create3d, including the parse/`E_OPNOTSUPP` arms, unmints the still-unbuilt record (`wbo_unmint_refused`, owner-conn + unbuilt + non-retiring gated), so a per-texture refusal loop can never fill `bos[]` with corpses and starve `bo/new`. The benign repeat-create3d on a BUILT bo is still refused `E_IO` but the live record is untouched |
| submit/transfer, fenced lane momentarily full | `E_AGAIN` (retry) |
| fenced lane permanently exhausted (every slot poisoned) | `E_IO` (do NOT retry) |
| submit stream larger than a slot | `E_INVAL` |
| engine dead (latched) | `E_IO` |
| fence read with `count` < one record (21 bytes) | empty read (never parks unfillable) |
| malformed ctl verbs / non-UTF-8 | `E_INVAL` |
| **stream the HOST refuses (vrend context error)** | **none on the write — it is reported as SUCCESS (#240).** The write returns the byte count, the fence retires, `fence-signaled` increments, `fences-in-flight` returns to 0, `poisoned` stays 0. Since C-0d it is DETECTABLE out-of-band: write `verify` to the ctx ctl, then read `stream-rejected`. See the caveat below |
| `verify` on a ctx with no probe (its mint failed) | `Ok` on the write, but neither `verify-seq` nor `verify-ok` advances, `stream-rejected` is untouched, and the global `verify-unknown` does **not** move either (round-2 F9 — an unaskable question is not an unknown verdict, and counting it let a client drive that counter at 9P-write rate, since the per-ctx rate limit sits below this arm) |
| `verify` while the ctx has fenced work outstanding **or is poisoned** — including a COMPOSITOR readback of its adopted BO in flight (Warp-C C-6) | `E_AGAIN` (audit F7, corrected by round-2 F1). The probe rides the **synchronous** slot on the client's own ctx, and past `SUBMIT_DEADLINE_MS` (500 ms) the engine latches `dead` — terminal, in the process that *is* the console. **What the gate bounds (C-0d Fable round F1): waits on the caller's OWN queue, and only those** — it reads only the caller's gauges. On the **buffer** probe pair (`warp_hprobe_build`, every ctx whose mint succeeds) that is the whole exposure: buffer transfers and copies are CPU-side on a tiled renderer, so nothing in the verify waits for the GPU unless this client has put a job on the probe's own resources (audit F1's attack), which its gauges see. On the **texture** fallback (`probe-texture` on the global ctl) each step is a blit job behind whatever the *device* has queued — client B's frames included — which no per-ctx gauge can see; that is why the buffer pair is minted first. Before this round the client probe was a texture pair unconditionally, and this row's "queues behind that client's GL work" was false: it queued behind everyone's. **Quiescent means `fences-in-flight 0` AND `poisoned 0`**: an abandoned fence takes its slot and zeroes *both* in-flight gauges while the GL work is by definition still unfinished after 30 s, so the poison flag is the only witness left — the same predicate `warp_fenced_admit` refuses on one lane over. **Caveat (round-3 F5): the gate reads the seam's own counter while the `fences-in-flight` key publishes the device-side one, and the seam's leads the ctl by up to one serve-loop pass** (a chain retiring inside the client's own dispatch empties the device slot immediately, but the per-ctx counter drops only when the serve loop next pumps fences). So the two can disagree briefly and a client can be refused while the published keys read quiescent. It fails safe — a spurious refusal, never a spurious admission — and `E_AGAIN` is retryable regardless |
| `verify` that runs but reaches no verdict (UNKNOWN) | `Ok`, `verify-seq` **advances**, `verify-ok` does **not**, `stream-rejected` untouched; the global `verify-unknown` increments. **`verify-seq` counts probes ADMITTED, not probes that concluded** (audit F2 — it is incremented before any device I/O). A reader tells "asked and healthy" from both "could not be asked" and "asked, no answer" only by requiring **`verify-ok`** to move; a bare `stream-rejected 0` is satisfied by all three (#184) |

## Known caveats / footguns

- **#240 — a host-refused stream is reported as SUCCESS, and it kills the
  context permanently.** MEASURED 2026-08-14 on thyla-pi (KVM, real V3D),
  `tools/warp-host.sh reject`. Submitting a stream vrend refuses (here a
  `CLEAR` with a zero-dword payload → `Illegal command buffer 7`) is
  indistinguishable at the seam from submitting a valid one: the two ctxs
  in the A/B differ in exactly one variable, both start at
  `fence-signaled 0`, and both read `poison 0 sig 1 inflight 0` at t=0.
  The refusal exists only in the HOST log. Worse, it is STICKY — vrend
  latches the context error, so a later VALID stream on that ctx moves no
  pixels (`SENTINEL`) while the identical stream on a fresh ctx works
  (`GREEN`). One malformed submit kills the context for its whole life
  while every fence keeps reporting success; the `transfer_from` is still
  accepted and still retires, it just delivers stale data, so even the
  readback path lies. **Blast radius is confined to the submitting ctx**
  (the second connection's ctx was unaffected throughout), so this is a
  robustness + observability defect, not a privilege escalation — an
  unprivileged client can only self-DoS.
  This is the other half of the hazard the winsys already names above
  ("the virgl driver ignores `submit_cmd`'s return, so a dropped command
  buffer must never be silent"): the latch there covers SEAM-level
  failures, which a host refusal is not — the submit write returns its
  byte count normally. It blocks Warp-C (`GPU-DESIGN.md` §4.5): a
  compositor submitting a per-frame blit stream would freeze the screen
  forever on one rejection while reporting composed frames.
  Do NOT infer a hang from a refusal — the original filing said the fence
  never retires, which came from a 200-iteration poll against a 30 s
  timeout and described the probe's budget, not the seam.
  **DETECTABLE since C-0d** (`GPU-DESIGN.md` §4.5.4b): the defect itself is
  unchanged — the seam still cannot make a refused submit *fail* — but it is
  now **observable**, via `stream-rejected` on the ctx ctl (below). Keep
  that field DISTINCT from `poisoned`: different causes (the host refused
  our commands vs. a chain that never retired), and collapsing them is how
  this defect was missed for as long as it was.
- **#170**: the graceful half is closed — `kobj_pci_quiesce` runs from
  `proc_quiesce_owned_devices`, so a PCI-transport driver stops decoding
  and mastering before the exit path frees its DMA pages (round-1 F8: the
  sweep knew only virtio-MMIO, and a BAR-decoded device is invisible to
  it). The task stays open for the residual ordering in the fallback
  `proc_free` path.
- **Client-visible limits**: 8 contexts total, one per connection, 1024 BOs
  each (16 → 128 at Warp-3: st/mesa mints ~8 `hw_res` before the first
  draw; 128 → 1024 at #204: GLQuake's map load holds more than 128
  textures live, so creates past the cap streamed `GL_OUT_OF_MEMORY`
  [#213] — the `bo-peak` census is the witness the width is sized
  against. The round-6 F1 graveyard cap is width-independent by
  construction — the discriminating churn probe reads the width from
  the global ctl `bo-cap` field instead of hardcoding it, so a cap
  change cannot silently un-discriminate it. CAVEAT (tracked): on the
  2026-08-12 runs the churn's refusal fired at attempt ~237 via the
  `dma-create` arm — each graveyard leak pins its dma handle by design,
  so tapestryd's handle table exhausts before `leaked_count` reaches the
  cap, and the R6-F1 count-cap is currently witnessed by the WRONG
  mechanism. The rows are heap
  allocations at ctx mint since #204; an OOM fails the mint clean),
  64 MiB live+leaked backing
  per context, 4 concurrent `/srv/warp` connections (a 5th blocks until
  one frees), 16 fenced chains in flight process-wide (4 → 16 at #204;
  per-ctx share 8, advertised as ctl `fence-lane`).
- One Twrite = one submission: the effective stream bound is the 9P iounit
  (msize 32 KiB − overhead), not the 36 KiB slot (`FREQ_LEN`). The Warp-3 winsys splits
  its command streams at CCMD boundaries to fit; the Loom-carried bulk
  path (§4.1) remains the successor for a single command wider than the
  bound, which today latches loudly.
- The fence file reports coalesced ids; a client that needs per-submission
  granularity tracks its own issue order (FIFO within the ring).
- Multiple fids parked on one ctx's fence file race for records
  (first-parked wins); one client per ctx is the intended shape.
- `rings <n>` and `capset <n>` are recorded, not yet negotiated to the
  device (F_CONTEXT_INIT / per-ring fencing are the Venus deltas).
- **#195**: host-side pixel capture is structurally unavailable on the GL
  host — under egl-headless + virtio-gpu-gl every scanout is
  host-GL-backed and QMP screendump reports "no surface" (QEMU 10.0.11,
  so not a version gap). The quake gate's pixel legs run as an ANNOUNCED
  best-effort there, and the direct scanout's display-orientation has no
  host-side witness: the no-flip property rests on the gallium top-down
  transfer contract (the shipping llvmpipe `y_up=FALSE` straight copy)
  plus the Linux virgl desktop anchor. Guest-side TRANSFER_FROM readback
  cannot substitute — it reads the resource, not the display. The local
  HVF 2D box keeps full pixel coverage (the ls-gfx family).
- **#196**: first-contact GLQuake throughput on virgl is ~3 fps aggregate
  at 1280×800. The original "~20–25x under the 192.8 anchor" framing was
  a misquote — 192.8 is macOS/HVF and does not transfer (the Warp-1
  status row's own warning); the same-host llvmpipe band is 2.4–5.9 fps
  at 640×480, so the virgl figure sits INSIDE the software band at 3.3×
  the pixels. The open question is stall-vs-compute: pegged guest CPU
  during the demo = TCG-amplified encode (no stall); idle = a real
  per-frame wait (fence pump / sync round-trips). `tools/warp-host.sh
  decomp gl|2d` measures both arms unpaced with per-demo qemu CPU
  attribution; the mid-run QMP TimeoutError (starved QEMU main loop)
  remains an unattributed corroborating observation. Answered — see
  Performance characteristics; the residue is #210.
- **#210**: unpaced early-zoom Direct-GL wedges silently during the map
  load — 0 progress at the ~26% resident-idle CPU floor, no client
  latch, no error output. GL-lane-specific (the identical leg on the 2D
  device runs clean); the paced mid-demo-zoom 4c run also ran. The
  autopsy probe (`tools/warp-host.sh wedge` →
  `tools/warp/glq-wedge-probe.exp`) isolates the pacing variable and
  captures the parked chain's kstacks via the I-39 debug surface from a
  background-job prompt.
- **#198**: at the post-timedemo transition the game's GL context breaks
  and Mesa spams `GL_INVALID_OPERATION (invalid call)` per frame —
  virgl-specific (the llvmpipe sibling idles clean); instrument-first plan
  in the task.
- **The launcher is the ^C seam**: pouch has no execve until LINEAGE L-6,
  so the ramfs face interposes via posix_spawn — and the serial console
  posts `interrupt` to the OWNER Proc alone (#197: pgrp parity owed),
  while a CAUGHT note never interrupts a blocking syscall (#199: POSIX
  EINTR parity owed; ut survives by polling its note fd). The launcher
  therefore forwards SIGINT/SIGTERM to its child and reaps via
  WNOHANG + 50 ms usleep — the syscall cadence is the note-delivery
  point. Both kernel-parity items are tracked tasks; when either lands,
  the launcher's shape simplifies.

## quarry — the renderer launcher and demo bench

`usr/quarry` (native libthyla-rs on Kaua) is the user-facing face of this
seam: one place to pick a renderer and one place to compare them. Five
rows, three live today — `sw` (the pure software rasterizer,
`/bin/tyr-quake`), `llvmpipe` (software GL via `GALLIUM_DRIVER=llvmpipe`),
`hw-gl` (this seam, `GALLIUM_DRIVER=virpipe`) — and two reserved for
Warp-6 (`lavapipe`, `hw-vk`), which report `awaits Warp-6 (Venus)` rather
than pretending to be absent for an unknown reason.

Probing is what makes a row `ready`, and the `hw-gl` probe is the one with
a footgun worth stating: it reads the warp `ctl`, and `/srv/warp` is a
SERVICE POST, so a single-shot walk of `/srv/warp/ctl` does not compose —
the open of the service root IS the connect. The probe therefore opens the
root, then opens `ctl` RELATIVE to it, which is the shape every working
consumer of a posted service uses.

Driver selection goes through `/env` (remove-then-create, restored on
exit) because `setenv` does not survive `exec` — the same reason the
tyr-glquake launcher writes `/env/GALLIUM_DRIVER` rather than setting it.

The CLI (`quarry list` / `quarry bench [demo] [leg...]`) is the automatable
face. Each leg spawns the engine with `-condebug` and stdio INHERITED, then
polls the engine's own console log (`/quake/id1/qconsole.log`) for tyrquake's
`N frames S seconds F fps` line plus `GL_RENDERER` and a Mesa-error count,
and is hang-killed at `BENCH_DEADLINE_MS` (600 s) via `/proc/<pid>/ctl`. A
leg that produces no fps line reports as such rather than as a zero.

It reads a FILE rather than a pipe because `+timedemo` does not quit the
engine: under a pipe the fps line sits in the child's full-buffered stdout
awaiting an exit that never comes, and a drain loop calling `read()` on an
unready fd blocks in the callee where the outer deadline can never be
evaluated (#231 -- a deadline a callee can outlive is not a deadline). The
existing `glq-bench.exp` lane only works because it reads a TTY, which is
line-buffered. `-condebug` routes `Con_Printf` through `Sys_DebugLog`, whose
writes are bare `open`/`write`/`close` with no stdio buffer in the path.

An explicit leg list SELECTS, ORDERS and SIZES the legs, all three
load-bearing for attribution rather than convenience. Order: with the default
order the GL leg is also the LAST leg, so "the GL client fails" and "the last
leg fails" are the same observation. Size: a leg is `key` or `key@WxH`
(`hw-gl@1280x800`), which reaches the engine as `-width`/`-height` — honoured
at any size because `-window` makes `VID_GetCmdlineMode` write the request
straight into `vid_windowed_mode` instead of searching the modelist and
`Sys_Error`ing on a miss.

Sizing exists for the #215 resolution sweep, and running it in ONE boot is the
point: it holds the boot constant, and repeating the first resolution as the
last leg MEASURES within-boot drift (#168) rather than assuming its absence.
`QUARRY_LEGS` drives the list from the host through `quarry-bench.exp`, whose
expect window grows per leg — a bound below the honest runtime reports a
healthy leg as hung, which is exactly how #232 produced a false wedge.

Every leg WITNESSES its own resolution: it passes `+vid_describecurrentmode`
and parses the engine's reply (`" 640 x  480 windowed"`) out of the same log
the fps line lands in, printing `mode-witness WxH` and naming a `MISMATCH`
against the request. This is not decoration — a per-submit-bound renderer and
a `-width` that never took effect BOTH produce a flat fps curve, so a sweep
recording only what it requested cannot tell its measurement from its own bug.
Legs that request nothing report it too, which is the only actual statement of
the engine's default: **640x480**, seeded by `sdl_common.c`, NOT the 800x600
`vid_width`/`vid_height` cvars (`-window` makes `VID_GetCmdlineMode` answer
before those are ever consulted). The command is registered at `host.c:803`
and `VID_Init` runs at `:944`, while command-line `+commands` execute from
`quake.rc`'s `stuffcmds` at `:962` — so the mode is real by the time it is
described. A software-renderer leg pair at two sizes is the POSITIVE CONTROL
for any sweep: a fill-bound rasterizer must lose fps as pixels grow, so it
proves the size knob bites at all.

Each leg brackets its kill with
`spawned` / `kill-begin` / `kill-end` / `reaped` markers, and prints
`log-at-end present=/bytes=/last_line=` when it ends WITHOUT an fps line --
the poll loop is otherwise silent, so a stall inside it leaves no pid and no
last-known step.

That log dependency has a sharp edge worth knowing (#232). Upstream
`Sys_DebugLog` acquires a FRESH descriptor for every console line and checks
none of `open`/`write`/`close`, and it builds the path as
`va("%s/qconsole.log", com_gamedir)` — where `com_gamedir` is EMPTY for most
of a run, populated only briefly around `COM_AddGameDirectory`. So the path
alternates between a usable `<gamedir>/qconsole.log` and a bare
`/qconsole.log` at the filesystem ROOT, which an unprivileged process cannot
create (EACCES). Every line emitted outside that brief window was discarded
in silence: the log stopped at 47 bytes — one line — while stdout carried the
entire 27-second run, and the bench read a healthy timedemo as an infinite
hang.

Fixed by `usr/ports/tyrquake/patches/0002-tyrquake-condebug-fd.patch`:
acquire once and PIN it, deliberately not re-opening when the caller's path
changes (doing so re-breaks it identically — the first empty-gamedir call
after a good open would close a working descriptor and never get another),
and report per-DISTINCT-PATH rather than one-shot, since a single latch is
spent by the harmless early failure and then hides the one that matters.

Two traps this cost, worth inheriting. The mechanism was first misdiagnosed
as fd exhaustion, reasoned from a GL-vs-llvmpipe differential WITHOUT ever
obtaining an errno; the first errno named a different failure and a different
path — get the errno before naming the resource. And `/clade/bin/tyr-glquake`
is POOL-resident, so a fix reaches the guest only after
`tools/build.sh stage-clade` runs BEFORE the pool bake:
`THYLACINE_BAKE_CLADE=1` puts the EXISTING stage into the pool, it does not
refresh it. Content-check the stage, not the build output.

Gate: `tools/warp/quarry-bench.exp` (via
`tools/warp-host.sh quarry-bench`) asserts `hw-gl ready` on a virgl boot
and then the bench table — so a regression that silently drops the seam
back to software fails the gate rather than quietly reporting llvmpipe
numbers under a hardware label. The host target echoes every leg's
mode-witness and warns when any leg did not run at the size it was given,
because an unattributed fps column is worse than a missing one. The
interactive TUI is covered by
`tools/interactive/quarry.exp`; note it asserts only
configuration-invariant text, because Kaua's cell-diff renderer fragments
changed strings across updates (the `prowl.exp` precedent — never assert
on a diffed status line).

## Warp-6 V-0 -- the Venus gating probe (as-built 2026-08-18)

The Venus arc opens with a probe because `GPU-DESIGN.md` §9.1 makes it binding:
everything up to "the winsys compiles" can be done locally, **nothing can be
run** locally, and that must be settled *before* code. V-0 answers "is Venus
reachable at all?" for a host, and nothing structural lands until it passes.

### What the gate asserts

`tools/warp-host.sh venus` boots the remote GL host **twice**, differing in the
device declaration alone:

| leg | device | expected |
|---|---|---|
| control | `virtio-gpu-gl-pci` | capsets `id=1`, `id=2`; **no `id=4`** |
| test | `virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M` | additionally **`id=4`** (VENUS, `max_version=0`, `max_size=160`) |

VERIFIED requires **both** legs to boot **and** the discrimination to hold in
**both directions**. The control leg is not a courtesy: a one-directional check
("the test leg saw `id=4`") is satisfied by a host that advertises the capset
unconditionally, and by a guest printing a line it did not derive from the
device. It is the `composed` verb's shape (§Warp-C C-2b), for the same reason.

No guest code was needed. `probe_capsets` (`usr/tapestryd/src/gpu.rs`) already
enumerates to `GPU_CAPSET_ENUM_MAX = 8` and emits one
`tapestryd: gpu capset[N] id=.. max_version=.. max_size=..` line per index;
V-0 only had to read them with a control beside them.

### venus is not an independent switch

QEMU refuses `venus=on` and `venus=on,blob=on` alike, and **names the
requirement**:

```
qemu-system-aarch64: -device virtio-gpu-gl-pci,venus=on: venus requires enabled blob and hostmem options
```

Only `venus=on,blob=on,hostmem=<size>` realises (`max_hostmem` defaults to
256 MiB). This is a **realise failure, not a degradation** -- a caller that
declares less does not get "GL without Venus", it gets no device. Callers must
not read that outcome as a negative Venus result.

### The render server: named, absent, and not fatal to this rung

`libvirglrenderer.so.1.9.0` on the Debian 13 hosts carries the Venus
implementation (`VK_MESA_venus_protocol`, `vkr_ring_thread`,
`vkr_dispatch_vkWaitVirtqueueSeqnoMESA`) and names
`/usr/libexec/virgl_render_server` as `RENDER_SERVER_EXEC_PATH`. **That binary is
in no Debian package** -- `apt-cache search virgl` offers `libvirglrenderer1`,
`-dev`, and `virgl-server`, and the last is the unrelated *vtest* server.
`GPU-DESIGN.md` §9.2 describes the render server as Venus-only-by-construction,
which reads as "no server, no Venus".

The capset is advertised regardless, so venus initialises **in-process** far
enough to answer a capset query. **This does not prove a Venus context can be
created** -- that is V-0b (`CTX_CREATE` with `capset_id=4`), which settles the
question empirically rather than by inference from either direction.

Instrument note: `nm -D --defined-only` reports **zero** venus/vkr symbols
because they are internal to the library. An export census is the wrong
instrument here and would have read as absence.

### Hosts

| host | Vulkan ICD | verified to | role |
|---|---|---|---|
| **thyla-pi** (RPi 400, KVM) | `V3D 4.2.14.0` / V3DV Mesa -- real hardware | **gate VERIFIED** | certification |
| **thyla-gl** (Parallels on the Mac, TCG) | `llvmpipe` / lavapipe -- software | **gate VERIFIED** | fast iteration |

Both hosts pass, and they return **byte-identical feature words**, so the arc has
a quick loop and a silicon loop that agree.

### The device's feature offer (measured; it used to be discarded)

`dev_feat_lo` was read during negotiation, used for one bit (VIRGL), and thrown
away -- so "does this host offer `CONTEXT_INIT`?" had no answer short of a new
build, on a value already in a register. It is now a per-boot line:

| device | measured on | `lo` | virgl | edid | uuid | blob | ctxinit |
|---|---|---|---|---|---|---|---|
| `virtio-gpu-pci` (the default 2D dev device) | the Mac, HVF | `0x30000002` | 0 | 1 | 0 | 0 | 0 |
| `virtio-gpu-gl-pci` | thyla-pi **and** thyla-gl | `0x30000013` | 1 | 1 | 0 | 0 | **1** |
| `virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M` | thyla-pi **and** thyla-gl | `0x3000001b` | 1 | 1 | 0 | **1** | **1** |

The two GL rows are byte-identical across both hosts. The 2D row is the Mac's
dev device and has no GL-host twin -- it is the control that makes the GL rows
mean something, not a third host's agreement. (`0x30000000` = `VIRTIO_RING_F_INDIRECT_DESC`
+ `VIRTIO_RING_F_EVENT_IDX`; `hi=0x00000101` = `VIRTIO_F_VERSION_1` +
`VIRTIO_F_RING_RESET`.)

Two consequences the arc plans around:

- **`VIRTIO_GPU_F_CONTEXT_INIT` is offered on a plain `-gl` device**, with no
  venus and no blob. `ctx_create` writes `context_init = 0` with the comment
  "F_CONTEXT_INIT not negotiated", and the device honours that field ONLY when
  the feature is negotiated -- which this driver never offers back. So **V-0b is
  a feature-bit change, not a field change**, and its naive form is a *false
  pass*: writing 4 into an ignored field returns `RESP_OK_NODATA` and yields an
  implicitly-virgl context while reporting success.
- **`VIRTIO_GPU_F_RESOURCE_BLOB` appears only with `blob=on`.** The default dev
  device offers neither (`virgl=0`), so **V-1's blob work cannot be exercised on
  the local dev loop at all** -- the same shape as #166's inert-hostmem-under-HVF
  constraint, and the practical reason promoting thyla-gl mattered.

### The stale-artifact hang (recorded; fixed is not the same as explained)

thyla-gl's FIRST run used its own Aug-12 artifacts, and tapestryd **hung** under
the venus declaration -- `warden: tapestryd gave no readiness/exit signal ->
terminating`, three restarts, then `gave up`. A hang (`Readiness::Timeout`:
neither signalled nor exited), not a crash, and the control leg on the same host
and build came up cleanly.

Two hypotheses died by measurement:

- *"the Aug-12 build predates #166's oversized-BAR skip"* -- refuted:
  `git show 534f3869:usr/lib/libthyla-rs/src/hardware.rs` carries the identical
  `if bar.size > PCI_BAR_VA_STRIDE { continue; }` and the same #166 comment;
  `git log -S` dates that code to 2026-06-15.
- *"lavapipe is slow, so venus init stalls the controlq"* -- weakened:
  `vulkaninfo --summary` returns in **248 ms** there, and `SUBMIT_DEADLINE_MS =
  500` already bounds the controlq wait, so the driver does not block
  indefinitely on a device response.

The current build came up clean on that same host and declaration, so the
attribution is the **stale artifacts, not the host**. One sample each way across
two builds means the old hang is *unexplained* rather than *explained*; there is
nothing to fix in the tree, and that distinction is recorded rather than
smoothed over.

### Testing the verdict without booting

The verdict is its own verb -- `tools/warp-host.sh venus-verdict <ctl> <tst>` --
so `tools/test-venus-verdict.sh` can drive **the real implementation** against
crafted logs. Two boots at ~220 s each make this gate the least affordable thing
in the tree to test by running it, and #245 is the standing lesson that a checker
reachable only by hand rots.

The suite is **one positive control plus one one-variable sabotage per failure
arm**, and it is described that way rather than by a count -- a count in prose is
a status field whose flip is nobody's step, and this sentence said "Five cases"
within an hour of the suite reaching eight. The arms, by class:

- the **clean pair VERIFIES** -- the positive control, without which every
  negative case is satisfied by a verdict that always fails;
- the control leg **also** sees `id=4` (then the declaration is not what
  produces it);
- the test leg sees **no** `id=4`;
- **either** leg did not boot (a leg with no verdict is distinct from a leg that
  booted and disagreed);
- the control leg enumerated **no capsets at all** -- 2D fallback, which lacks
  `id=4` trivially, so "venus absent" would be read off a control that measured
  nothing;
- the control leg lost **only** the baseline `id=1`;
- a capset numbered **40** must not satisfy the `id=4` check (both id checks
  anchor on a trailing space for exactly this).

Run it with `make test-venus-verdict`; the script prints its own pass count.

## Warp-6 V-0b -- a Venus context creates (as-built `bf448929`)

V-0 proved the host *advertises* capset id=4. V-0b answers the question that
matters: can a Venus *context* be created? It was open because
`/usr/libexec/virgl_render_server` is absent from every Debian package and
GPU-DESIGN 9.2 calls the render server Venus-only-by-construction, so
advertisement only proved venus init reached capset *reporting*.

### The measurement (thyla-pi, KVM, real V3D)

| leg | ctx-capset result |
|---|---|
| `venus=on` | `id=2 CREATED` (virgl control) + **`id=4 CREATED`** |
| no venus | `id=2 CREATED` + `id=4 skipped (capset not enumerated)` |

So **the absent render server does not block context creation** —
virglrenderer's in-process venus init handles it. The `id=2` create is the
positive control on both legs; `id=4` discriminates on venus, and the no-venus
`skipped` (not `CREATED`) is the negative control that closes the false pass.

### Why it is a feature-bit change, not a field change

`ctx_create` wrote `context_init = 0` under the comment "F_CONTEXT_INIT not
negotiated", and the device honours that field **only** when the feature is
negotiated — which the driver never offered back. So writing a capset into it
without negotiating the feature yields `RESP_OK_NODATA` over an implicitly-virgl
context: a false pass. V-0b therefore:

- negotiates `VIRTIO_GPU_F_CONTEXT_INIT` (LOW bit 4) when offered, on the same
  accept-if-offered footing as virgl, carried as `Gpu.ctxinit`. It is **not** a
  second gate on "is 3D available" — that stays `virgl` alone; the two are
  orthogonal and a host could offer either without the other;
- adds `ctx_create_capset(ctx, capset, name)` writing the capset into
  `context_init` bits 0-7 (`ctx_create` is now `ctx_create_capset(.., 0, ..)`,
  every caller byte-identical), with `debug_assert!(capset == 0 || ctxinit)`;
- at the tail of `probe_capsets`, creates+destroys a capset-2 control and (if
  capset 4 was enumerated) a capset-4 context. Distinct ctx ids **200 / 201** so
  a failed destroy cannot make the next create collide on a duplicate id and
  read as a Venus refusal; the ids sit above the client `dev_ctx` range (slot+1)
  and below `COMPOSITOR_CTX` (0x100) and `CONV_PROBE_CTX_BASE` (0x101). Same
  failure disposition as the rest of the function (audit W1 F1): an
  engine-healthy refusal is a log line, only a real engine death propagates; a
  skip (feature absent / capset not enumerated) is logged, never silent.

### The gate

`warp-host.sh venus` asserts the ctx-capset discrimination in **both**
directions — `id=4 CREATED` with venus, absent without; the `id=2` control
required on both legs so "control lacks id=4 CREATED" is not satisfied by a leg
where creation was broken outright. `test-venus-verdict` grew four arms (8 →
12), all discriminating without a boot.

### Not covered / deferred

The client-facing capset plumbing is **V-3**: `WarpCtx.capset` exists at the
seam with a `capset <id>` ctl verb but nothing reads it. V-0b is driver-internal
— the probe creates+destroys before any client exists and confers nothing. When
V-3 makes the field live, rejecting a capset the device never enumerated belongs
in that change (an unvalidated client `u32` would otherwise reach `CTX_CREATE`).

## Warp-6 V-1 -- a guest blob creates (as-built `3a98f902` + `e49dac52`)

V-0b proved a Venus *context* is creatable; V-1 proves the next thing a Venus
driver needs: a **guest blob**. Venus carries its command ring as a
`RESOURCE_CREATE_BLOB` guest-memory blob (GPU-DESIGN section 2.4 -- the ring's
head/tail/status cachelines are guest pages the host also reads), so the blob
object model is Warp-6's real prerequisite, sequenced here by the F2 vote.

### The measurement (thyla-pi, KVM, real V3D 4.2)

| leg | blob result |
|---|---|
| `venus=on` (`blob=1`) | **`blob-create guest CREATED`** |
| no venus (`blob=0`) | `blob-create skipped (F_RESOURCE_BLOB not offered)` |

So **a guest-memory blob is accepted on real V3D** -- `RESOURCE_CREATE_BLOB`
(`blob_mem = GUEST`, one guest-page `mem_entry`, `blob_flags = 0`) returns
`RESP_OK_NODATA`. The create discriminates on the negotiated feature: the
control leg does not offer `F_RESOURCE_BLOB`, so the probe self-skips (a
positive `skipped` line, not an absent one). The venus leg also boots fully
clean with the feature negotiated, so negotiating blob does not disturb the
compositor path.

### Guest blob, not host3d -- and why that is the whole V-1 scope

A guest blob's storage **is** its guest `mem_entry` pages: the host registers a
resource referencing them, no host allocation and no hostmem BAR. That is
exactly Venus's ring. The host3d blob (host-allocated storage the guest reaches
through the hostmem window via `RESOURCE_MAP_BLOB`, 0x0208) is the **V-2** delta
and is deliberately not here -- it is also why the default 2D dev device
(`virgl=0`, no `F_RESOURCE_BLOB`) cannot exercise V-1 at all; every iteration is
a GL-host boot.

V-1 is the create path, not the full object model: the probe creates+unrefs one
device-global guest blob (id `0x2b`, below the server's first minted resource id
`SCREEN_RES + 1`, and unref'd before any client exists -- the ctx-capset probe's
timing guarantee, plus a compile-time guard on the numeric margin). The
client-facing blob-BO type, mapping, and the coherent ring arrive with **V-3**.

### The feature negotiation and the probe

- negotiates `VIRTIO_GPU_F_RESOURCE_BLOB` (LOW bit 3) when offered, on the same
  accept-if-offered footing as virgl and ctxinit, carried as `Gpu.blob`. A blob
  command is illegal on the wire without it, so this both records the offer and
  gates the probe. `init_device`'s feature bools now ride a named `DevInit`
  struct rather than a positional tuple (the shape that let V-0b's `ctxinit` go
  briefly unreturned).
- adds `resource_create_blob(id, blob_mem, blob_flags, pa, len)` -- the wire
  command `RESOURCE_CREATE_BLOB` (`0x010c`, still the 2D group; `GET_EDID` and
  `RESOURCE_ASSIGN_UUID` sit unused between it and `GET_CAPSET`), a single guest
  `mem_entry`, `RESP_OK_NODATA`.
- `blob_probe(backing_va)` at the tail of `probe_capsets` (virgl-gated, so a 2D
  boot pays nothing): a dedicated one-page DMA backs the blob -- its own buffer,
  not the ring or fenced lane, so there is no question of residue over a live
  transport region -- created then unref'd. Same failure disposition as the rest
  of the function: an engine-healthy refusal is a log line, only a real engine
  death propagates; a skip (feature absent) is logged, never silent. On a
  *failed* unref (engine alive) the backing is **leaked, not unmapped** -- the
  host may still reference the pages, and one leaked page at init beats unmapping
  referenced memory (self-audit SF1).

### The gate

`warp-host.sh venus` grew a V-1 arm: `blob-create guest CREATED` required on the
venus leg, the positive `skipped` line required on the control (not merely the
absence of `CREATED`, which a probe that never ran would also satisfy), and no
`CREATED` on the control (a blob on a wire that never negotiated the feature
would mean the test leg's create proves nothing). `test-venus-verdict` grew
three arms (13 -> 16), all discriminating without a boot.

### The round (CLEAN, 0 P0 / 0 P1 / 0 P2 / 3 P3)

Opus 4.8 fallback (Fable out of credits; tier noted, a finished fallback owes no
Fable re-run). Two fixes it verified sound, both defense-in-depth for V-3:

- **The `!self.blob` runtime guard** on `resource_create_blob` -- a blob command
  is illegal on the wire without `F_RESOURCE_BLOB`, and this refuses it in the
  function rather than trusting the caller. `blob_probe` also checks `self.blob`,
  so the guard is redundant for the V-1 probe and load-bearing for V-3 (the
  V-0b F2 lesson: a caller-side-only guard is a no-op the moment a future caller
  forgets it). It is unconditional (unlike the sibling `ctx_create_capset`'s
  `capset != 0` exemption) because there is no feature-free blob command.
- **F1**: SF1 leaks the backing on a failed unref (host may still reference the
  pages), but the sibling dead-engine create-Err branch DROPPED it -- and a
  deadline-dead create is already published (the doorbell rings before the wait),
  so the device may equally hold the PA. Fixed to `forget` on both branches; the
  two must agree or V-3 (where transfers exist) reuses the wrong one.

Two forward notes, owed at their chunks, not V-1:

- **F2 -> V-3**: `resource_create_blob` trusts caller-supplied `pa`/`len`. For
  V-1 the caller is `blob_probe` with a driver-owned page; when V-3's
  client-influenced caller reaches here, `pa`/`len` must be validated to
  reference DMA the *client* owns (an I-45/I-32 boundary).
- **F3 -> V-2**: the gate greps the literal `guest` in the log line, not the
  `blob_mem` actually sent. Sound while V-1 hardcodes GUEST; when V-2 adds
  HOST3D, assert the mem-type from evidence or a mem-type regression passes
  silently.

### Not covered / deferred

Host3d blobs + the hostmem-BAR mapping are **V-2** (with F3's mem-type assertion);
the coherent command ring, the client blob-BO type, `vn_renderer_thylacine`, and
F2's client `pa`/`len` validation are **V-3**. V-1 is driver-internal -- the
probe creates+unrefs before any client exists and confers nothing.

## Warp-6 V-2 -- host-visible BAR mapping (as-built, uncommitted)

V-2 maps a subrange of a PCI hostmem BAR (Venus `HOST_VISIBLE` memory) into a
client VA. GPU-DESIGN 6.2 / 6.2.1. Unlike V-0/V-0b/V-1 (tapestryd device
commands, no kernel change), V-2 is a kernel memory-authority path -- I-45 +
I-32 + I-37.

**The ABI.** `SYS_BURROW_FROM_HOSTMEM(pci_handle, shmid, offset, length,
cache_policy) -> mapped VA / -1` (syscall 107). Mints a `BURROW_TYPE_HOSTMEM`
Burrow over `bars[shm.bar].pa + shm.offset + offset` (length bytes), gated by
owning the `pci_handle` claim (KOBJ_PCI is I-5 non-transferable, so holding the
handle IS the authority -- no CAP_HW_CREATE, unlike the *_CREATE mints), and maps
it RW into the caller's burrow-attach window at the host-dictated cache
attribute. The returned VA feeds the existing `SYS_WEFT_SHARE` -> client
`SYS_WEFT_MAP` -> `burrow_share_into` (the audited share/budget/reaper path).
`enum t_cache_policy` {CACHED=0->NORMAL_WB, WC=1->NORMAL_NC, UNCACHED=2->NORMAL_NC}.

**The mmu attr-index widening.** The fault path's `bool device_memory` (WB-or-
Device only) is widened to a MAIR index: `mmu_install_user_pte_attr(...,u32
mair_idx)` is the general form, and `mmu_install_user_pte(...bool)` is a
semantics-preserving wrapper (false->WB, true->Device) so none of the ~13 bool
callers hit the `false==0==MAIR_IDX_DEVICE` inversion. `make_user_pte_l3` takes
the index, range-checks it (`<= MAIR_IDX_NORMAL_WT`), and confines EXEC to
NORMAL_WB (W^X/I-12; stricter than the old device-only reject). **NORMAL_NC
already existed in the MAIR since P1-C** -- V-2 plumbs it and adds no byte
(6.2.1's "add a MAIR byte" premise was stale; corrected).

**The Burrow + share.** `BURROW_TYPE_HOSTMEM`: pages==NULL, pa = the BAR
subrange, pins a `KObj_PCI` ref, `hostmem_mair` = the create-time index;
`burrow_create_hostmem` mirrors `burrow_create_mmio`, with three type arms (the
fault switch, `burrow_free_internal`, `burrow_acquire_mapping`). Share admission
is widened at BOTH `burrow_share_into` and `sys_weft_share_for_proc` in LOCKSTEP
(the Warp-2b half-widen bug); `weft_claimed_kind` classifies HOSTMEM map-only
(entries==0) as `WEFT_BIND_HOSTMEM`, a fourth map-only kind. I-45: a hostmem BAR
is device-PASSIVE shared memory (cfg_type=8), not a command/register surface, so
the client's cacheable/NC RW mapping conveys zero hardware authority.

**F1 (server-death liveness).** On the owning server's death,
`proc_quiesce_owned_devices` does a DMA-ONLY quiesce (`kobj_pci_quiesce_dma_only`:
BUS_MASTER cleared, MEM_SPACE KEPT) for a claim with `hostmem_burrows > 0`, so a
client's live mapping never observes a MEM-decode-disabled BAR; MEM_SPACE clears
at the last `kobj_pci_unref` (`pci_release_bars_and_claim`), after the mapping is
gone. `hostmem_burrows` is atomic, bumped in `burrow_create_hostmem`, dropped in
the HOSTMEM free arm.

**Bounds (I-45).** `hostmem_resolve_subrange` (the pure, unit-tested core) bounds
the subrange within the named window (`offset <= shm.length`, `length <=
shm.length - offset`; discovery pins `shm.offset + shm.length <= bar.size`), so
`base_pa + length` never escapes the BAR. The type is create-immutable + settable
only by the ownership-gated mint. **I-32**: the mint's VMA is bounded by
PROC_VMA_MAX; the client's shared-in pages by `shared_map_pages`; BAR pages are
not RAM, so no `page_budget` charge.

**Audit.** Opus holotype round: 0 P0 / 1 P1 (F1, server-death quiesce) / 1 P2
(F2, no handler-bounds test) / 3 P3 (F3 weft-lockstep test, F4 shm/regions
disjointness, F5 cache-policy footgun doc). F1 fixed (the DMA-only quiesce), F2
fixed (the extracted `hostmem_resolve_subrange` + its test); F3/F4/F5 tracked
P3. Re-audit of the fixes: *(pending)*. Tests: `weft.hostmem_share`,
`weft.hostmem_resolve`, `pgtable.install_user_pte_attr_index`; suite 1431/1431.

**Status.** As-built, uncommitted. The weft client-delivery is wired but
exercised only by unit tests -- V-3 (`vn_renderer`) drives it E2E on a real
device. The libthyla-rs ABI mirror (107) + the GL regression boot ride the merge.

## Tests

Kernel: `pci.walk_caps_shm` (6 discriminating vectors incl. the
`length_hi` verdict-flip + the overflow-wrap sum) and
`weft.gpu_bo_share_and_claim` (mint bits, envelope, BOTH admission gates,
kind decision, budget charge/uncharge, reaper registration) — both in the
1358-test suite; the four weft buggy cfgs still fire. Userspace: the local
2D boots prove the degradation path (`Warp PROBE OK ... virgl 0`, submit
E_OPNOTSUPP, no flane allocation) and `ls-gfx`/`ls-gfx-play` exercise the
rewritten controlq under real present pressure; `/warp-prove` on the GL
host is the seam's PASS-path gate (`tools/warp-host.sh prove`) — nine legs
since #218: the clean path, the poisoned/graveyard/vindication machine, the
two-client cross-properties, and the corpse-reclaim leg (cap+1 REFUSED
create3ds per family [wbo_create-validated + pre-parse], the mint must
survive attempt cap+1, the repeat-create3d-on-a-built-bo guard; A/B-proven
on thyla-pi 2026-08-12 — the pre-fix server fails it at exactly
`size-align attempt 1025 (cap 1024)`) — and
`/clade/bin/virgl-prove` is the Warp-3 stack gate — the full Mesa virgl
driver through the winsys to a rendered triangle (`tools/warp-host.sh
tri`, both #186-anchored verdict lines required).

`warp-prove reject` (`tools/warp-host.sh reject`,
`tools/warp/warp-reject.exp`) is the #240 OBSERVATION lane, not a gate: it
measures what the seam reports for a stream the host refuses, and prints
data rather than a verdict. Its own two controls are what make it
readable — a class-matched valid submit on a second ctx (so a lost
submit-fence could be told from a retired one; a `transfer_from` control
could not have, #212), and counters read BEFORE the submits (so the ctx
build's own fenced work is not credited to the stream). Kept out of the
Warp-2 battery because it spends 45 s waiting out a 30 s timeout and that
gate's value is being cheap enough to run every time. The scenario waits
on `C0-REJECT DONE` while the host-side verdict greps `C0-REJECT ANSWER=`
— inverted deliberately: `lc_expect` writes its own pattern into its
timeout text, so a verdict keyed on the awaited token would match a run
that never happened (#186).

The Warp-4 present-integration gate is `tools/warp-host.sh quake`
(`tools/warp/glq-virgl.exp`): GLQuake launched through the RAMFS bare
name — the launcher's production auto-detect (the two-step raw
`/srv/warp` attach probe → the `/env/GALLIUM_DRIVER=virpipe` write; a
one-shot pouch open cannot cross the srv post, probed not assumed) — with
BOTH present arms in one run. The letterboxed launch (composed-entry say
line) puts the early demo through the Composed-GL sync-transfer arm; the
default Super+F zoom chord typed over QMP (the game's pane holds focus
from `host()`) makes the display-sized surface the sole visible leaf, and
the `scanout direct N GL res R` switch line is the SET_SCANOUT(bo)
evidence; the demo's remainder runs on the Direct flush-only arm; the
timedemo figure is REPORTED, not gated (the ls-gfx-glquake rule; the
honest same-host reference is the Warp-1 llvmpipe band, 2.4–5.9 fps at
640×480 — the macOS/HVF 192.8 does not transfer); and the ^C teardown leg drives the
launcher's interrupt forward → SDL_QUIT → surface retire → the
BO-eviction + console-restore chain (tick-driven — the restore is a
multi-present chain, each hop needing console output). Verdict = the
`GLQ-VIRGL PASS` + `LS-CI PASS: glq-virgl:` conjunction, discrimination-
proven against synthetic pass/fail/swapped logs including the poisoned
half-match. The battery's glsrc 2D reject legs (off idempotent / unknown
ctx E_NOENT / junk E_INVAL — a warp ctx cannot exist on 2D, the mint is
virgl-gated) ride the local interactive gate.

The Warp-C **C-2b** gate is `tools/warp-host.sh composed`
(`tools/warp/composed-screen.exp`): does the compositor's SCREEN follow the
host's negotiated GL capability? It exists because C-2a/C-2b shipped with the
3D arm having never executed — `alloc_screen` runs only under
`Scanout::Composed`, and every verb above that boots the GL device (`capset`,
`prove`, `tri`) drives at most one display-sized surface, which `reconcile()`
resolves to Direct, scanning out the client's own resource and bypassing the
screen entirely. (`quake`/`decomp`/`wedge` DO reach Composed — but through
GLQuake, i.e. the pool binary, S3TC quirks and 900 s budgets.) The driver here
is `/bin/tapestry-battery`: a ramfs-native client whose two surfaces are the
cheapest thing `reconcile()` resolves to Composed, needing neither GL nor the
pool, so the only GL object in the experiment is the compositor's own screen.
**The device IS the control**, which is why the scenario takes one as a
parameter rather than hardcoding the GL model — two legs on one host, one
variable, each asserting the other's outcome is wrong: `virtio-gpu-gl-pci` →
posture `GPU` → `screen res N 3D (compositor ctx)`; `virtio-gpu-pci` → posture
`CPU` → `screen res N 2D`. A GL-only leg would pass identically against a
tapestryd that ignored `comp_ctx` and always minted 3D, so the non-GL leg is
what makes the GL one mean anything. Two claims stay separate rather than
collapsing (the posture matches the DEVICE; the screen arm matches the POSTURE),
so a host that silently lost its GL cannot satisfy the second by making both
sides equally wrong. The verb's four-term conjunction requires both screen lines
AND both `LS-CI PASS: composed-screen:` completions — a leg that died right
after printing its screen line would otherwise still show the gate everything it
greps for, which is the `reject` verb's own F8 failure (it grepped `C0-REJECT`
while the producer printed `C0-DETECT`) in a different costume. First measured
on thyla-pi (KVM, real V3D) 2026-08-16: `res 67 3D (compositor ctx)
(1280x800)` and `res 67 2D (1280x800)` — the identical resource id on both legs
corroborating that the capability branch is the only thing that moved.

**What the `3D` word attests — corrected 2026-08-17.** This paragraph used to say
the arm was "the conjunction of four response-checked round trips the host
answered OK — `CTX_CREATE`, `RESOURCE_CREATE_3D`, `CTX_ATTACH_RESOURCE`,
`ATTACH_BACKING` — a claim about the host ACCEPTING the object". That was
**false** (GPU-DESIGN §4.5.4c): `Ctrl::step` does wait and check the response
type, but QEMU's virgl path (v10.0.0 `hw/display/virtio-gpu-virgl.c`) answers
`RESP_OK_NODATA` for all four whatever virglrenderer returned — those
`virgl_renderer_*` returns are ignored — so the responses attested that QEMU
*parsed* the commands (nonzero, non-duplicate ids) and nothing about the
renderer. Only `SET_SCANOUT` (via `resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) carry a verdict, and tapestryd dropped
the first one's result at the composed switch. #240 had already shown this for
`SUBMIT_3D`; the mistake was not checking the rest of the family.

Now the word is **earned**: `alloc_screen` writes 16 sentinel pixels into the
fresh screen's backing, `TRANSFER_TO_HOST_3D`s them through `COMPOSITOR_CTX`,
clobbers the backing, `TRANSFER_FROM_HOST_3D`s back and compares (then restores
the zeros) — a round trip that succeeds only if the renderer holds the
resource, has it attached to the compositor context, and moves pixels through
it. A refusal falls back to 2D *for real* and the screen line says why
(`-- 3D refused: create | ctx attach | attach backing | renderer round trip`);
the composed-entry line prints *after* the bind with its verdict (`scanout
composed (WxH) res N bound` / `BIND FAILED`), and the scenario's fifth term is
that the bound resource is the minted screen (`WARP-COMPOSED BOUND: res N`, on
both legs; the verb requires exactly two). What the arm is *still not* is a
claim about composed pixels: the screen is CPU-filled at C-2b, so correct
pixels here would evidence the CPU path. The pixel oracle becomes load-bearing
at C-3, where this scenario grows a QMP arm.

**Measured on thyla-pi (KVM, real V3D, boot-ms ~212 000, 2026-08-17).**
*Sabotage* (the 3D create issued with `VIRGL_FORMAT` `0x7FFF`, which the
renderer refuses and QEMU parses): GL leg → `composed path = GPU`, then
**`screen res 71 2D (1280x800) -- 3D refused: renderer round trip`**, then
`scanout composed (1280x800) res 71 bound` — i.e. `CREATE_3D`,
`CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all returned OK from the device
under a format the renderer cannot accept (the reason would otherwise have
named the step), the renderer refused, the fallback to 2D was real and the
display got a working screen; the scenario went RED on the arm (`want 3D`),
the verb reported three GATE FAIL terms. The non-GL leg was unaffected (`2D`,
`res 71 bound`, PASS). From the old `is3d` — `comp_ctx && create.is_ok() &&
attach.is_ok()` — those measured OKs would have printed `3D`, and the bind of
the phantom resource would have failed silently: that half is inferred from
the measured OKs and the old boolean, not itself measured. *Clean build*, same host, same hour: GL leg → **`screen res
71 3D (compositor ctx) (1280x800)`** — the word now earned by 16 pixels round-
tripping through the renderer on real V3D — then `scanout composed (1280x800)
res 71 bound`, `WARP-COMPOSED BOUND: res 71`, PASS; non-GL leg → `2D`, `res 71
bound`, PASS; the verb's five terms all held (rc 0). One variable (the format
the renderer will accept), two verdicts, on the same resource id both times.

The Warp-C **C-2d** gate is `tools/interactive/ls-gfx-age.exp` (LS-CI, HVF,
local — it needs no GL: the property is the guest client's, not the host's).
C-2d is the per-slot host resource (`GPU-DESIGN.md` §4.5.8: one host resource
per weave slot, so a compositor blit of slot *i* cannot collide with a client
fill of slot *j*), and its cost is that the single accumulating resource
damage-only clients silently relied on is gone — a client must repaint the
union of everything that changed since the slot it is about to draw into was
last presented (`libtapestry::age()`, the `EGL_EXT_buffer_age` contract,
derived client-side because the library owns the rotation and the loom CQE is
kernel ABI). Landed as C-2d-a (client, `931bf15a`) then C-2d-b (server,
`f86177b6`) — and `f86177b6` shipped **explicitly unverified**: the §4.5.8c
sabotage (aurora's age handling disabled) left `ls-gfx` green, because
`ls-gfx` asserts a console-shaped frame and `ls-gfx-panes` drives a full-frame
client. The gate that can see it is described in `140-aurora.md` "The gates";
its structural points: it must run in DIRECT scanout (composed blits only the
damage rect into the screen, which is itself the accumulator, so a stale client
slot is invisible there), the slot phase is DRIVEN by keystrokes rather than
sampled at random, and the 1,1,2,1,1,2,1,1 key pattern covers all three slot
residues for any constant blink rate — the one-stale-slot class (an off-by-one
in the union) is otherwise passed 2/3 of the time per dump. Both sabotage
classes measured red 3/3 attempts (S1 rounds 2,1,2; S2 rounds 2,5,2), the fixed
build green 0/368280 px on 8/8 dumps. What it does not cover: the composed
path (C-3's property), the hidden→visible redraw fan (aurora is never hidden
here), and tapestryd's per-slot refactor as such — the focused audit round on
`usr/tapestryd` (I-40 surface) is still owed.

The Warp-C **C-2c** gate rides `tools/warp-host.sh composed` as the scenario's
third claim (`composed-screen.exp`) and the `quake` verb's ctl census
(`glq-virgl.exp`). C-2c is the compositor-side import (`GPU-DESIGN.md`
§4.5.10, as built §4.5.10a): at `alloc_weave` every slot resource of a
generation is `CTX_ATTACH_RESOURCE`d into `COMPOSITOR_CTX`, and — because
that command's OK attests nothing about the renderer (§4.5.4c) — each import
is WITNESSED by a pixel copy: two distinct tokens seeded into the slot's host
copy at guest rows 0 and h−1 through the present path's own
`TRANSFER_TO_HOST_2D` (the guest pixels are borrowed for the transfers while no
client mapping exists yet, then zeroed), the compositor's own 1×1 sentinel
poisoned, `RESOURCE_COPY_REGION` slot box (0,0,1,1) → sentinel *inside* the
compositor context, the sentinel read back, RGB compared against both tokens
(which row the copy read on the `Y_0_TOP` source is REPORTED, not assumed —
`witnessed 3/3 (copy read texel row R)`). A health copy
(mark → sentinel) runs first so a REFUSED is attributable to that import and
not to a context an earlier refusal latched (`vrend` refuses every later
command buffer on a context that reported `ILLEGAL_RESOURCE`, §4.5.4a — which
is also why `comp_attached` fails closed and C-3 must never blit from a
resource without it). One say line per generation (`comp-attach surface N res
A..B: witnessed 3/3 | REFUSED (slot i copy did not land) | SKIPPED (...) |
attach failed (device, slot i)`); the instrument reports on its own line on
GL (`comp-attach witness armed (probe res M,S)`, after the posture anchor —
not on it, since the mint's round trips put the anchor into the kernel's
`proc: orphan` burst on the first measured run and it came out torn), and
the posture line carries `comp-attach: skipped (no compositor ctx)` without a
compositor ctx; the global warp ctl
carries `comp-attach witnessed W refused R`. The GL adoption's consented BO
is imported at `present-to` with a two-poison CHANGE witness (the BO's texel
is the client's rendering, unknown to us) and revoked before its unref on
every death path (`wbo_retire`, `present-to off`/replace, the surface's
retire). **At most one import witness per ctx per compositor tick** (C-0d
Fable round F5, the `verify_tick` shape on `WarpCtx.import_tick`): the
witness is a dozen synchronous device ops on the SHARED compositor context
(the attach, the health copy, up to two rounds of texture-sentinel
readbacks), and a `present-to N bo` / `present-to off` / `present-to N bo`
loop re-ran all of it at 9P-write rate. A second consent in the same tick is
**deferred, never dropped** — `frame_tick` replays the import of whatever
`present_to` names by then (`comp_replay_deferred_imports`); the winsys
re-consents only when its front buffer changes, so the only legitimate
second write in one frame is a resize storm, and coalescing those onto ticks
costs it at most one tick of the readback arm. Gate terms: GL leg — ≥ 2 per-surface `witnessed n/n` lines (the
battery's two surfaces) and none refused (the posture anchor carries no C-2c
claim on GL); 2D leg — the import declared skipped and no per-surface line
(the control); verb terms six/seven `WARP-COMPOSED ATTACH: witnessed K
surfaces` / `skipped (no compositor ctx)`; `quake` — `refused` 0 and
`witnessed ≥ 1` in the census read after the game died. **Measured on
thyla-pi (KVM, V3D), 2026-08-17** — clean: 8/8 generation imports `witnessed
3/3`, the copy reading texel row h−1 every time (the FBO copy path measures a
`Y_0_TOP` box from the bottom; C-3's blit boxes inherit that), `WARP-COMPOSED
ATTACH: witnessed 2 surfaces (copy read texel rows: 799 797)`, both legs
PASS, verb VERIFIED (7 terms); sabotage (slot attaches skipped): first import
`REFUSED (slot 0 copy did not land)`, every later one `SKIPPED (compositor ctx
unhealthy)` — the vrend latch measured — and the screen's 3D mint fell back
(`2D ... -- 3D refused: renderer round trip`, the §4.5.4c fallback taken for
real), verb RED, 2D leg unaffected. Wrong turns paid: a single-row seed read
REFUSED on the clean build (the copy lands from row h−1 → the two-row seed);
the posture anchor torn by the kernel's `proc: orphan` burst (the console TX
ring is byte-atomic — OPEN, `memory/bug_console_tx_ring_byte_atomic.md`); and
three gate-script defects (a say-line format change under an anchored regexp;
`-re` arm ORDER beating buffer position; an unanchored alternation matching
PARTIAL serial lines). `quake` (KVM/V3D): `comp-attach ctx 1 bo 1 res 79 ->
surface 1: witnessed`, `GLQ-VIRGL COMP-ATTACH: witnessed 5 refused 0`, WARP-4
GATE VERIFIED — after a first run failed closed on a C-2d-b leftover (five
`scanout direct N (WxH)` regexps in `glq-virgl`/`glq-decomp`/`glq-wedge-probe`
broken since `f86177b6` added the `slot S` token; optional in all five now).
What it does not cover: composed PIXELS (the screen is
still CPU-filled; C-3 owns the first composition blit and grows from
`comp_copy_px`), the in-flight clause (no fenced blits exist yet), and the
focused I-40/I-45 audit on `usr/tapestryd`, still owed.

The Warp-C **C-3** gate is the same scenario's fourth claim — the composed
PIXELS — plus two battery legs (`GPU-DESIGN.md` §4.5.11). C-3 replaces the
CPU fill of the composed screen: on a GL host a software surface's present
transfers its damage into the presented slot's own resource and composes by
`VIRGL_CCMD_BLIT` slot → screen inside `COMPOSITOR_CTX` on the compositor's
SYNC slot (`submit_blits`, one dispatch: transfer → blit → flush, so the
I-40 by-construction shape is kept and detach-before-unref stays the whole
retire ordering); a witnessed GL adoption composes by one blit BO → screen;
chrome stays CPU-painted and uploaded on damage on both paths (a focus-only
repaint uploads only the frame/strip rects — the whole-buffer push would
blank every pane on the GPU path); the screen is minted `Y_0_TOP` (the 2D
screen's display convention; C-2b's flags-0 screen displayed a top-down CPU
fill inverted on a GL display, invisible under #195). Box conventions are
MEASURED at bring-up per (source shape × size class) with a confirmation
each (`blit-conv <slot|bo> <U|S> <variant>: rows <16-char map> -> …`,
`… confirm (…): rows … -> CONFIRMED`) on throwaway contexts, fail closed per
class; the compose path picks the class by the op's box sizes and issues
through the same builder. The compositor runs its #240 health copy after a
GPU-composed present and latches GPU composition OFF (sticky,
`composed-gpu-dead`, a structural repaint at the next tick) on a failure —
since C-4 on a BUFFER pair, issued once per `HEALTH_PERIOD` (4 ticks) and
read a period later, so no step of it enqueues GPU work (§4.5.12).
**The pixel oracle** is `probe-screen X Y` (tapestry global ctl, test-mode,
ungated like the determinism verbs, rate-limited): the compositor reads texel
(X,Y) of the SCREEN back — `via readback` (TRANSFER_FROM_HOST_3D through the
compositor ctx, the only place a GPU-composed pixel exists) on the 3D screen,
`via backing` on the 2D one — and says `screen-probe (X,Y) = #rrggbb via …
[scanout S; composed gpu G cpu C]`. The battery probes its own sample points
at every pixel stage and grew `multirect-v` (B split top/bottom green over
yellow — the vertical asymmetry a mirrored/displaced box cannot fake) and
`tab-cycled ready` (A hidden by the tab, revealed by the cycle, presented
red, probed — the C-2d redraw contract on the composed path). Gate terms:
9/9 probes exact `via readback` with `composed gpu ≥ 1` on the GL leg (a
build whose GPU path silently routed everything to the CPU one composes
CORRECT pixels; only the census tells that apart), 9/9 exact `via backing`
with `gpu 0` on the non-GL leg — verb terms eight/nine. **Measured on
thyla-pi (KVM, V3D), 2026-08-17.** Run 1, one convention for both classes
(measured unscaled, applied everywhere): the battery's panes — both SCALED
(A 1280×800 → 638×398, B 640×400 → 636×398: the 1-px frame inset makes every
"matching" pane the scaled class) — composed vertically swapped; the first
probe read `(960,200) = #0000ff` for A's red, `LS-CI FAIL` — the oracle at
real geometry caught what the probe's own (unscaled) confirmation could not.
Run 2, per-class: `slot U plain sf1 df1` (the copy-image path, both sides
inverting), `slot S plain sf0 df0` (glBlitFramebuffer, raw boxes), `bo U
plain sf0 df1` (copy-image lands a GL-native source mirrored on its own), `bo
S src-neg sf0 df0` (the plain scaled request landed STRAIGHT `.0011…`; the
negative-source-height idiom mirrors it), all four CONFIRMED; then A red
`(960,200)`, B blue `(960,600)`, multirect green/yellow `(800,600)/(1119,600)`,
multirect-v green/yellow `(960,500)/(960,699)`, tab strips `#3a3a44`/`#7a9ecc`
at `(800,2)/(1120,2)` (chrome through the 2D transfer into the 3D screen),
tab-cycled A red `(960,402)` — `9 probes via readback ok (composed gpu 35 cpu
0)`, GL leg PASS. The 2D leg (run 1's, the CPU path with the same battery)
`9 probes via backing ok (composed gpu 0 cpu N)`, PASS. Run 3 (both legs on
the final binary) + the sabotages: see the C-3 status row.

**Warp-C C-4 — the decomp instrument grew a cost census and a second display
lane** (`tools/warp/glq-decomp.exp`, `tools/warp-host.sh decomp`,
`tools/run-vm.sh`; GPU-DESIGN §4.5.12). Each leg now snapshots tapestryd's
present-path cost census (`cost <kind> <n> <sum_us> <max_us>` lines of
`/dev/tapestry/ctl`, read with `cat` before the launch and after the
teardown — the census prints every kind in a fixed order ending in `push`,
which is the parse's stop condition, so no sequencing marker is typed) and
prints the DELTA as `GLQ-DECOMP COST-<dev>-<leg>: <kind> n=N us=T avg=A
max<=M; …` (one entry per kind whose count moved; the max is the cumulative
one, an upper bound, labelled `<=`). `GLQ-DECOMP DISPLAY-<dev>: <lane>`
names the GL display backend the figures were taken under: `egl-headless`
(the default; a full-frame `glReadPixels` per flush) or `dbus-gl`
(`WARP_DISPLAY=dbus-gl` → `WARP_GL_DISPLAY` → `THYLACINE_DISPLAY=dbus-gl` →
`-display dbus,p2p=on,gl=on`, no listener, no readback; the log is
`build/warp-decomp-gl-dbus-gl.log`). Figures reported, never gated, as
before. Under `dbus-gl` nothing can screendump the guest — it is the lane
for the guest's own present costs, and only that. Measured 2026-08-17 on
thyla-pi (KVM, V3D): see "The composed residual decomposed" under
Performance characteristics.
