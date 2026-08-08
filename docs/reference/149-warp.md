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
                             # test-mode ONLY adds: "abandoned <n>\nfenced-free <n>\n"
caps                         # the RETAINED preferred capset blob, raw
ctx/
  new                        # open+read mints a ctx -> "<pub_id>\n" (ONE per conn; I-45)
  <id>/
    ctl                      # write: "capset <n>" | "rings <1..64>" | "destroy"
                             # read: "<id>\npoisoned <0|1>\nleaked-count <n>\nleaked-bytes <n>\n"
                             # test-mode ONLY adds: "fences-in-flight <n>\nfence-signaled <n>\n"
    submit                   # write: one Twrite = one atomic opaque CCMD submission (fenced)
    fence                    # read: the completion stream -- newest signaled fence id,
                             #       one record per read, PARKS when nothing unreported
    bo/
      new                    # open+read mints a BO record -> "<pub_id>\n"
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
unchanged semantics); fenced slot `i` (0..4) owns the fixed descriptor pair
`(2+2i, 3+2i)` with its request buffer and response header in a SECOND DMA
region (`FLANE_DMA_SIZE` = 4×64 KiB + a response page at `GPU_FLANE_VA`),
allocated **only when VIRGL negotiates** — a 2D boot allocates nothing and
the audited two-page sync ring is byte-identical.

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
(`fences_in_flight` / `fence_signaled` / `fence_reported`). A read returns
the newest signaled id once (records coalesce — FIFO within the single
ring, so id N retires everything <= N) and PARKS otherwise (`PendingFence`,
the FK_EVENT netd leg) with all four cancel sites mirrored: clunk, Tversion
reset, conn teardown, Tflush. A dead ctx EOFs the stream — and so does a
**poisoned** one, unconditionally (round-5 F2). That EOF used to be
conditional on `fence_signaled <= fence_reported`, which the client could
suppress itself: fence ids are globally monotone, so any later submission
that completed left `signaled > reported` and the read returned that higher
id — which, under the coalescing rule above, *asserts the abandoned fence
completed*. A poisoned ctx also refuses new submissions and transfers with
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

Three read-only `test-mode` ctl fields exist purely so those legs have a
bounded, discriminating observable:

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

## Error paths

| Path | Verdict |
|---|---|
| any warp file on a 2D device (`virgl 0`) needing the device | `E_OPNOTSUPP` |
| ctx/BO resolve not owned by this conn, or dead | `E_NOENT` |
| second ctx mint on one conn; ctx-slot/BO exhaustion | `E_NOMEM` |
| `create3d` refused (size, implausible geometry, ctx backing cap) | `E_IO` |
| submit/transfer, fenced lane momentarily full | `E_AGAIN` (retry) |
| fenced lane permanently exhausted (every slot poisoned) | `E_IO` (do NOT retry) |
| submit stream larger than a slot | `E_INVAL` |
| engine dead (latched) | `E_IO` |
| fence read with `count` < one record (21 bytes) | empty read (never parks unfillable) |
| malformed ctl verbs / non-UTF-8 | `E_INVAL` |

## Known caveats / footguns

- **#170**: the graceful half is closed — `kobj_pci_quiesce` runs from
  `proc_quiesce_owned_devices`, so a PCI-transport driver stops decoding
  and mastering before the exit path frees its DMA pages (round-1 F8: the
  sweep knew only virtio-MMIO, and a BAR-decoded device is invisible to
  it). The task stays open for the residual ordering in the fallback
  `proc_free` path.
- **Client-visible limits**: 8 contexts total, one per connection, 16 BOs
  each, 64 MiB live+leaked backing per context, 4 concurrent `/srv/warp`
  connections (a 5th blocks until one frees), 4 fenced chains in flight
  process-wide.
- One Twrite = one submission: the effective stream bound is the 9P iounit
  (msize 32 KiB − overhead), not the 64 KiB slot. The Loom-carried bulk
  path (§4.1) lifts this at Warp-3 if the winsys needs it.
- The fence file reports coalesced ids; a client that needs per-submission
  granularity tracks its own issue order (FIFO within the ring).
- Multiple fids parked on one ctx's fence file race for records
  (first-parked wins); one client per ctx is the intended shape.
- `rings <n>` and `capset <n>` are recorded, not yet negotiated to the
  device (F_CONTEXT_INIT / per-ring fencing are the Venus deltas).

## Tests

Kernel: `pci.walk_caps_shm` (6 discriminating vectors incl. the
`length_hi` verdict-flip + the overflow-wrap sum) and
`weft.gpu_bo_share_and_claim` (mint bits, envelope, BOTH admission gates,
kind decision, budget charge/uncharge, reaper registration) — both in the
1358-test suite; the four weft buggy cfgs still fire. Userspace: the local
2D boots prove the degradation path (`Warp PROBE OK ... virgl 0`, submit
E_OPNOTSUPP, no flane allocation) and `ls-gfx`/`ls-gfx-play` exercise the
rewritten controlq under real present pressure; `/warp-prove` on the GL
host is the PASS-path gate.
