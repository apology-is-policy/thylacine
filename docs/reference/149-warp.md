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
                             # test-mode ONLY adds: "abandoned <n>\nfenced-free <n>\n"
caps                         # the RETAINED preferred capset blob, raw
ctx/
  new                        # open+read mints a ctx -> "<pub_id>\n" (ONE per conn; I-45)
  <id>/
    ctl                      # write: "capset <n>" | "rings <1..64>" | "destroy"
                             # read: "<id>\npoisoned <0|1>\nleaked-count <n>\nleaked-bytes <n>\n"
                             #   + "fences-in-flight <n>\nfence-signaled <n>\n" (promoted at Warp-3)
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
  which is the least diagnosable failure this harness can produce.
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
- **Composed** (windowed, the ladder's readback fallback): a SYNCHRONOUS
  `TRANSFER_FROM_HOST_3D` (the compositor's own sync step, NOT the
  client fenced lane -- the present stays one dispatch, the I-40
  premise) pulls the frame into the BO's own backing, and
  `blit_composed_pixels` composes from those pages (letterbox/crop
  shared with the weave path; `res_stale` stays TRUE -- the weave never
  saw GL frames). No orientation flip: the guest-visible transfer
  contract is gallium top-down, same as a weave.
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
| **stream the HOST refuses (vrend context error)** | **none — reported as SUCCESS (#240).** The write returns the byte count, the fence retires, `fence-signaled` increments, `fences-in-flight` returns to 0, `poisoned` stays 0. See the caveat below |

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
