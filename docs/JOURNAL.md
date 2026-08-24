# The autonomous-run journal

**What this is for.** After a long autonomous run the operator needs to
reconstruct what happened without stitching together `git log`, six phase-status
rows, and a memory directory. This is that single thread: what landed, in order,
why, what it cost, and what it left open.

**What it is NOT.** Not a changelog — `git log` already has the commits, and
duplicating them here would rot. Not a status doc — `docs/phaseN-status.md` owns
per-chunk rows. What lives here is the *narrative*: the reasoning, the wrong
turns, the findings that were not in anyone's plan, and the decisions that
needed the operator.

**Conventions.**

- Newest run first. Within a run, chronological.
- Every claim carries its evidence: a hash, a measured number, a file:line.
- **A wrong turn is worth more than a win** — record the ones that were caught
  and how, because those are the reusable part.
- **Say what is still open, and be exact about what "fixed" covers.** A half a
  defect closed is written as a half.

---

## 2026-08-24 — V-3b-1c-2b-a: a green gate over a dead claim path (reverted, parked)

Rolled straight into 1c-2b after 1c-2a landed: the client-claimable host3d ring.
The tapestryd change was one line — `wring_weft_ensure`'s `if r.dma_fd < 0` bail
became `&& r.host3d.is_none()`, so a host3d ring's hostmem burrow gets
`t_weft_share`d and routes to the kernel's `WEFT_BIND_HOSTMEM` (weft.c:401). I
extended the boot self-test to weft-share the ring, added a `WEFT_SHARE` gate leg,
and it all went green: build clean, 29/29 discriminator, and — the part that
should have been reassuring and was in fact the trap — **VENUS GATE: VERIFIED on
real V3D**, `warp host3d-ring venus-ctx=512 MAPPED+ROUNDTRIP WEFT_SHARE teardown
OK`.

The Fable holotype refused the green. **F1 [P1]: the client CLAIM is structurally
dead.** Four kernel sites must admit a kind for a weft share to be *claimable* —
register, kind-decision, client-map, and the binding alloc. V-2 widened the first
three for HOSTMEM; `weft_binding_alloc_maponly` (weft.c:472) still requires
`BURROW_TYPE_DMA` and handles only weave/gpu_bo, so a hostmem burrow returns NULL
and the client's `t_weft_map` unwinds to -1. My WEFT_SHARE gate certified the
*register* half — `t_weft_share` succeeds — as "the client-claim substrate," and
the self-test never once exercised the binding alloc, so it was green over a claim
that cannot happen. This is the [[bug-240-new-gate-hollows-old-negative]] shape
inverted: a widen that touches N-1 members of a property set the "must widen
together" comment (syscall.c:6004) itself names, leaving the last a dead half — and
a self-test written to prove the whole path that only ever drives the live part.

**F2 [P2] is worse and masked by F1.** For guest-blob rings the #847 dual-count
pins the guest-RAM *pages*, so a client's mapping survives teardown. A hostmem
ring's backing lives behind the GPA (`map_blob`'s subregion) — the dual-count pins
only the kernel Burrow *object*, and `drop_host3d_ring` yanks the host backing +
re-hands the offset unconditionally. The instant F1 is fixed, tearing down a
claimed ring exposes one client's live mapping to another's ring. The teardown
comments asserting "the client's mapping survives via its own ref" were vacuous
under 1c-2a and would have become load-bearing and false. tapestryd cannot even
see the kernel `mapping_count`, so the fix is a real lifetime design (a reaper with
a new syscall, or leak-on-claim, or a kernel primitive) — not a patch.

**My self-audit missed both**, the same way as 1c-2a's F1: I verified ownership,
extent, and teardown of the *unclaimed* (self-test) case exhaustively and never
traced the client *claim* through all four kernel sites — I stopped at the surface
I changed. And critically the delta *regressed* the client path (from a clean
`E_NOMEM` to a half-broken map fid), so it could not land. Surfaced the F2 design
fork to the operator (design-conversation pattern); they voted **park 1c-2b, do
V-3b-2 next** — F2 deserves a design pass, not a rushed post-600k call. Reverted
the delta (tree pristine at `3e12ef12`); findings enqueued in
`memory/bug_v3b_1c2b_hostmem_weft_claim_gap.md`. The reusable lesson, twice this
run: a green gate proves what its self-test *drives*, and a self-test that exits
before the load-bearing call is green over a hole.

## 2026-08-24 — V-3b-1c-2a: the server host3d-ring path (three catches the local gates could not make)

Same autonomous run, resumed past a second self-compaction. 1c-1 was the engine;
1c-2a wires it into the `/srv/warp` server so a HOST3D ring is a client-mintable
flavor under a per-client venus device-ctx. The plumbing was routine — the value
of this entry is the three defects, each caught by a *different* instrument
because the cheaper one was structurally blind to it.

**The recon was wrong, and a grep caught it before a line was written.** The
pickup pinned the venus ctx id as `COMPOSITOR_CTX + 1 + slot`. Before writing it
I enumerated every `ctx_create*` id in the daemon (the enumerate-mirrors reflex)
and found `CONV_PROBE_CTX_BASE = COMPOSITOR_CTX + 1` — the conv-probe throwaways
occupy exactly that base. The two families were temporally separated (conv probes
die before any client mints), so the alias was latent, not live — which is
precisely how it would have survived review. Moved the band to a dedicated
`0x200 + slot` with a `const _` gap assert. The lesson is old (a recon note is a
hypothesis, not a fact) but the mechanism is worth naming: a pure-function id
scheme (`base + slot`) collides silently with any *other* pure-function scheme
sharing the base, and only a full enumeration — not a spot check — finds it.

**F1 was mine to catch and I didn't; Fable did.** My self-audit reached the
`wctx_finish` leak arm, saw it did not destroy the venus ctx, and reasoned:
"consistent with dev_ctx (which the leak arm also leaves alive), and the slot is
poisoned so the id can't be reused — fine." That is half the machinery. What I
did not trace is the *vindication* path: when the device finishes the abandoned
chain, it destroys dev_ctx, **un-poisons the slot**, and recycles it — destroying
only dev_ctx, never venus. So a wedged-then-recovered slot leaks its venus ctx
*and* the next client that lands there re-mints `WARP_VENUS_CTX_BASE + slot` into
a still-live host context (EEXIST → that slot permanently loses host3d). The fix
is the holotype's option (a): destroy venus in the leak arm too — it is quiesced
by construction at 1c-2a (no submit path targets it, its rings were dropped
unconditionally just above) — and on a refused destroy skip the vindicate stamp
so the slot is permanently condemned rather than recycled into the collision.
This is the exact shape of the whole-system-stewardship failure mode: I stopped
tracing at the boundary of the function I changed; the bug lived one call away in
the recovery path I did not open. A same-family reviewer with *context
independence* (Fable had not watched me talk myself into "consistent with
dev_ctx") is what closed it.

**F2 is a disclosure armed one rung ahead.** The 1c-1 free-list hands back a
reused hostmem extent verbatim — `drop_host3d_ring` reclaims the offset but does
not scrub — and `wring_install_host3d` wrote nothing into the ring. At 1c-2a the
client claim path fails closed (`wring_weft_ensure` returns None on `dma_fd < 0`),
so it is latent; but this chunk is the substrate for the very next rung that makes
the memory client-visible, and the next author greps for "claim", not "zero". Zero
the ring at install. The 1c-1 probe's own physical-reread leg is built on the fact
that freed bytes persist across re-mint, so this was not hypothetical.

**The GL boot caught what the no-boot gate never could.** `test-venus-verdict.sh`
is 28/28 and discriminates — against *crafted fixtures*. It tests the verdict
logic, not the capture. The real venus boot on thyla-pi came back UNVERIFIED: the
control leg emitted no `warp host3d-ring skipped` line at all. Ground truth (read
the 13-line filtered log, don't theorize): `boot-probe.sh` captures only
`grep "tapestryd: gpu"` — and my self-test lives in `server.rs` with a `warp`
prefix, so the filter dropped it before the gate could see it. The 1c-1 line was
`tapestryd: gpu hostmem-ring` (gpu.rs); mine is `tapestryd: warp host3d-ring`, and
that one-word prefix difference is invisible to a fixtures-only test. Broadened
the filter to `gpu|warp host3d-ring`. This is the `test.sh(HVF) != test-interactive(TCG)`
class restated for capture: a discriminating gate can still be blind to whether
its evidence line is ever *recorded*, and only a real boot exercises the record.

Audit: holotype-reviewer Fable 5 (max, MODEL start==end, family diversity),
**0 P0 / 1 P1 / 1 P2 / 3 P3, all fixed** (F3 = my pre-landed kick guard; F4
"teardown OK" now reads the poisoned flag; F5 named the structural bound over the
compiled-out `debug_assert`). Not dirty. Re-verified on real V3D after the fixes.
Owed to 1c-2b/V-3b-2: when venus submits land, both the leak-arm
"venus quiesced by construction" argument and the kick fail-closed graduate to
real venus discipline — named in the code.

## 2026-08-24 — V-3b-1c-1: the persistent hostmem ring engine (a deliberate split)

Same autonomous run, resumed on the far side of a self-compaction at the 600k
checkpoint (the run-through rule). The pickup named V-3b-1c as one chunk: hoist
the allocator, build the client-claimable `/srv/warp` ring, drive teardown. On
reading the ground I split it, and the reasoning is the interesting part.

**Why the split.** The Model B ring, for a cross-Proc client, is a HOST3D blob
weft-shared as a `BURROW_TYPE_HOSTMEM` burrow -- and `weft.c:401` already admits
exactly that (`WEFT_BIND_HOSTMEM`, the V-2 surface built but never exercised by a
real client), reached through the same `t_weft_share(va,size)` tapestryd already
calls for the V-3a guest-blob ring. So the client path needs no kernel work --
which means the whole thing is buildable, and pull-forward would say build it
all. But it decomposes at a clean seam: the *engine* (a persistent allocator + a
reusable mint/teardown lifecycle, provable by the probe alone) versus the *client
surface* (a per-client tapestryd-owned venus device-ctx + the weft-share of a
hostmem burrow + a `warp-prove` cross-Proc leg). The engine is a complete,
non-forking, independently-auditable foundation; the client surface is a larger
new kernel-exercised path that earns its own audit. 1c-1 is the engine; 1c-2 is
the surface. This is a sub-chunk split, not a deferral of scope -- deliverables
#1 (hoist) and #3 (teardown lifecycle) are 1c-1; #2 (client claim) is 1c-2.

**What landed.** `HostmemAllocator` hoisted into `Gpu.hostmem`, sized once at
probe, with a first-fit free-list so a persistent daemon reclaims a retired
ring's offset (bump-only would exhaust the 256 MiB region). A reusable
`mint_host3d_ring` / `drop_host3d_ring` pair with full error-path unwinding
(offset -> resource -> subregion). The probe rewritten to PROVE the engine, not a
single map: two rings at distinct offsets (`0x0`, `0x1000`), a sentinel through
each guest VA, teardown of both, then a re-mint that must reuse a freed offset --
one verdict line, emitted only when all four hold.

**The gate got a real discrimination, not a token check.** The probe emits its
success line only on `a_ok && b_ok && distinct && reuse`; a lifecycle regression
(e.g. `reuse=false`) emits `hostmem-ring FAIL (...)` instead. `test-venus-verdict`
gained a leg that REPLACES the success line with a `reuse=false` FAIL line and
asserts the verdict rejects it -- so the free-list reclaim is a tested property,
not one that rides an absent-token check (the M-PIN: anchor on what only success
produces; sabotage the path under test). 24/24 discriminates, no boot.

**The holotype's best catch was a type-system one (Fable 5, 0/0/1P2/3P3, all
fixed).** F1 [P2]: `HostRing` was `#[derive(Copy)]`, `drop_host3d_ring` took it
by `&ref`, and `free()` validated nothing -- three innocuous choices that
COMPOSE into a silent double-free. The probe drops each ring exactly once, so
1c-1 is correct today; but this rung's deliverable IS the reusable engine API,
and the day 1c-2 lands a second retire path (a death reaper AND a close verb,
the shape tapestryd already has for BOs), two `Copy` handles each drop the same
ring, `free()` pushes the offset twice, and two later mints hand ONE hostmem
offset to two clients' rings -- cross-client aliasing, no log line. The fix is
the type system: drop `Copy`, take the handle BY VALUE, so a double-drop is a
compile error; the `free()` oob/overlap guard is the belt to that suspenders.
The reusable lesson: a resource handle that is `Copy` is a double-free waiting
for a second caller -- make it a move-only single-use token and let the compiler
hold the contract the doc comment cannot. F2 [P3] was the same instinct on the
probe: it proved the ALLOCATOR handed distinct offsets, not that the two guest
mappings were PHYSICALLY distinct (one sentinel constant, A never re-read after
B) -- so a kernel aliasing bug would have passed it. Offset-derived sentinels +
re-reading both after both writes makes it witness the physical fact. My own
self-audit had F1 (as two P3s) and F3, and converged with the round on the rest;
the round's upgrade of F1 to P2 (the API IS the deliverable) was the right call.

**Cost/open.** userspace + tools + docs only; kernel byte-unchanged, so no
specs/SMP delta. GL verification owed on thyla-pi (the two-ring distinct-offset +
reuse line under a real venus ctx). V-3b-1c-2 (the client-claimable ring) is next
and is where the weft-share-of-hostmem and per-client venus-ctx forks live.

## 2026-08-24 — V-3b-1b: the guest-map, and the Result alias the compiler caught

Same autonomous run as the V-3b-1a entry below, continued past that chunk's push
(the run-through rule -- a checkpoint is not a stopping point). V-3b-1b guest-maps
the HOST3D ring blob: the client binding for SYS_BURROW_FROM_HOSTMEM (which V-2
built kernel-side but never wrapped -- "client delivery exercised only by unit
tests until V-3 drives it E2E") + a tapestryd hostmem-offset allocator + a probe
that round-trips a sentinel through the guest VA.

**The build earned its keep.** The compile-check caught a real error before the
GL boot: `PciDev::burrow_from_hostmem` was declared `-> Result<u64, PciError>`,
but hardware.rs has a 1-arg `Result` alias (`crate::err::Result<T>`) in scope, so
the 2-arg form is E0107, and `Err(PciError::MapBar)` then mismatched (E0308). The
existing PciError-returning methods (claim / claim_nth) spell it
`core::result::Result<Self, PciError>` for exactly this reason; the fix matched
them. Read by CONTENT, not by the wrapper script's exit code -- that was 0
because a trailing `echo` masked build.sh's real status, a reminder to grep the
log for `error[` rather than trust `$?` through a pipeline.

**The sentinel proof, and its limit, stated.** `hostmem_sentinel` writes a u32 to
the guest VA and reads it back at the same address. ARM same-address same-core
coherency round-trips it with no barrier, so a MISMATCH means the VA does not
alias the mapped BAR. It proves the guest can ACCESS the blob -- NOT that
virglrenderer sees the guest's writes (host-visibility), which is deliberately a
later rung (the ring poll, V-3b-1c/2) and is not claimed. The returned VA
reaching the BAR is the kernel's V-2 guarantee; the sentinel confirms the mapping
is live.

**The audit caught a design bug I would have shipped.** Fable 5 (family diversity
restored this round), 0 P0 / 0 P1 / 1 P2 / 3 P3. F1 [P2]: the probe hardcoded
`T_CACHE_WC` and *discarded* `map_blob`'s `map_info` -- but the host dictated
`map_info=0x1` (CACHED), and GPU-DESIGN 6.2 is signed-off that the guest maps the
attribute "honored exactly". A guest-WC vs host-WB alias is the ARM64
mismatched-attribute hazard the scripture's own field-agreement warning forbids;
it would have surfaced TWO rungs later at V-3b-1c as a "host never sees the kick"
coherency mystery on real-silicon KVM, with a comment on the FFI actively
pointing the debugger at write-combining (the x86 intuition, wrong on ARM). I had
written that comment myself. Fixed by consuming `map_info` -> `map_info_to_cache`
-> passing the host-dictated attribute, and rewriting the comment to state the
rule. The reusable lesson: an attribute you *choose* for a shared mapping is a
claim about the other side's mapping -- derive it from what the other side
dictated, never from what feels right for your access pattern. F2-F4 [P3] all
fixed (zero-size alloc alias; the leaked offset-0 mapping now `t_burrow_detach`'d
-- the "no detach primitive" comment was wrong, tapestryd already uses it; doc rot).

**Cost**: two pi boots (the venus verb, re-run after the F1/F3 fixes changed the
probe's behavior).

## 2026-08-20..24 — V-3b-1a: the HOST3D substrate, and the render server that wasn't there

Model B's first rung: the tapestryd primitive that mints a HOST3D blob and maps
it through the hostmem window (`create_host3d_blob` / `map_blob` / `unmap_blob`
in `usr/tapestryd/src/gpu.rs`), plus a two-arm `host3d_probe` init self-test that
proves the path against the real host. Small code; the run's weight was in
proving it on GL, and the proof took a four-boot hunt through a host-side blocker.

**The wire-format groundwork paid off twice.** Before a line of code, the
constants were re-derived against QEMU v10.0.2's verbatim `virtio_gpu.h` enum,
not the plan. Two catches: `RESP_OK_MAP_INFO` is `0x1106`, not the plan's
`0x1105` (that value is `RESOURCE_UUID` -- a silent off-by-one that would have
made every MAP_BLOB read the wrong response type); and a WebFetch fast-model
"summary" miscounted `CMD_RESOURCE_CREATE_BLOB` as `0x010d` -- refuted by the
already-GL-proven shipped GUEST-blob code, which uses `0x010c`. Ground truth
(the verbatim enum + working shipped code) beat both secondary sources.

**The GL hunt (four pi boots + a source build).** Boot 1: HOST3D create refused
`resp_type=0x1200` (RESP_ERR_UNSPEC) under both a virgl context and
device-global, while a GUEST blob still created fine. Source-cited to the vkr
(venus renderer) shm path -- a `blob_id=0` `USE_MAPPABLE` HOST3D blob is reached
ONLY via a capset-4 context (`vkr_context.c:369-372`) -- so the fix was to mint
Arm A under a venus context. Boot 2: STILL refused under the venus context. That
refuted the venus-ctx-alone theory and pushed the hunt down into virglrenderer,
where a `-d guest_errors` boot named the real error: `virgl blob create error:
Invalid argument` = `EINVAL` from `virgl_renderer_resource_create_blob`, with NO
fork-fail line anywhere.

**The wrong turn, and what caught it.** That missing fork-fail line led me to
write "the render server is likely irrelevant, in-process mode" and to tell the
operator their earlier "build the render server" instinct was refuted by the
errno. That was WRONG. The operator had ratified building the RS on my first
(render-server-missing) diagnosis; I then talked myself out of it on an *absence*
-- no fork-fail log -- which is not evidence. The catch was instrumented
ground-truth, not more reasoning: a non-destructive LD_PRELOAD boot with an
instrumented virglrenderer (my `libvirglrenderer.so.1.9.0` + my
`virgl_render_server` via the `RENDER_SERVER_EXEC_PATH` getenv override) traced
`ctx_lookup(202)=<registered> ... get_blob ret=0` and printed the substrate's own
proof line -- `tapestryd: gpu host3d-map venus-ctx MAPPED (map_info=0x1)`. The
render server WAS the root cause: Debian's `libvirglrenderer1` is process-mode
and ships no `virgl_render_server` binary (no package provides it), and without
it `get_blob` returns a bare `EINVAL` -- no distinct fork-fail log, which is
exactly the absence that misled me. The operator's original instinct was right;
my errno-based refutation was the wrong turn.

**The reusable lesson**: an absent error log is not evidence of absence of a
cause. Three rounds of reasoning (render-server-missing -> refuted-by-errno ->
re-diagnose) chased their own tails; one instrumented boot that printed the
actual `get_blob` return value ended it. When a mechanism has no distinct failure
signature, instrument it -- do not infer its absence from silence.

**The fix + the rigorous confirmation.** The RS binary was built from
virglrenderer 1.1.0 source and installed to `/usr/libexec/virgl_render_server`
(additive; does not touch `libvirglrenderer.so`). The proof was then re-run with
the PURE SYSTEM library -- no LD_PRELOAD, no env override -- and still showed
`host3d-map venus-ctx MAPPED`, with the device-global arm refused (the negative
control). So the instrumented lib was never load-bearing; the RS binary alone is
the fix, and thyla-pi venus is now functional for all HOST3D work.

**Cost**: four pi boots (~220 s KVM each) + one ~30 min virglrenderer source
build, ~1.9 h of pi lease.

**Decisions that needed the operator** (three AskUserQuestion votes): confirm the
errno then build the RS; instrument virglrenderer non-destructively; install the
RS to `/usr/libexec/` permanently.

**Still open**: this is rung 1a of Model B. Ahead: V-3b-1b (the hostmem-offset
allocator + the guest map via `SYS_BURROW_FROM_HOSTMEM`), 1c (the Model B ring
subtree), V-3b-2 (the SUBMIT_CMD forward of the raw venus stream + reply-shmem),
V-3b-3 (the Mesa `vn_renderer_thylacine` backend -- the thyla-keep cross-build).
A provisioning note now lives in WARP-V3-DESIGN section 0.6: any fresh venus GL
host needs the `virgl_render_server` binary, or HOST3D resource ops fail with a
bare EINVAL.

---

## 2026-08-20 — V-3b design pass: the ring Venus can't use, caught before the code

The operator chose "V-3b Venus, design first" after V-3a pushed. The pass -- two
prior-art research agents + a focused fork-resolution spike -- found the V-3
arc's foundational premise was WRONG, and caught it before a line of the
~1.2 kLOC Venus backend was written against it.

**The premise that failed.** WARP-V3-DESIGN (section 2) had it that the V-3a
coherent ring IS Venus's command ring. The spike proved otherwise, source-cited
against Mesa 25.0.7 + virglrenderer main: (1) unpatched Venus creates its ring
UNCONDITIONALLY (`vn_instance.c:320`, no gate, fatal on failure) and routes every
real Vulkan command through it -- only 4 bookkeeping commands use SUBMIT_CMD; (2)
the ring MUST be host-allocated shmem (`HOST3D`/`FD_SHM`) -- virglrenderer fatally
rejects a non-FD_SHM ring (`vkr_transport.c:201`) and Venus's driver hard-codes
`HOST3D`, refusing guest memory (`vn_renderer_virtgpu.c:1457`; host process
isolation can't deref guest sglists). The V-3a ring shipped at `f12d7317` is a
`blob_mem=GUEST`, tapestryd-consumed ring with head=producer/tail=consumer --
wrong backing AND the opposite head/tail convention from Venus's
virglrenderer-consumed HOST3D ring. It cannot be Venus's ring.

**Why this is the design pass earning its keep.** The premise was about an
EXTERNAL system's requirements (how Mesa's Venus + virglrenderer expect their
ring), and it was never verified against their source before the V-3a substrate
was designed, built, audited (three rounds), and shipped. The design pass -- not
the implementation -- caught it, before the backend was written against a ring
Venus would reject at instance creation. The reusable lesson: a design premise
about an external system must be checked against that system's source before you
build the substrate that depends on it; three green audit rounds on V-3a proved
the ring SOUND, not that it was the RIGHT ring.

**The resolution.** The fork was surfaced with the research attached; the
operator ratified Model B (virglrenderer polls a HOST3D ring, minted by tapestryd
via the V-2 hostmem path, tapestryd staying venus-agnostic -- the upstream model,
with production precedent). WARP-V3-DESIGN section 0 now records the finding +
Model B + the corrected premise. V-3a is not wasted: a valid coherent-ring
primitive for a native (non-Venus) client, its /srv/warp ring ABI surface partly
reusable -- but its tapestryd-consumer core is off the Venus path. Also settled
in the pass (fork-independent): the OWED host-side rescue (a needs_drain
serve-loop sweep) and `ops.wait` (t_poll on the fence fd -- the one backend risk,
dissolved). No code landed; the next step is the Model B implementation.

## 2026-08-19 — V-3a green on virgl, and the DoS one thread couldn't show

Resumed from a self-compact with the `1<<43` encoding fix committed (`2fb542c6`)
but its ramfs never rebuilt or re-verified on virgl. This run took it green --
and a dirty-close re-audit found a box-wide DoS that round 1, and the
single-threaded prover, were both structurally blind to.

**Green on virgl.** Rebuilt with `1<<43`, synced to thyla-pi, ran the ring gate
under KVM on real V3D: `WARP-6 V-3a GATE: VERIFIED` -- the full round-trip (map +
doorbell + feedback + fence), F2 geometry, the two-conn I-45 ownership gate, and
the I-9 re-scan discrimination all pass on virgl. The encoding is now
build-enforced disjoint (the `const _: () = assert!` over all six qid tags
compiles, which is the proof), so the class that took two GL boots to catch last
run cannot recur silently.

**The DoS the prover couldn't build (round-2 F1 [P1]).** With the encoding sound,
the owed dirty-close re-audit (Opus 4.8 fallback, MODEL start==end) re-derived the
round-1 dispositions instead of trusting them -- and F6 ("the per-kick drain
bound is V-3b's; at V-3a the guest is blocked on the kick RPC, so head is fixed
and the loop is one pass") fell. The premise conflated two actors. The KICK RPC's
caller is blocked, yes -- but `head` is not the caller's to hold still: it is
CLIENT-WRITABLE shared memory (the ring maps RW into the client via weft; the
prover itself writes `head` at `warp-prove/src/main.rs:534`), and tapestryd is
single-threaded (`main.rs`, zero thread spawns). So a client with a SECOND thread
spins `head += 64` while the first kicks: `wring_kick`'s `loop` re-reads `head`
fresh every pass, always sees `head > tail`, sets `tail := head`, and never
terminates -- one unprivileged client freezes the compositor for every other
conn. It is reachable at V-3a, not V-3b; V-3b's real submit only makes each spin
iteration costlier. The single-threaded prover cannot construct the
concurrent-advance window, which is exactly why round 1 read green -- the textbook
latent-P1.

**The fix, and a regression the prover CAN run.** Cap one kick's drain at
`WARP_RING_MAX_DRAIN_PER_KICK` (4096) passes: on the cap, publish `idle=1` and
return, and the guest re-kicks for the rest, so no one kick monopolizes the serve
thread (a legit V-3a kick drains in ONE pass, so the cap is never hit in normal
use). The regression is the interesting part: the prover is single-threaded, so
it cannot reproduce the concurrency -- so I generalized the `ring-inject` lever
from a one-shot bool to a COUNT (`ring-inject <ridx> [count]`, one advance
consumed per re-scan pass; `count==1` preserves the I-9 witness exactly). A
512 KiB ring + `count=5000` (> the 4096 cap) drives ONE kick's drain past the cap;
the leg asserts `0 < delta < 5000` (bounded), then re-kicks to stable and asserts
the full 5000 eventually drain (the cap DEFERS work, it must not DROP it). It
fails on the pre-fix code (one kick drains all 5000) and passes on the fix -- and
it passed on virgl in the gate above. Also fixed F2 [P3]: the inject arm's
`tail + WARP_RING_HDR` -> `saturating_add` (a client can set `tail` near
`u64::MAX`; an overflow-checked build would abort). Everything else round 2
re-derived sound (encoding, SeqCst, per-ring noscan, I-45 end-to-end, F5 rewind,
`PendingRingFence` lifetime, I-32, I-7).

The lesson worth keeping: a "defer to a later phase" disposition is only as good
as the actor model it rests on. "The guest is blocked so the shared word is
fixed" was true of the WRONG actor -- the caller, not the client's other threads
-- and a shared-memory word has no single writer to be blocked. When a deferral's
safety argument names one actor for a resource that several can touch, re-derive
it.

Committed the round-2 close at `07767462` (on the stop-hook synchronous-await
enforcement `76975050`, an orthogonal ratified-feedback re-land recovered this
run from a self-compact clean-tree revert).

**Round 3 (the dirty close a P1 owes) found the fix's SECONDARY claim was
overstated (F1 [P2]).** The cap-break publishes `idle=1` and breaks WITHOUT the
post-drain re-scan below it -- so it silently drops the host's half of the I-9
register-then-observe promise for any advance still pending at the cap. My fix
comment claimed "the guest re-kicks for the rest, so no work lost"; the
documented doorbell protocol never obliged the guest to re-kick. A doc-conformant
multi-threaded client that advanced head while `idle==0` (eliding its kick,
relying on the host re-scan) would strand its own advance and park on the fence
forever. It is LATENT -- the only V-3a ring client is the single-threaded prover,
which re-kicks explicitly, and a malicious client only strands itself (the DoS
bound, the fix's PRIMARY goal, is sound and effective). It materializes at V-3b's
Venus (a doc-conformant pipelined ring). Two lessons stack here: round 2 was "a
deferral premised on the wrong actor"; round 3 is "a fix that solved its primary
job and quietly shifted an unstated obligation onto a future consumer to claim
the secondary one." Fixed correct-by-CONTRACT now (the guest obligation is a
documented term at the cap-break + the const + the `wring_kick` doc, and the
prover honors it); the robust host-side rescue -- a follow-up drain the serve
loop runs after other conns -- is OWED at V-3b, where the pipelined drain
replaces this echo and a self-reschedule primitive (absent at V-3a) exists
([[design-v3b-ring-kick-rescue-owed]]). F2 [P3]: warp-prove leg 8's `flood`/`big`
were silently coupled to the server-private cap const -- pinned with a comment
both sides. Both round-3 fixes are pure comments (no binary change), so the green
gate above still holds byte-for-byte. Round-3 close: `067849b6` (the arc is userspace + docs + tools ONLY -- zero
kernel/arch/mm/specs from the pushed `60f6c929` -- so specs + the SMP gate are
non-regression on a byte-identical kernel; the ring is GL-verified + suite-green;
the interactive gate is the userspace boot confirmation). Everything else
re-derived sound across three rounds.

**The close (this run): green, documented, pushed.** The interactive gate
(LS-CI, `brs0ccizd`) went 37/37 -- the last push-bar gate. The vault was rung:
`sub-tapestryd` gained "The coherent ring lane" (the mechanism is vault-OWNED --
server.rs/gpu.rs -- so the prose belongs there, not in the reference doc), plus
`inv-i9` in guarded-by and a Tests pointer; the prover binary `usr/warp-prove/src`
is UNOWNED, so its ring-verb reference went to `docs/reference/149-warp.md` and
the coverage decision is filed as `seam-warp-prove-unowned` (vault commit
`6da4b11e`, on the local-only `vault/bootstrap` branch). The whole stack pushed to
both mirrors and was verified by ls-remote on each URL: `85526127`. The
reference/vault split is the reusable part -- `quaestor owner` returns MIXED on a
mechanism+prover diff, and BOTH actions are owed, not the one the exit status
names.

## 2026-08-19 — V-3a: the coherent ring, and the "local" premise that wasn't

Built the Warp-6 V-3a coherent-ring mechanism whole in one pass (the design
`60f6c929` said it does not decompose into stubs, and it doesn't): the
`ctx/<id>/ring/<ridx>/{info,map,kick,fence}` subtree in tapestryd
(`usr/tapestryd/src/server.rs`) -- a weft-shared, coherently-mapped GUEST blob
with a control header (head/tail/idle/seq), the doorbell with the I-9
register-then-observe re-scan, the fence feedback slot + a blocking fence file,
F2 geometry validation (refused-not-clamped), the I-32 backing charge, and the
I-45 owner gate. Plus a `warp-prove ring` client exercising the round-trip + F2
+ I-45 + the I-9 discrimination (a `ring-inject`/`ring-noscan` test-lever pair:
an injected mid-drain head advance is DELIVERED with the re-scan and LOST
without it). Compiles clean, zero new warnings.

**The wrong turn, and what caught it.** The design's sub-chunk table called
V-3a "local, no builder." The first local run hung silently: `warp-prove ring`
produced NO output for 90 s, three deterministic attempts. Ground-truth-first
(no theorizing): a warmup `echo` proved ut runs commands, isolating the failure
to `/warp-prove ring` specifically; stripping ANSI from the raw console showed
the command ran and returned to a clean prompt having printed nothing. The
decisive read was the device banner -- `virgl=0 blob=0` -- and `server.rs:8127`:
`ctx/new` is virgl-gated (`E_OPNOTSUPP` on a 2D device), the twin of the SUBMIT
gate. The ring lives UNDER a warp ctx, so **the mechanism cannot be minted on a
2D device at all** -- the "local, no builder" premise was wrong. (The silent
"hang" was actually a fast clean exit whose prover output never appeared,
because I first ran it as `/warp-prove` -- an absolute path -- and the relative
`warp-prove` form ut resolves via PATH worked and printed everything: a separate
ut absolute-vs-relative exec oddity, enqueued, not chased.)

**What that means, exactly.** V-3a's mechanism proof needs a virgl DEVICE (the
GL host), not local 2D. The local 2D path is now proven-GRACEFUL: the prover
prints `RING SKIP -- no virgl on this device (ctx mint unavailable)` and
tapestryd does NOT hang (`ctx/new` fails clean). A local "deviceless ctx" test
lever was considered and REJECTED -- it would green an unconstructed state (a
configuration production's 2D devices can never reach). The test moved to
`tools/warp/warp-ring.exp` (GL host, via `tools/warp-host.sh ring`), mirroring
`warp-prove`; the design doc + this journal record the correction.

**The GL-host loop, and two encoding traps only virgl could catch.** The
prosecutor (Opus 4.8 fallback, MODEL start==end) INDEPENDENTLY found the headline
P0 -- `WARP_RING = 1<<37` collides with the 30-bit id field, so `warp_id` can't
round-trip a ring path and nothing resolves -- the same bug the first GL boot hit
(`ring ctx minted` then `open-for-read` on `ring/0/info`). Its suggested fix
(`1<<40`, "bits 40/41 are free") was ALSO wrong: `1<<40` is `SURF_FLAG`, so
`is_surf(ring)` went true and the walk misrouted to the surface arm -- the second
GL boot failed identically. Ground truth pinned it: `say!` diagnostics showed
the ring minted + the ridx walk resolved, but the `info` walk arm never fired,
because an EARLIER `is_surf` arm swallowed it. The real fix is `1<<43` -- bits
38..42 are ALL taken (WARP_BO/WARP_CTX/SURF_FLAG/PANE_FLAG/WARP_FLAG) -- plus a
`const _: () = assert!` that now checks all SIX qid tags mutually disjoint, the
guard whose absence let both my `1<<40` and the reviewer's suggestion through.
The lesson: a qid-tag-bit choice must be checked against the WHOLE tag namespace
(surf + pane + warp), not just the `WARP_*` half; and 2D-local testing is
STRUCTURALLY blind to it (2D SKIPs before a ring resolves). The other 6 findings
were dispositioned: F2 (I-9 SeqCst doorbell + a documented store-buffer contract,
replacing the AArch64-only Acquire/Release), F3 (per-ring `ring-noscan`, not a
global box-wide I-9 kill switch -- the #178 shape), F4 (a two-conn I-45 OWNERSHIP
test replacing the liveness-only one), F5 (VA rewind on the mint failure arms),
F6/F7 (documented: the drain bound is V-3b's, the seq wrap is the shared class,
`wctx_of_conn` is unambiguous by one-ctx-per-conn).

**Verified so far:** the mechanism COMPILES clean, the local 2D graceful-skip
path is runtime-confirmed, and the ring now MINTS + RESOLVES on virgl (the walk
reaches `info`). **NOT yet green:** the full round-trip + F2 + I-45 + I-9 on
virgl -- the `1<<43` rebuild was blocked mid-run by mac contention (aux's ~53m
pts trace), so the re-verification is the immediate next step. Nothing is pushed
until it is green on virgl.

## 2026-08-19 — V-2: host-visible memory, and the death path a shared BAR opened

Two threads. First, a stray `/compact`: the operator saw two `/compact` lines
after a self-compaction and asked which agent issued the second. Ground truth
(the selfcompact ledger + both scripts) showed it was neither an agent nor the
nudge watcher — it was a *premature* self-compact cancelled earlier at 560k,
whose Enter-queued `/compact` a `tmux send-keys C-u` never actually retracted; it
rode the input queue ~4 hours and fired against the already-compacted session (a
harmless "Not enough messages"). Landed as contract (`19103efe`): a queued
self-compaction is NOT yours to cancel — only the operator's (raise a blocking
question); invoke the script only on the real 600k signal. While in the ledger I
found the belay gate keyed on the mutable `@thyla-role` tag — main's compacts
logged as `aux`, colliding with aux's state and silently defeating the governor;
rekeyed it on the git toplevel (`83c7f56d`).

Then **V-2** — the first kernel memory-authority path of the Warp-6 arc: map a
subrange of a PCI hostmem BAR (Venus HOST_VISIBLE memory) into a client VA. The
ratified design (6.2.1) was wrong about the tree in two places:
- It said "add the NORMAL_NC MAIR index." The recon measured it: NC has been in
  the MAIR since P1-C (index 1). V-2 *plumbs* it — widening the fault path's
  `bool device_memory` to a MAIR index — and adds no byte. A design claim wrong
  about the tree, caught by ground truth, not by re-reading its prose.
- It said the client map "rides the existing SYS_WEFT_SHARE." The code showed the
  weft path fail-closes on unknown burrow types AND carries a duplicate admission
  gate that "MUST widen together" (its own comment, from the Warp-2b bug).
  Delivering a client mapping meant wiring the I-37 weft kind-machinery — more
  than "one syscall." Surfaced as a scope fork; the operator chose to complete it
  in V-2 (both gates widened in lockstep, `WEFT_BIND_HOSTMEM`).

The widening carried a footgun: `false == 0 == MAIR_IDX_DEVICE`, so a naive
bool->index widen would silently map every existing `false` caller as Device.
Handled by keeping `mmu_install_user_pte(bool)` as a semantics-preserving wrapper
over the new `_attr(u32)` — zero churn on the ~13 callers, no inversion.

The Opus holotype round (Fable out of credits) closed **0 P0 / 1 P1 / 1 P2 / 3
P3**, verifying the whole bounds/lifetime/W^X/charge/lockstep core sound. The P1
(F1) is worth recording: V-2 introduces the first cross-Proc-shareable
*PCI-BAR-backed* Burrow, and on the owning server's DEATH the unconditional
device quiesce clears the BAR's MEM decode under a client's live mapping. The
prosecutor refused to guess the terminal severity — an EL0 access to a quiesced
RAM-backed BAR is either benign garbage or a box-fatal external abort — and said
measure it, not reason it away. Surfaced as a design fork; the operator chose the
partial-quiesce fix: on death, for a claim with a live hostmem burrow, clear
BUS_MASTER (stop the dead device's DMA) but KEEP MEM_SPACE, deferring its clear
to the last unref — so the client never observes a decode-disabled BAR and the
measurement is moot. F2 (the handler's bounds had no test) was closed by
extracting a pure `hostmem_resolve_subrange` + testing it; F3/F4/F5 tracked P3.
Re-audit of the fixes: CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3 cosmetic; Opus 4.8 fallback -- Fable out of credits). Suite 1431/1431; commit 7973f8dc. Merge follow-ons (71306b60 + the libthyla/gate close): P3-1 landed the /proc/maps hostmem arm; the SMP gate PASSED (40 boots, 0 corruption across default+UBSan x smp4/smp8), the burrow/weft buggy cfgs FIRED and the clean cfgs stayed green, LS-CI console PASSED; the libthyla-rs ABI mirror (107) landed. The GL venus regression was DEFERRED, not failed: the thyla-pi LAN mDNS name stopped resolving mid-run -- a sync ssh wedged 36 minutes on its first mkdir, a bounded probe returned nodename-nor-servname, and the Cloudflare tunnel then proved the pi healthy (up 7 days, idle). venus is not in the push-bar and V-2 new code is unexercised until V-3, so the push proceeds; venus reruns when the LAN name resolves (or via the CF tunnel).

What V-2 does NOT ship: a real client. The weft delivery is exercised only by
unit tests — V-3 (vn_renderer) drives it E2E on real hardware, where the residual
P3s land with a driver to exercise them.

## 2026-08-19 — V-1: a guest blob creates, and the scope hidden in "blobs"

Resumed from my own self-compaction; the resume note ordered V-1 (blobs) next.
The ladder names V-1 "blobs (`RESOURCE_CREATE_BLOB` + the blob object model)",
which reads as a large chunk. Reading the design collapsed it to something
smaller and sharper.

The load-bearing fact is in GPU-DESIGN §2.4: **Venus's command ring is a guest
blob** — its head/tail/status cachelines are guest pages the host also reads.
That is why V-1 is Venus's real prerequisite. But "guest blob" is the whole
point: a guest blob's storage *is* its own guest `mem_entry` pages — the host
registers a resource referencing them, with no host allocation and no hostmem
BAR. The host3d blob (host-allocated storage the guest reaches through the
hostmem window via `MAP_BLOB`) is a *different* thing, and it is exactly the V-2
delta the reference already flagged (149-warp "Mapping a subrange is the §6.2
Venus-chunk delta"). So V-1 is the guest-blob *create* path — nothing maps,
nothing is coherent yet — and it rides the existing venus gate's two legs
unchanged: the venus device offers `F_RESOURCE_BLOB`, the plain `-gl` control
does not. The whole chunk is a tapestryd-side device command; no kernel path
(that arrives at V-2, which maps MMIO into a client VA).

Two wrong turns, both caught before they cost anything.

First, the opcode. I reached for `RESOURCE_CREATE_BLOB = 0x0212` from memory —
and it is wrong. Counting the virtio-gpu 2D enum forward from the code's own
anchor (`GET_CAPSET = 0x0109`, already in the tree) puts it at **0x010c**
(`GET_EDID` 0x010a and `RESOURCE_ASSIGN_UUID` 0x010b sit unused between). 0x0212
was a confabulation. The "a number recalled is a number unverified" rule earned
its place again — I verified against the tree's anchors, not memory.

Second, a lifetime bug in my own probe (self-audit SF1). `blob_probe` backs the
blob with a dedicated one-page DMA and unref's it, then the buffer Drops
(unmaps + frees the pages). If the *unref* fails while the engine is alive, the
host may still reference those pages — and Drop would unmap them out from under
a live reference. The probe issues no transfer so it is theoretical, but the
correct discipline is to **leak, not unmap, under a live reference**: one page
at init beats a UAF. `core::mem::forget(backing)` on the unref-fail path.

I also heeded a prior lesson rather than re-learning it: `init_device` returned
a positional `(u64, bool, bool)`, the exact shape that let V-0b's `ctxinit` go
briefly unreturned. Adding a third bool to a positional tuple is how that bug
happens again, so the three feature flags now ride a named `DevInit` struct.

The probe's resource id (`0x2b`) is collision-free by the same timing argument
the ctx-capset probe uses (it runs before the Server exists) plus a numeric
guard: the server mints ids from `SCREEN_RES + 1` upward and never down, so any
id `<= SCREEN_RES` is unmintable forever. I sabotaged the guard to prove it
fires — `id = 0x40` fails the build with the guard's message, `0x2b` compiles.

It creates. On thyla-pi (KVM, real V3D 4.2): `blob-create guest CREATED` with
venus, `blob-create skipped (F_RESOURCE_BLOB not offered)` on the control, and
the venus leg boots fully clean with the feature negotiated — so negotiating
blob does not disturb the compositor path (a self-audit worry, answered by the
boot). VENUS GATE VERIFIED, `test-venus-verdict` 13 → 16 arms, all discriminating
without a boot.

One measurement worth keeping for the next GL run: the control boot took **268s**,
not the ~220s the notes cite. A combined `warp-host.sh venus` run (both legs in
one call) would have been ~536s — close enough to the 600s foreground cap that a
slightly slower host would have moved it to a background task and killed the
second boot mid-run. Running each leg as its own sub-600s call was the right
call, and the number says why.

The prosecutor round closed **CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3)** on the Opus
4.8 fallback (Fable was out of credits — the round is a real degradation on the
independence axis, family-shared with the author, and it said so; a Fable re-run
is not owed because it finished). It caught one thing worth the round on its own:
**F1**, an inconsistency in my *own* SF1 fix. SF1 leaks the backing on a failed
unref (the host may still hold the pages); but the sibling branch — a create
that fails because the *engine died* — Dropped the backing, and a deadline-dead
create was already *published* (the doorbell rings before the wait), so the
device may equally hold that PA. Two branches, opposite dispositions, one
principle. Fixed to leak on both. Inert today (the probe issues no transfer, and
the dead path triggers a proc-death device reset), but it is exactly the kind of
disagreement that reuses the wrong disposition at V-3, where transfers exist. The
round also filed two forward notes: **F2** (V-3 must validate a client's
`pa`/`len` before they become a host `mem_entry` — an I-45/I-32 boundary) and
**F3** (when V-2 adds host3d, the gate should assert the blob mem-type from
evidence, not the hardcoded "guest" string).

The operational miss, recorded because the catch is the reusable part: partway
through the run the **host went to sleep**. It killed the prosecutor mid-response
("your computer went to sleep") and hung an LS-CI chunk into a 590s timeout doing
nothing — and I had forgotten `caffeinate`, the exact trap
`feedback_caffeinate_long_tasks.md` names. The tell was two failures at once with
one cause; the fix was a background `caffeinate -dis` plus `caffeinate -i` on
every LS-CI chunk, after which the heavies ran to 468s clean. The prosecutor's
partial output before it died was already a real finding (the missing runtime
guard on `resource_create_blob`), so the sleep cost time, not correctness.

A note on what "37/37 on the shipped binary" actually rests on: the guard and F1
are provably **unreachable** on the 2D device LS-CI boots (`blob_probe` is
virgl-gated, so `resource_create_blob` is never called there), so the 26
scenarios I ran before the fixes are byte-identical to the final binary, and I
re-ran only the remaining 11 on it. The venus gate I *did* re-boot on the final
binary directly — the test leg exercises the guard (which falls through, since
`self.blob == true`) — rather than lean on the same unreachability argument for
the load-bearing claim.

SMP stands (kernel byte-unchanged). Ahead: V-2 (host3d + the hostmem-BAR mapping,
the first real kernel memory-authority path of the arc) → V-3
(`vn_renderer_thylacine` + the coherent ring) → V-4/5/6.

## 2026-08-18 — V-0b: a Venus context creates, and the seam size I recalled wrong

I had classified V-0b as blocked this session — the arc's next step is
audit-bearing `gpu.rs` work and I'd been treating the Agent tool as barred. The
Stop hook pushed back: a checkpoint is not a stopping point, and the standing
operator grant (`feedback_prosecutor_agents_permitted.md`) authorizes the
`holotype-reviewer` for exactly this. So I opened it.

The question V-0b answers is narrow and real: V-0 proved the host *advertises*
capset id=4; it did not prove a Venus *context* can be created. That gap mattered
because `/usr/libexec/virgl_render_server` is in no Debian package, and §9.2
calls the render server Venus-only-by-construction — so "the capset is
advertised" could have meant venus init reached capset reporting and no further.

It creates. On thyla-pi (KVM, real V3D): `ctx-capset id=4 CREATED` with venus,
`skipped` without, `id=2` virgl the positive control on both legs. The absent
render server does not block it — virglrenderer's in-process venus init handles
context creation. That is the empirical answer the inference could not give.

The design point worth keeping: this is a **feature-bit** change, not a field
change, and the naive version is a *convincing* false pass. `ctx_create` wrote
`context_init = 0` under a comment saying the feature was not negotiated, and the
device ignores that field unless `F_CONTEXT_INIT` is negotiated — which the
driver never offered back. So "pass capset 4 and see" would have written into an
ignored field, collected `RESP_OK_NODATA`, and produced an implicitly-virgl
context reporting success. The negative control is what proves we avoid it: on a
no-venus boot the id=4 create is *skipped* because the capset was not enumerated,
never spuriously CREATED.

Then my own self-audit, run beside the prosecutor, caught me doing the exact
thing this whole run has been about. My commit message and code comment said the
probe's ctx ids (200/201) sit "above the client range (slot+1, <=128)". The
client range is not 1..128. `MAX_WARP_CTXS = 8` — one grep away — so it is 1..8.
The collision-safety conclusion holds (200/201 are far above 8 and below
`COMPOSITOR_CTX` at 0x100), but I cited a number I recalled instead of the one in
the tree, and the "128" is a real but *different* limit from Warp-3a. A number
recalled is a number unverified; the session's refrain, landing on me one more
time. Folded the correction into the round's disposition rather than amend under
a running reviewer.

Committed at `bf448929`, **not pushed** — `gpu.rs` is an audit-trigger surface
and this changes the device negotiation contract plus adds context creation, so
the round runs before the push. Fable was out of credits, so the round is on the
Opus fallback tier at max effort — context-independent even if same-family,
which is what the fallback rule preserves.

**The round closed CLEAN -- 0 P0 / 0 P1 / 1 P2 / 2 P3 -- and it converged with
the self-audit.** F1 (the "128-slot seam" that is really 8) was my SF1; F2 (the
debug_assert that vanishes in release) was my SF2. Two independent prosecutors,
the same two findings -- the reassurance the discipline is designed to produce.
The round added the part I had left as prose: F1 is not just a wrong comment, it
is a *missing compile-time guard*, because collision-freedom was argued from a
numeric window (liftable) instead of from timing (the probe runs before any
client and destroys before returning, which cannot be lifted). Fixed both ways:
the comment states the timing guarantee, and a `const _: () = assert!(...)` ties
the probe ids to `MAX_WARP_CTXS`/`COMPOSITOR_CTX` so a future seam lift past 199
fails the BUILD. Sabotaged it (probe id -> 5) to confirm it fires, then
reverted. F2 I closed early rather than deferring to V-3: the debug_assert
became a real `return Err` so a client-influenced capset in a release build
cannot silently mint a wrong-kind context. F3 was the round's own -- the gate
control leg asserted absence of "id=4 CREATED" without presence of "id=4
skipped", a negative a broken fixture satisfies -- now paired.

Honesty note the round pressed and I am keeping: it ran on **Opus 4.8**, a step
below the intended Opus-5 fallback (the `model: opus` override resolved low),
and it said so itself. A finished fallback round is closed per scripture, so no
re-run is owed -- but the tier is on the record, and the convergence with an
independent self-audit is what carries the confidence, not the tier alone.
---

## 2026-08-18 — An owed test, and the audit premise that was wrong when written

The extinction round (`5de6093f` F2) left an owed item: exec's failure
diagnostics were "compile-verified and never executed", because "no boot log
contains a single `exec:` line". I went to close it and found the premise was
half wrong — which is worth more than the test.

`exec_report_fail` was **already covered, and had been for seventeen days when
the round ran**. `test_execve_failed_load_leaves_target_drainable` (2026-08-01,
`e47bfa31`) drives a W+X-union failure and emits a real `exec:` line that sits in
the current suite boot log. The round's measurement — "no `exec:` line" — was
simply false when it was written. I know because I wrote it, and I did not
re-check it before turning it into an owed item.

`exec_say` was the actual gap: the dynamic-Linux-binary and dynamic-PT_INTERP
rejects had no test and appeared in no log. Genuinely never executed — the #244
class exactly, a diagnostic whose only witness was that it compiled.

Closing it was small: an ELF with a PT_INTERP naming a musl loader makes
`elf_load` return `HAS_INTERP` and `elf_brand_hint` answer `LINUX_LIKELY`, so
`exec_load_body`'s native arm runs `exec_say` and rejects the load. The suite
boot log now carries `exec: dynamic Linux binary rejected — ...` where before
there was nothing, which is the direct witness that `exec_say` runs without
faulting. Suite 1427 → 1428.

The reusable part is not the test. It is that **an audit finding's premise is a
claim about the tree, and it decays like any other.** This one asserted "never
executed" on top of a measurement that was already wrong, and the owed item
inherited the error. It is the same failure as the three throwaway verifiers
earlier in the run and the "currently broken" cross-reference before them: a
statement about what the tree does, trusted because someone once checked it,
that nobody's step re-checks. The whole session kept landing on one lesson from
different directions — a check is only worth the last time it actually ran.

---

## 2026-08-18 — The gate refused the host, and it was right to

V-0's remaining half was to stop *assuming* thyla-gl and boot it. Both halves
are now closed, and the interesting part is that the first attempt **failed**.

**The gate said UNVERIFIED, and the reason was real.** On thyla-gl's own Aug-12
artifacts, tapestryd **hung** under `venus=on,blob=on,hostmem=256M` — `warden:
tapestryd gave no readiness/exit signal -> terminating`, three restarts, `gave
up after 3 restart(s)` — while the control leg, same host, same build, came up
clean. A hang, not a crash: `Readiness::Timeout` means neither signalled nor
exited.

Two explanations suggested themselves, and both died by measurement rather than
by argument, which is the only reason I trust the third:

- *"the Aug-12 build predates #166's oversized-BAR skip."* Refuted in one
  command: `git show 534f3869:usr/lib/libthyla-rs/src/hardware.rs` carries the
  identical `if bar.size > PCI_BAR_VA_STRIDE { continue; }`, comment and all,
  and `git log -S` dates that code to 2026-06-15.
- *"lavapipe is slow to enumerate, so venus init stalls the control queue."*
  Weakened: `vulkaninfo --summary` returns in **248 ms** on that host, and
  `SUBMIT_DEADLINE_MS = 500` already bounds our controlq wait — so whatever hung,
  it was not our driver blocking forever on a device response.

Syncing the current build and re-running the same host with the same declaration
came up clean and VERIFIED. **So the attribution is the stale artifacts, not the
host** — but one sample each way across two different builds is not an
explanation, and I have written it down as unexplained rather than let "current
build works" quietly become "we know what that was." There is nothing to fix in
the tree, which is a different statement from knowing why.

The gate behaving correctly under a real failure is worth as much as the pass:
it refused to promote a host that could not show the capset, and it named the
reason.

**The driver was throwing away the answer to the arc's next question.**
`gpu.rs` reads `dev_feat_lo` during feature negotiation, uses exactly one bit of
it (VIRGL), and discards the rest. So "does this host offer `CONTEXT_INIT`?" —
the question that decides whether a Venus context is reachable at all — had no
answer short of writing a new build, about a value the driver already had in a
register. One `say!` line fixed that, and it immediately changed what V-0b *is*:

`CONTEXT_INIT` turns out to be offered on a **plain `-gl` device**, no venus and
no blob required. Meanwhile `ctx_create` writes `context_init = 0` under the
comment "F_CONTEXT_INIT not negotiated" — and the device honours that field only
when the feature is negotiated, which this driver never offers back. So the
obvious form of V-0b — pass capset 4 and see — would have written a 4 into a
field the device ignores, collected `RESP_OK_NODATA`, created an
implicitly-virgl context, and reported success. **A false pass, and a
particularly convincing one.** V-0b is a feature-bit change.

The same line settled V-1's host question for free: `RESOURCE_BLOB` appears only
with `blob=on`, and the default dev device offers neither (it is `virgl=0`), so
blob work cannot be exercised on the local dev loop at all. That is #166's
inert-hostmem-under-HVF constraint wearing different clothes, and it is the
concrete reason promoting thyla-gl was worth a morning.

**And a hole in my own gate, found by prosecuting it rather than admiring it.**
The gate asserted "the control leg does NOT see capset id=4". A control that
measured *nothing* — virgl not negotiated, 2D fallback, no capset lines at all —
satisfies that trivially, and the gate would read "venus absent" where the truth
is "capsets absent". That is the standing lesson about negative assertions and
broken fixtures, reappearing **inside the very gate I wrote to honour the
discrimination rule**: I had put the control in the *boots* and forgotten to
require that the control leg had measured anything. It now demands the baseline
pair (`id=1` and `id=2`), with two sabotages for it. 5/5 became 7/7.

Re-verified against the real thyla-pi logs from the passing run — still VERIFIED
under the strengthened verdict, so no re-boot was owed for that.

Both hosts, finally, return **byte-identical feature words** (`0x30000013`
without venus, `0x3000001b` with) — a cross-host agreement the arc did not need
but is better for having.

**Postscript, because repeating a pinned lesson is worth more written down than
quietly fixed.** Going into the pre-push bar I ran every TLA+ spec through a
one-liner that declared a spec green iff `tail -3` of its output contained
*"Model checking completed. No error has been found."* Every spec came back
FAIL. The specs were fine: TLC prints that line about twelve lines in and
finishes with state-graph statistics, so my verdict window could never contain
the string it was looking for. **A guard on the reporting path fabricating the
defect it reports — key on the exit code, never the prose** — is already an
M-PIN in this project's memory, and I wrote the same bug anyway, in a checker I
composed in one line because it felt too small to get wrong.

Two things follow. The pinned lesson does not fire from *reading* it; it fires
from noticing the shape "I am grepping prose for a verdict", and that shape is
easiest to miss in throwaway code. And the tell was available immediately:
*every* member of a large set failing at once is almost always the classifier,
not the set — which is itself the other half of a pinned lesson ("when ONE
member of a family misbehaves, suspect the classifier"; here it was all of them,
which is even louder). Confirmed in one command: exit code 0, success line at
line 12.

The run was not owed in the first place — clean-cfg TLC has been suspended since
2026-05-21, and a `say!` line in a virtio driver touches no modelled mechanism —
so the whole excursion cost ten minutes to learn something about my own reflexes
rather than about the specs.

**And then it happened twice more in the same session, which is the actual
finding.** (2) A shell loop meant to re-verify three real log pairs under a
changed predicate reported all three FAILING with an empty verdict string; run
directly, every one passed — the loop's `$?` was not measuring what I thought.
(3) A one-liner checking that my new documentation tables were not broken
flagged the GPU-DESIGN row as suspect, because I had hard-coded the pipe count
of a *four*-column table onto a *three*-column one; every sibling row had the
same count, so the doc was fine and the checker was not.

Three throwaway verifiers in one session, three false alarms, zero real defects
among them. Each was caught the same way — by checking the surprising result
against a known-good reference before acting on it — and none cost more than
minutes. But the shape is worth naming, because the pinned lessons are all about
distrusting *gates I build deliberately*, and every one of these was a scrap of
shell I wrote in passing to confirm something I already believed. **The care I
give a committed checker does not automatically extend to the one-liner that
checks it**, and the one-liner is the one nothing else will ever review.

The practical rule that fell out: when an ad-hoc check reports that *everything*
failed, or that something I just verified by hand is broken, the first suspect
is the check. That is the same instinct as the pinned "when one member of a
family misbehaves, suspect the classifier" — it just has to fire for code that
never gets committed.

---

## 2026-08-18 — Warp-6 opens on a probe, and the blocker that wasn't

Warp-C closed, so Warp-6 (Venus) is next. `GPU-DESIGN.md` §9.1 makes the first
move non-negotiable: *"Nothing can be **run** locally. This must be settled
before code starts, not discovered after."* So the arc opens with a gating
probe, the Warp-C C-0 shape, and `vn_renderer_thylacine` waits.

**The measurement, with its control.** Two boots on thyla-pi differing in the
device declaration alone. Control (`virtio-gpu-gl-pci`): capsets `id=1`, `id=2`.
Test (`+venus=on,blob=on,hostmem=256M`): additionally **`id=4` — VENUS,
`max_version=0`, `max_size=160`**. Both legs `BOOT: PASS` (215–225 s under KVM),
which is the part that makes it evidence: had the control merely failed to boot,
the missing capset would have been attributable to that instead of to the
declaration.

**No guest change was needed, and I nearly bought a boot to learn that.**
`probe_capsets` (`usr/tapestryd/src/gpu.rs`) already enumerates to
`GPU_CAPSET_ENUM_MAX = 8` and prints one `gpu capset[N] id=..` line per index.
My first grep filtered them out — the pattern was `GET_CAPSET`, and the lines
say `gpu capset[`. The evidence was on disk in the logs I had already produced.
A pattern that matches the wrong thing returns a confident partial answer, not
an error; the tell was that a boot which *did* enumerate three capsets reported
nothing about what the third one was.

**QEMU documented its own requirement better than I would have.** `venus=on`
alone is refused, and so is `venus=on,blob=on`, both with
`venus requires enabled blob and hostmem options`. Only the triple realises.
That is a **realise failure, not a degradation** — a caller declaring less does
not get "GL without Venus", it gets no device, and must not read that as a
negative Venus result. It also settles V-2's position in the ladder by
measurement rather than judgement: hostmem cannot be a late refinement of a
chunk whose device will not come up without it.

**The blocker that wasn't, and why it is written down anyway.** The host's
`libvirglrenderer.so.1.9.0` carries Venus (`VK_MESA_venus_protocol`,
`vkr_ring_thread`, `vkr_dispatch_vkWaitVirtqueueSeqnoMESA`) and names
`/usr/libexec/virgl_render_server` as `RENDER_SERVER_EXEC_PATH` — **and Debian
ships that binary in no package**; `virgl-server` is the unrelated *vtest*
server. §9.2 calls the render server Venus-only-by-construction, which reads as
"no server, no Venus", and for about ten minutes I had a dead arc. The capset is
advertised regardless, so venus initialises in-process at least far enough to
answer a capset query.

The discipline point is what I did **not** then write. "Venus works on
thyla-pi" is not what was measured. What was measured is that venus init reaches
capset reporting; whether a *context* creates is a different claim, and the
render server could still bite there. That became V-0b (`CTX_CREATE` with
`capset_id=4`) — a rung that settles it empirically instead of by inference in
either direction.

Instrument note worth keeping: `nm -D --defined-only` finds **zero** venus
symbols in that library, because they are internal. Had I run the export census
first and stopped there, I would have concluded Venus was absent from a library
that plainly contains it.

**The measurement was then made into a gate, because a hand-run measurement is
not one.** `warp-host.sh venus` runs both legs and asserts the discrimination in
**both directions** — present with the declaration, absent without. One
direction is not enough: "the test leg saw `id=4`" is satisfied by a host that
advertises the capset unconditionally, and by a guest printing a line it never
derived from the device.

Then the gate's own problem: it costs two ~220 s remote boots, which makes its
verdict the least affordable thing in the tree to test by running it — and #245
is three days old and says exactly what happens to a checker reachable only by
hand. So the verdict is its own verb (`venus-verdict`), and
`tools/test-venus-verdict.sh` drives **the real implementation** against crafted
logs: five cases, four one-variable sabotages plus the clean pair. The clean
case is not decoration — without it, four negative cases are satisfied by a
verdict that always fails. `5/5, DISCRIMINATES`, wired to `make
test-venus-verdict` and into CLAUDE.md's command block, which #245 measured to be
the property that actually prevents rot.

**Open, and named as open.** thyla-gl (Parallels, lavapipe) has the same QEMU
10.0.11 and a venus-carrying virglrenderer but has **never booted with
`venus=on`** — it is checked to the property level only, and promoting it is
V-0's remaining half. It matters beyond tidiness: if it works, Venus has a fast
local-ish iteration loop; if not, the whole arc iterates over the Pi's SD card.

The V-0..V-6 ladder is now in GPU-DESIGN §12, and V-2 is flagged audit-bearing
on I-45 and I-32 *independently of the rest of the arc*, because mapping MMIO
pages into a client VA is a new kernel memory-authority path and not a graphics
detail.

**And then the wrong turn, caught about twenty minutes after it landed.** I
wrote V-2 as carrying "the `PciDev::claim` eager-map-every-BAR fix, pulled
forward as a dependency" — because §6.2 ends with *"Also required and currently
broken: `PciDev::claim`'s eager map-every-BAR policy (§3)."* It is not broken.
It was fixed at **Warp-2a (#166)**, and §3 — **the section §6.2 points at** —
has said `[FIXED at Warp-2a (#166)]` in bold for weeks, along with the exact
remaining delta: *"Mapping a subrange of the shm window remains the §6.2
Venus-chunk delta."*

What caught it was not re-reading the doc. It was going to look at the tree for
an unrelated reason — how big is V-2, really? — and finding
`kernel/pci_handle.c` already resolving `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`, a
`pci.walk_caps_shm` test passing in the boot log I already had open, and
`hardware.rs` carrying a `#166` comment at the exact line that skips an
oversized BAR.

Two things worth keeping. First, **a cross-reference pointing AT the correction
is not the same as being corrected** — §6.2 pointed straight at the section that
refuted it, and the pointer kept its own verdict; a reader who follows the
pointer has already believed the pointer. Second, **a "currently broken" note in
a design doc is a claim about the tree, and it ages exactly like a status field:
nobody's step flips it.** The fix's own commit updated §3 and did not think to
hunt the other half of the sentence one section away.

So V-2 is **smaller than I wrote it**: discovery is done, the claim policy is
fixed, and what remains is the mapping half alone — an owner-minted,
client-mappable, revocable, budgeted map of a *subrange* of the shm window at
the host-dictated cache attribute. Corrected in §6.2, §12, and the status row;
the original claim is left visible in §6.2 rather than quietly overwritten, so
the next reader can see which half of a self-contradicting document was stale.

---

## 2026-08-18 — A reroute from a blocking primitive to a dropping one, and the budget I left behind

The audit `extinction.c` owed — it is a declared trigger surface and #246 put a
fault-injection hook on it — came back **0 P0 / 1 P1 / 2 P2 / 4 P3**. Clean by
the numeric rule. F1 was mine and had to land before merge.

The round opened by naming its own degradation rather than reciting a caveat:
the code was Opus-authored and so was the reviewer, so **family diversity is
forfeit here** and only context independence survives. It then used that
independence properly — it re-derived the EL1-sync depth ladder, measured the
shell predicate against twelve adversarial inputs, and **withdrew two of its
own prosecutions** against the code.

### F1: I moved the diagnostic and left the accounting where it was

`uart_puts` spins per byte and always emits. `cons_diag_line_emit` is
**all-or-nothing** and drops silently. I swapped the first for the second and
left the dedupe bit and the report budget being consumed *before* the emit.

Under back-pressure from a guest writing `/dev/cons` — the room-wait wakes on
**one** free byte and immediately refills, so the 8192-byte ring sits at
capacity — a 107-byte all-or-nothing unit never fits. So the drop is not racy,
it is **deterministic**, and it is the regime a container bring-up produces.
The syscall number is then marked seen forever and the budget is one lower. The
census under-reports and still reads as a measurement.

That is verbatim the failure the function's own header says the per-Proc rework
existed to kill: *"worse than no diagnostic, because it reads as a
measurement."* I re-opened it one step down, by changing the primitive and not
re-examining what was spent around it. **A reroute from a blocking primitive to
a dropping one changes the failure mode of every budget spent around it.**

The emit now reports whether the unit landed; the bit and the cap are taken
only when it does, so a dropped line is retried on the next decline.

### F2: I fixed the bounded emitter and left the unbounded one

The commit's own headline was "route the EL0-triggerable diagnostic through the
ring", singular. `exec_report_fail` is five raw calls, twice per failed spawn,
with **no dedupe and no cap**, and every `SYS_SPAWN_*` reaches exec through it —
so an unprivileged Proc spawning a malformed ELF in a loop drives it at will.
Strictly worse than the site I closed, and the severity ordering was inverted
relative to the fix that landed.

Converted, with a **global** cap rather than per-Proc: a per-Proc bound is
re-armed by spawning, which is the attack. The old comment defending the raw
loop ("to stay non-blocking") no longer selects it — `cons_diag_line` is also
non-blocking, never spins, and takes no console role — so that sentence went
too.

### F3: I wrote the lesson and then didn't apply it

My commit said *"a set with four independent spellings has no spelling anything
can be checked against."* I then enumerated only the file I was already
editing. Six more spellings were stale: two in `CMakeLists.txt` (a cache
docstring at four-of-eight, a comment block reading as complete at
three-of-eight), two in a **binding** reference doc at three-of-eight, and a
Makefile help line saying "seven" and "7 boots" against eight — in a line I had
just tagged `#245`. All now point at `ALL_VARIANTS` instead of re-duplicating,
because duplication is the thing that rotted.

### F6: the arm my test reached but could not fail on

I claimed the hook's placement put `cons_tx_claim_for_dump`'s
already-owned-by-this-cpu arm under test. It *reaches* it. Delete that arm and
the re-entrant claim burns its bound, returns false — and the banner still
prints, because the miss path is "torn beats silent". The expected string is
present and the variant passes, twenty milliseconds slower. Detection, not
discrimination.

Closed with a `forbid_for` table asserting the log must **not** contain
`console-ring: NOT held`, wired into the PASS arm rather than merely defined.
The round was also exact about what my sabotage proved: sensitivity to *"the
claim primitive does not dereference TPIDR_EL1"* — not to *"the ring lock is
actually held"* or *"the bound is honoured"*.

### Measuring the block instead of asserting it

I twice reported myself blocked on hardware with twelve files edited and none
compiled. The third time I checked: `ps` showed **37% of 800%** and nothing of
the lease-holder's on the cores — their concurrent work was a prosecutor round,
which is network-bound. The standing rule permits exactly this case (a check
while a peer holds the lease, when nothing of theirs is running, *checked with
ps and announced by note*). One kernel-only compile, seconds: **clean**, the
`void`→`bool` signature change harmless to its five callers, the sole warning
pre-existing in a file I never touched.

I was blocked on a *lease*, not on *cores*, and had not distinguished them. The
peer turned out to be genuinely mid-build a few minutes later, so the window
was real and narrow — which is why the rule says to measure at the moment
rather than reason from the lease. Boots still wait for the lease; I said "no
boots" in the note and that holds.

## 2026-08-18 — The round found the inverse defect: my fix for an over-permissive gate had landed as an over-restrictive one

The follow-up round the dirty C-6b close owed came back **0 P0 / 1 P1 / 1 P2 /
3 P3** — clean on both triggers. `MODEL(start) == MODEL(end)`, Opus fallback,
no mid-run drop. Worth saying which way the diversity caveat pointed, because
it **flipped**: the previous round audited Fable-authored code, so Opus was
genuinely cross-lineage; these fixes are Opus-authored, so this round was
same-family and its entire contribution was context independence. The spawn
said so and named the reflex to fight. The round named it back:

> I would have written that brace too, keyed on the same format, thinking
> about compressed textures and not about a driver that declares one byte on
> purpose.

### F1: the guard that refused what it had to admit

The P0 I closed last chunk was real — a 512×512 BO declaring 4096 bytes made
the compositor read 1 MiB out of a 4 KiB mapping. I fixed it in two places:
an exact bound at the **read** gate, and a "belt" brace at the **create**
door keyed on B8G8R8A8.

The brace refuses ordinary Mesa resources, and the proof was already in this
repo — in a comment, written by this project, at the exact line that chooses
the size (`usr/ports/mesa/patches/0006-*.patch:1511`):

> The seam refuses unaligned or zero backings; the driver's staging-path
> textures legitimately ask for size 1.

Mesa's virgl driver declares one byte on two paths that keep the real
width/height — the staging path (`alloc_size = 1`) and MSAA (*"don't create
guest backing store for MSAA"* → `total_size = 0`) — and our winsys rounds
that to one page. So `create3d … 512 512 … 4096` is **byte-for-byte both the
attack shape and a perfectly ordinary staged or multisampled BGRA texture**.
There is nothing to tell apart. Only the reader can distinguish them, by
whether it is about to read the backing — which is exactly what the read gate
does, and why it was the load-bearing half all along.

**The part worth carrying is why every gate stayed green.** The staging arm
hangs on a virglrenderer capset bit that *nothing in this tree measures*, and
thyla-pi's 1.1.0 evidently does not set it. The MSAA arm needed no host bit at
all: every multisampled BGRA render target above 32×32 was refused outright,
and no gate we have would notice, because a gate proves what the system *does*
and an over-refusal shows up only as something a client can no longer do.
**A guard whose activation no gate can see is worse than the hole it closes.**

And the prover leg I'd added to guard the P0 was asserting that a legitimate
allocation must fail. It is re-targeted as `C0-STAGING`: the door must *admit*
the one-page shape, with an unaligned backing as the control so "admitted"
cannot pass against a door that admits everything. The read gate's own runtime
regression test is **owed and tracked**, not quietly dropped.

**My parallel self-audit did not find this**, and the reason generalizes: I
prosecuted seven fixes and asked of each "is this gate sound?" — never "does
this gate refuse what it must admit?" Only the second question reaches a
client the tree does not contain. The round confirmed all seven of my
soundness findings and then found the one I had no question for.

### Rejecting the round's suggested fix (F4)

The DEEP arm's bar was stated three different ways and the code matched none.
The round proposed asserting the round's **max** via a census delta. I
re-derived it and **rejected it**: `Cost.max_ns` is a *global running maximum
that is never reset*, so a per-round max is not derivable — after round one a
delta detects only a new global record. But `mean ≥ T` does entail `max ≥ T`,
so the code was already a sound lower-bound witness and only the *prose*
overstated it. Fixed as prose, reconciled across three documents.

That it mattered showed up on silicon an hour later: round 3 measured a mean
of 128 ms over 2 retires, so the old "every compositor readback waited ≥ 100
ms" would have been false on that round.

### The deterministic failure that was my own fixture

`decomp gl` then failed twice, deterministically, at
`rp6 never confirmed the /env write (60s)`. I had just changed the compositor,
so it read as my regression.

It was my **pool**. `tools/test-fault.sh` re-bakes `pool.img` with `CLADE=0`
on every variant — and I had just run it ten times — so `/clade` was gone, and
`glq-decomp.exp` builds its `rp6` wrapper on-device with `/clade/bin/clang`.
The scenario's `echo rp6-ready` runs *whether or not clang succeeded*, so the
harness reported "rp6 built" and then failed 60 s later naming `/env`, a
subsystem with nothing to do with the cause. **A step that confirms the next
command instead of the one under test will always misattribute the failure.**

My own failure inside that: I verified the **ramfs** by content before syncing,
exactly as the discipline demands, and did not verify the **pool**. Verifying
one paired artifact by content and trusting the other is not verifying by
content. The build's output had said so plainly — `bake config CLADE=0`,
`payloads verified PRESENT: GOROOT GOCACHE GO4C QUAKE`, no CLADE — and I read
past it. A one-command check settles it: 917M with clade, 449M without.

Also recorded because it cost real context: **do not `grep` the pool image.**
It is an encrypted Stratum image; grepping it dumped megabytes of binary into
the transcript and told me nothing.

Re-baked both paired artifacts with `PRESERVE=0`, re-synced, and the same code
passed: **GLQ-DECOMP PASS gl**, 969 frames at 37.9 fps composed on real V3D.
Same code, different fixture — the attribution is settled, not assumed.
`test-fault.sh` mutating a shared fixture other gates depend on is filed
(main#250); it should restore the operator's bake config or refuse, the same
shape as `test-interactive.sh` refusing when a VM is already running.

### #243 and #246, from the extinction work

`uart_puts` takes no lock, so the ring claim serializes against ring traffic
only. The class was **observed live and fixed once already** — #76 removed the
same raw loop from `SYS_PUTS` after it shredded a login prompt byte-for-byte —
and `viv_report_unserved` reached for it again, on a path an unprivileged EL0
program triggers by choosing an unserved syscall. Now one `cons_diag_line`
unit; verified live in the boot log.

`el1_sync_runaway` had no test and `7dd5be19` had just put three calls on it.
Confirmed by reading why: the depth ladder tops at 3, the #806 guard extincts
at 2, so only a fault from *inside* the extinction path reaches it — #244's
shape, on purpose. **Discrimination proven** by sabotaging the claim back to
the counted trylock and watching the variant fail. Stated exactly: that
sabotage does *not* reproduce #244's silent park — the counted trylock trips
`lock-across-sleep` first — so what it proves is sensitivity to the claim
path's correctness, not reproduction of the original bug.

And `test-fault.sh` enumerated its variant set **four times**; adding one
updated two of them, so `test-fault.sh el1_sync_runaway` answered "Unknown
arg" while `make test-fault` ran it happily. The arg arm and `--help` now
derive from the one list.

## 2026-08-18 — Two gates nobody ran, and the count that refuted my first explanation

Spawned the follow-up prosecutor round the dirty C-6b close owed (`c8c83348` +
`2f3c0bcc` — a P0 returned and P1+P2 hit six, so CLAUDE.md's re-audit rule
fires). Fable is out of credits, so it went straight to the Opus fallback per
scripture. **Worth stating which way the diversity caveat points this time,
because it flipped**: the previous round audited Fable-authored code, so Opus
was genuinely cross-lineage; these fixes are Opus-authored, so this round is
*same*-family and its whole contribution is context independence. The spawn
says so explicitly and tells the prosecutor which reflex to fight — agreeing
with a construction because it is the one it would also have written.

While it ran, the audit-in-flight discipline: non-colliding work, then
prosecute the same surface myself.

### The non-colliding work turned out to be the more interesting half

main#245 said `test-fault.sh` is wired into no gate. A census over `Makefile` +
`tools/`, with a control at each end (`ci-smp-gate.sh` must resolve to a target,
`test-fault.sh` must not), found **two** orphans rather than one:
`tools/verify-kaslr.sh` has no caller either. The only references to either are
two *comments* in sibling scripts.

Neither is decorative. `test-fault.sh` is the only witness that the seven
hardening protections actually **fire** rather than merely being compiled in —
the canary, kernel-image W^X, BTI, the two stack guards, the boot-CPU idle
guard, the recursion arm. `verify-kaslr.sh` is I-16's only runtime witness:
ROADMAP §4.2 requires the kernel base to differ across boots, and `make test`
accepts any *single* boot, so it is structurally blind to a slide that never
moves. This is how #244 hid for a month.

**Then the interesting part: my first explanation was wrong, and its own
measurement said so.** The obvious hypothesis is that the survivors are in
CLAUDE.md and the orphans are not — CLAUDE.md is auto-loaded every session, so
that would be a clean anti-rot story. The count refutes it: `test-fault` and
`verify-kaslr` appear in CLAUDE.md **twice each**, exactly like `test-a72` and
`check-v80-floor`, which did not rot.

The difference is *where*. The survivors sit in the "Build + test commands"
block, as commands. The orphans appear only in the boot-banner paragraph's
prose, named as **consumers of the ABI literals** — things that would *break*
if you reworded one, never things to run. Every session learned they existed
and nothing about invoking them. Which is precisely the mention-versus-program
distinction that same paragraph teaches about its own co-update list, applied
to itself and not noticed.

So the remedy is both halves, in the idiom this project already uses for the
class (`check-production`/#228, `test-a72` and `check-floor`/#91): a named
target with a WHY comment, **plus** an entry in the command block. `55c5d2f8`.

**A second wrong turn, caught after the commit.** The census as first run also
grepped `.github` — which does not exist. There is no CI in this repo at all,
so that arm searched nothing and contributed no evidence, while the commit
message reports "no Makefile target, no gate, no CI step" in a list that reads
as three findings. The claim is true; one third of it is *vacuous*. An empty
arm of a census must not be reported as though it were a negative result, and
the tell is that the arm was never given a control the way the other two were.

**A wrong turn caught before it shipped.** The first draft of the help text put
backticks around `make test` inside a Makefile `@echo "..."`. Backticks inside
double quotes command-substitute — `make help` would have *run the full test
suite*. Caught by rendering the target rather than trusting the diff.

**What this does not close, stated rather than glossed:** neither script now
runs *automatically*. They are named targets a human or agent invokes, exactly
like `test-a72`. Whether test-fault joins the pre-push bar costs 7 builds + 7
boots, and the gating evaluation is the operator's call, so it is surfaced.

### The vault gains a fourth failure class

`quaestor owner` routed the change to `abi-boot-banner`, whose taxonomy
enumerates three ways a co-update list member fails — *phantom* (named, never
existed), *inert* (exists, matches nothing), *document* (matches, only goes
stale) — against an implied healthy fourth, the **program** that "breaks
silently and immediately".

Two of its fifteen derived mirrors were programs nothing ran. That class has a
program's full co-update obligation and **no failure behaviour at all**: it does
not break loudly, and unlike a document it never even becomes visibly wrong,
because nothing evaluates the mismatch. Strictly worse than the document class.

The mirror rule itself is unaffected — it answers "who must be co-updated", and
an unrun program must still be co-updated. What the note now guards against is
reading a fifteen-member derived set as *defence in depth*. **A mirror set
bounds the co-update obligation; it says nothing about detection latency, and
only the members something actually runs contribute to detection at all.** Same
shape as the extinction seam one level up: a contract on a value is silent about
its delivery; a contract on the set of readers is silent about whether any of
them reads. Vault `60095c97`, lint 946/0/0.

### Self-audit: seven fixes prosecuted, seven sound, one suspicion withdrawn

Re-derived from the code rather than from each fix's own comment. The P0 repair
is covered better than its comment claims: the pre-existing `b.w == s.w` check
sits before the new size guard on the same path, so the guard's geometry *is*
the reader's; and `comp_readback_retired` re-runs `gl_adoption` as
`same_adoption` at retire, so the guard re-validates at **read** time and the
issue→retire TOCTOU is closed by construction. The "sole `Some(va)` caller"
claim was re-derived, not accepted: exactly two call sites, one `Some`, one
`None`, and the Warp-4 synchronous arm that originated the P0 no longer exists.

`FenceTag.ok` has one construction site, fail-closed at `false`, and two
textually identical assignments. `FenceVindication.comp` takes its
discriminator and its ctx from the same loop index at both sites, so they cannot
disagree. The `COMP_FSLOT` exemption is conditional on scope and correct in
*both* directions — the client-driven scoped lever cannot touch the reserved
slot, the internal unscoped callers still can, because a wedge that is real is
genuinely global.

**One suspicion raised and withdrawn by measurement**: `rb_coalesced` looked
mis-charged (the `+= 1` sits outside the match, so both arms reach it) — the F9
class again. Two checks killed it: `git show 24e6753d` proves the unconditional
increment is pre-existing and untouched by my fix, and `149-warp.md` defines the
key as "presents that enqueued instead of issuing", which is exactly what
`rb_enqueue`'s two callers are. Recorded as withdrawn rather than dropped
silently, because a fabricated defect eats the budget a real one needs.

Findings in `memory/audit_c6b_followup_selfaudit.md`, to be **merged** with the
round's report when it lands, not segregated from it.

## 2026-08-18 — The owed C-6b round: a deviation is dangerous everywhere else that reads the same field

Fable ran out of credits mid-spawn — the prosecutor died after loading the
preamble and before producing findings, which is an **absent** round, not a
clean one. Per CLAUDE.md that goes straight to the fallback tier rather than
retrying Fable, so it ran on Opus 5.

**The family-diversity caveat is INVERTED here, and reciting it would have been
wrong.** The standing rule assumes an Opus prosecutor shares the author's
priors because Opus is this project's implementation agent. But `ef58d639` and
`24e6753d` were written by **Fable 5** earlier the same session — so an Opus
prosecutor is genuinely cross-lineage against *this* author. I said so in the
spawn, told it its contribution was context independence, and warned it that
the code's own justifications (dense comments, the AS-BUILT paragraphs, the
audit row's prosecute-on-change list, five closed lists of "VERIFIED SOUND"
arms) are the author's argument and not evidence. It came back with **1 P0 /
3 P1 / 3 P2 / 3 P3**, and three of the findings are corrections to claims the
tree makes about itself.

### The lesson, and it is specifically about AS-BUILT 1

C-6b deviated from the design's letter in one recorded place: the compositor
readback's fence tag carries the **client's** `ctx_pub` rather than 0. That was
argued carefully and it is right — 0 is `warp_ctx_vindicate`'s no-slot
sentinel, and the client's own vindication has to wait for our poisoned slot.

What was never enumerated is the deviation's **cost**. Every mechanism keyed on
a tag's ctx now reaches the compositor's reserved slot, and two of those are
*shipped, client-drivable levers* (`warp-hold` / `warp-abandon`, since
`default = ["test-mode"]` and nothing passes `--no-default-features`). Their
safety argument is #178's: "the worst a client can do is wedge its own ctx,
which it could already do." C-6b made that false one resource over, silently,
and the round found it (F4) by prosecuting the documented deviation **as a
design change rather than as a footnote**. Worse, `drain` cleared
`fslot_since` one line *before* the hold check, so a held slot could never
reach `reap_abandoned`'s staleness test — the pin was indefinite, not bounded
by 30 s. Compositor-wide: every other client's readback frozen, the 500 ms sync
deadline disabled process-wide, and a ~1 kHz spin in the console for the life
of the box.

**A deviation is sound for the reason it was taken and dangerous everywhere
else that reads the same field.**

### The P0 was pre-existing, and its guard was a comment about the wrong subject

F1: `wbo_create` validated the client-declared backing with two gates and
**both are upper bounds** — its comment states the one-directionality outright
("a 1x1 texture cannot ask for 64 MiB"). `gl_adoption` compared `w`/`h` for
*equality*, never capacity. And `compose_cpu` reads `sw * sh_full * 4` from the
BO's `va` with the dims taken from the **surface**. So a 512×512 BO declared
with 4096 bytes — page-aligned, under both caps, `Y_0_TOP` so it takes the
readback arm — was admitted, adopted, and composed by reading **1 MiB out of a
4 KiB mapping**: a bump-allocated neighbour (another client's pixels, painted
onto the attacker's own pane) or a fault in the process that *is* the console.

`compose_cpu` carries a `SAFETY` comment asserting the rows are in range
"because damage was validated against the surface geometry". True of the
**weave**, whose size derives from that geometry. False of a client-declared BO
backing. The same function reads both.

Pre-existing from the Warp-4 synchronous arm and in none of the five
preambles — attribution, not ownership. Fixed at the read gate (exact:
`b.size >= b.w * b.h * 4`, exact because adoption already pins the dims, and
`comp_readback_retired` is the only `Some(va)` caller — enumerated by enclosing
function, not by grep hit) and at the door (keyed on `B8G8R8A8_UNORM` alone: a
general per-texel floor would refuse legitimate *compressed* textures, and it
must not key on `composable` because the attack shape is precisely
non-composable — that is how it reaches the readback arm).

### Converging with my own pass, and the one I sharpened afterwards

I ran the self-audit in parallel per the audit-in-flight discipline and found
F3 independently (a vindicated compositor readback bumps the **client's**
`fence_signaled`, so `warp_fence_wait` — which returns on `signaled >= seq` —
returns one fence early for the ctx's life). Filing it before the round
reported is the useful part: two prosecutors reaching the same defect from
different directions is the strongest signal either one produces.

The round also sharpened something I had noticed and under-read: `rb_wanted`'s
growth. I saw it was unbounded in principle; the round pinned *why the comment
was wrong* — the dedup key included `gen`, drawn from a monotonic counter, so
"bounded by MAX_SURFACES" bounded `n` and not the pair.

### The fix that broke the gate, and what that is worth

My fix to F8 (DEEP asserted a **sum** over an unknown retire count against a
per-readback threshold) required *exactly one* retire per round. The gate went
**red on a healthy build**: `comp-rb landed 1->7` across three rounds — **two**
retires each, because the flight loop's later presents each request a readback
and the pump issues the next the moment the first lands.

Every round satisfied the substance (waits 794 / 1007 / 260 ms, each observing
draw 1199 of 1200 by its pixel witness) and failed my arithmetic. **I had
replaced a wrong statistic with a claim about the mechanism's scheduling**, and
the claim was false. The round had offered the right alternative in the same
breath and I took the wrong half of it. Now it asserts the round's **mean**:
robust to any retire count, still rejects the case the sum admitted (one long
readback plus one instant one averages below threshold), and the pixel witness
still carries which draw was observed. The per-round line prints the count and
the mean so the next red is diagnosable without a re-run.

Worth recording plainly: the gate caught my own fix, on real silicon, one
commit after I wrote it. That is the system working — and it is the second time
this run that a control earned its keep by going red for a reason that was not
a defect.

### What is NOT closed

F7 [P2] is a **measurement debt**, not a code change, and saying otherwise
would be the worse outcome. The readback gate cannot *discriminate* a sabotage
that removes the deadline widening: the certifying run measured `F2B max
267 ms` against a `SUBMIT_DEADLINE_MS` of 500, so a build without the widening
passes identically. Sharper still — the deadline is evaluated **only at a stale
wake**, and the stall it exists for (a synchronous host
`TRANSFER_FROM_HOST_3D` on QEMU's serial main loop) raises no interrupts. So
whether the widening is load-bearing *at all* depends on INTx sharing nobody
has measured. GPU-DESIGN 4.5.13 now says that instead of "correct by
construction", and names what closes it. Tracked as main#253.

The close is **dirty** (a P0 returned; P1+P2 = 6) and several fixes are
structurally invasive, so **a follow-up round is owed on the fixes themselves**.

---

## 2026-08-18 — The extinction line, source 2 of 3: the fix found a fault gate that had been printing nothing for a month

Same run, after C-6b landed and pushed at `f525cea3`. Next on the resume note
was the follow-up Fable round on the C-0d fixes + C-6b; it was spawned first
(read-only, no cores), and this chunk ran alongside it.

### What was owed

The `EXTINCTION:` ABI line has **three** tearing sources and the names are
close enough that I have conflated them before. Source 1 —
extinction-vs-extinction — was closed 2026-08-16 by `extinction_claim_console`
(one `__atomic_exchange_n`; losers park silent). Source 2 —
**extinction vs a peer's ordinary console write** — is the vault's
`seam-extinction-line-unserialized`, and it is the one that matters most by
readership: the seam's own census found **fourteen of fifteen** declared
mirrors match the crash prefix, against eight for the boot-success line that
got the guarantee. Source 3 is `IPI_HALT`, still a commented-out reservation.

### The prescribed remedy was a hypothesis, and it was wrong in one specific

The seam prescribed a **try**-acquire of the *writer role* (never a park).
Checking it against the drain path says no: the role (`g_cons_tx.writing`)
serializes whole `cons_output_write` calls, but **the drain never consults the
role** — that is main#144, already written down in `cons.h` — so bytes a peer
had already pushed would still pop into the FIFO from cpu0's TX IRQ or from a
peer's `cons_tx_kick`, landing inside the banner while the role sat held.

What actually owns the wire is **the ring lock**: every steady-state producer
pushes its unit under `g_cons_tx.lock` (`cons_tx_push_bulk` — SYS_PUTS through
the role, the echo, `cons_diag_line`) and every ring→FIFO drain pops under the
same lock. So the winner takes *that*, and never lets go
(`cons_tx_claim_for_dump`, `kernel/cons.c`). The role is also the wrong
primitive on a second axis: a healthy peer holds the ring lock for one bounded
push or one FIFO-depth drain — microseconds — where the role is held across a
whole write, room-waits included.

Every property is deliberately the **opposite** of the console word one file
over, and the reason is the same in each case — *who holds the thing you are
waiting for*:

| | console word (source 1) | ring lock (source 2) |
|---|---|---|
| holder you contend with | a **dying** peer that never releases | a **healthy** peer that will release in µs |
| therefore | **try once**, never spin | **bounded spin**, because try-once fails exactly when it matters |
| primitive | raw atomic (a spinlock could fault on a dying machine) | **raw** trylock, same reason — new `spin_trylock_raw` |
| on failure | park silent (a missing line is visible; a torn one reads as a clean boot) | emit anyway, and **report the miss** after the dump |

IRQs are masked before the acquire and never restored: with the ring lock held
on this CPU, its own TX IRQ arm (`cons_tx_drain_from_irq` → `spin_lock_irqsave`)
would self-deadlock — a silent hang in place of the dump. The caller parks in
`_torpor`, so nothing is owed back. And the flush under the lock became the
*full* bounded ring rather than one FIFO's worth, because holding forever means
whatever is still queued when the flush stops is lost, where the predecessor's
release let the rest trickle out behind the dump.

### The compile found the emitter the census had missed

`cons_tx_flush_for_dump` had a second caller: `arch/arm64/exception.c::
el1_sync_runaway`, the #214 recursion guard's terminal banner — which prints
`EXTINCTION: el1-sync recursion ...` **without going through `extinction()`**,
and was therefore enrolled in *neither* serializer. Not in the 2026-08-16
console-word fix, and not in the vault's `abi-boot-banner` mirror set either:
`quaestor owner` flags it as matching the ABI literal *outside* the set. It now
takes both, via a new `extinction_console_claim_or_own()` — claim the word, or
confirm this CPU already owns it, since the runaway is reachable from a chain
that claimed it at depth 1; a *peer* holding it means a peer is dumping, so it
parks silent like any loser, counted.

Worth noting how it surfaced: **not** by the census I ran, but by deleting the
old symbol and letting the build fail. A rename is a census that cannot lie.

It also reports a ring-claim miss after its own banner, which cost the SMP gate
a restart: I noticed the asymmetry (only `extinction()` reported) five boots
into the matrix. Killing it there and re-running cost ~10 minutes; letting it
finish and re-gating afterwards would have cost ninety, and shipping the green
from an ELF that no longer matched the source would have been a *misleading*
green, which is worse than a red.

**And that path is exercised by no test at all — this chunk just put three
calls on it (main#246).** In a healthy kernel the #806 guard extincts at the
*second* kernel fault, so `g_el1_sync_depth` never reaches 3; reaching the
runaway needs the extinction/Halls path itself to fault — which is precisely
the base-tree defect below, and precisely what this fix removed. The fix
deleted the only thing that was reaching the path it also modified. "No current
path drives it" is the latent-P1 trap, not a safety argument, so it is filed
rather than glossed.

### Then the base measurement, which is the actual finding

`tools/test-fault.sh` passed 7/7 on the change. To be sure the pass meant
something I stashed the work and ran the sharpest variant on the base tree:

| tree | `recursive_kernel_fault` |
|---|---|
| base `f525cea3` | **TIMEOUT (60 s)** — last guest line is `fault-test: invoking recursive_kernel_fault...` |
| this change (raw try-spin) | PASS — `EXTINCTION: recursive kernel fault (handler re-entered) 0xdead000000000000` |
| this change, counted `spin_trylock` restored | TIMEOUT, symptom byte-identical to base |

**The base tree printed nothing at all.** That variant installs
`TPIDR_EL1 = 0xdead000000000000` deliberately — a wild `current_thread()` is
its entire premise. `extinction()` flushes the ring *before* the banner (on
purpose: causal order), the old flush took the lock with the **counted**
`spin_trylock` → `spin_preempt_inc` → `current_thread()->magic` → **fault,
inside the extinction path**; the nested EL1-sync faults climbed to depth 3 →
`el1_sync_runaway` → which called the *same* flush → faulted again → depth 4 →
the `depth > MAX` arm parks **silently**.

So the one fault variant whose whole point is a destroyed `current_thread()`
could not print its own banner — and failed by **silence**, not by a wrong
message, which is the shape that reads as "the harness is slow" rather than
"the protection did not fire". Broken since `ed56f21f` (#75 P1-F, 2026-07-20)
met `ce7bd352` (#360's counted spinlocks, 2026-07-04): about a month, because
**`test-fault.sh` is wired into no gate** — grep-proven over the Makefile,
`ci-smp-gate.sh`, `test.sh`, `test-interactive.sh` and `.github`. It is the
only runtime witness that W^X, BTI, the stack guards and the #806 guard
actually fire, and it runs when someone remembers. Filed main#244 (the defect,
closed here) and main#245 (the ungated harness, open).

**The rule that generalizes, and the reason `spin_trylock_raw` exists:** a
dying-machine path may not call a primitive that reads state the crash may have
destroyed. #360 retrofitted that `current_thread()` deref under *every* existing
`spin_trylock` caller — including one on the extinction path — without anyone
re-asking whether that caller could survive it. The `spin_lock_raw` comment now
enumerates its two legitimate holders instead of naming one and calling every
other use a bug.

### A defect I nearly fabricated, and what stopped it

The sabotage run's failure lines came out as
`[test] cons.ring_claim_core_returns_holding ...   [runnable-dump returns HOLDING: a second taker must fail while the claim is held]`
and I read that as a live tear of exactly the residual class I had just filed
(main#243: direct-`uart_puts` diagnostics outside the ring lock). It is not.
`test_fail(msg)` calls `sched_dump_runnable(msg)`, which prints
`"  [runnable-dump " + tag + "]"` — the tag **is** the failure message. Intended
output, read as an interleave because I was primed for one. Withdrawn within
the minute, by reading the caller instead of the line. *A fabricated defect
outranks a missed one*: it would have eaten the budget a real one needs, and it
would have "confirmed" a bug I had filed an hour earlier — the worst direction
for a confirmation to arrive from.

### Posture

Suite **1427/1427** (was 1424 — three new legs), `test-fault.sh` **7/7**, both
sabotage arms verified in one run (1427 → 1424/1427, each failure naming its
own assertion; source restored byte-identical to the verified WIP and re-run
green). The kernel changed, so the SMP gate is owed and running.

**Still open, exactly:** source 3 (`IPI_HALT`) — untouched. And the ring lock
reaches only writers that go *through* the ring: steady-state kernel
diagnostics that still call `uart_puts` directly (`sched.c`'s runnable-dump,
`syscall.c`'s vivarium unserved / `viv-trace`, `exec.c`'s exec-failure,
`9p_client.c`'s ownerless-frame) sit outside it and can still land inside the
banner from a peer CPU. `cons.h`'s contract already says those callers should
use `cons_diag_line`; converting them is main#243, and they carry the #126
20-ms-per-byte exposure too. **This closes one of three sources, and the third
would subsume the residual of the second.**

---

## 2026-08-18 — C-6b: the readback arm off the console's dispatch, and the load that measured which GL context a queue is on

Resumed from the self-compaction at `64ded01d` (the C-0d Fable close + the
C-6a spec pushed). The mac was aux's for the first hours (its SMP gate, then
its round-B P1 fix), so this run did its reading, code and docs cold and
queued on the lease for every build — three times, because the gate's
positive control kept saying "the queue you built is not the queue you
think", which is the finding worth writing down.

### The implementation (`server.rs` / `gpu.rs`) — one refinement the design's letter did not have

GPU-DESIGN 4.5.13 said the compositor-owned tag would carry `ctx_pub = 0`.
Reading the driver's abandonment bookkeeping said no: `fslot_poison_ctx`,
`FenceVindication.ctx_pub` and `ctx_has_poisoned_slot` all key on the tag's
ctx, and 0 is `warp_ctx_vindicate`'s "no condemned slot" sentinel — an
abandoned compositor readback under ctx 0 that the device later retired
would push a vindication for ctx 0, `position(p == 0)` would match an
arbitrary un-condemned slot, and `ctx_destroy(slot+1)` would hit a live host
context. And the client's own vindication has to WAIT for our abandoned
readback of its BO (round-4 F1: one late retire proves nothing about the
rest), which only holds if the slot is attributed to the client. So the tag
carries the CLIENT's `ctx_pub` plus explicit `readback` / `comp` bits; the
pump routes on the bit and poisons / decrements the right ctx. Recorded as
AS-BUILT 1 in 4.5.13. Everything else is the design as written: the
reserved slot (`COMP_FSLOT` = 15; the client pool is 0..15 and
`lane_exhausted` / `fenced-free` read only that), `Comp.comp_rb` +
the gen-pinned `rb_wanted` FIFO (one in flight compositor-wide — the slot IS
the bound), `comp_readback_retired` BEFORE `warp_pump_retires` in the pass
(the pump's decrement can quiesce a retiring BO; the compose must read `va`
first, and `gl_adoption` refuses a retiring BO/ctx so a destroy in flight
drops the frame), `fences_in_flight` + `comp_rb_in_flight` symmetric on
issue and retire, the admission subtraction, the sticky 30 s deadline while
any readback is in flight, `Cost::ReadbackWait`, the `comp-rb` census (keys
prefixed — `abandoned` was already the test-mode key and `parse_field` takes
the first hit).

### The gate, and the two loads that were not the load

`warp-prove readback` (its own verb, like `reject`: it stalls the device on
purpose) with named arms — ARM (a present on an idle queue issues and lands
a compositor readback), DEEP (the readback the device paid waited ≥ 100 ms:
the positive control that the queue existed), LIVE (while it is in flight,
the adopting surface's own presents and warp ctl reads answer inside 50 ms —
under the old arm the first present takes the whole wait), DEADLINE (a
client's OWN fenced readback of its busy BO, then ten bystander presents
behind it: all succeed, engine alive — busy read as busy), F2B (the
bystander's latency, reported), CLEAN. `C6-READBACK DONE` is a verdict (the
F6 shape); `warp-readback.exp` hard-fails on `INCOMPLETE(<arm>)`.

**Run 1** (800 1:1 NEAREST full-frame blits, ping-pong BO ↔ scratch): ARM
PASS, LIVE PASS, DEADLINE PASS — and DEEP FAIL: `readback-wait max 16 ms`.
1.6 GB of copies do not finish in 16 ms on a Pi. `vrend_renderer_blit`
(1.1.0) takes the `glCopyImageSubData` shortcut for a 1:1 same-format RGBA
NEAREST blit; whatever those became, they were not GPU work the readback
waited on. Without the control LIVE would have passed on a light queue —
which is exactly why the control is there.

**Run 2** (SCALED blits, 512² ↔ 1024²): the 8 submits retired in **1335 ms**
— real work — and DEEP still FAILED: the compositor readback of the same BO
waited **84 ms**, and the client's own readback stalled the bystander by at
most 149 ms. LIVE FAILED too (94 ms), which turned out to be the same
mechanism seen from the other side. A scaled blit goes through
`vrend_renderer_blit_int` → the BLITTER, and vrend's blitter owns its **own
GL context** (`vrend_blitter.c`); a client-context fence and a
client-context `glReadPixels` are not ordered behind another context's
work. The queue was deep; the readback was not behind it. **A claim about a
lane must be re-derived per COMMAND CLASS** was C-0d's lesson; this is its
sibling: **a queue is deep only on the GL context the wait is on.** A real
client's draws land on its own context, so the honest load is client-context
work: **run 3** queues clear PAIRS (the BO to an index-encoded colour, then a
2× scratch, alternating framebuffers so mesa v3d cannot fold them — each a
full-surface store), and the leg now prints the queue's fence timeline and
**which clear index the compositor readback observed** (the BLUE byte of the
pixel it landed): "the readback waited for the queue" is a pixel, not a
duration.

**Run 3** (alternating full-surface clears, BO ↔ a 2× scratch, index-encoded
colour): the readback observed clear **639 of 640** — it DID wait for the
whole queue, the mechanism is right — and the whole queue took 122 ms: mesa
v3d keys jobs by framebuffer (`v3d_get_job`), an FBO switch does not flush,
and 1280 clears folded into two jobs. **Run 4** (draws — hand-encoded from
the Mesa tree's `virgl_encode.c` field for field, a `verify` after the prime
so a rejected stream names itself): DEEP PASS at last (readback-wait 130 ms,
draw 2399 of 2400 observed) — and LIVE FAIL on the SECOND present (140 ms
inside a 168 ms flight; the issuing present 0 ms). **Run 5** made LIVE the
issuing present over three rounds and reported the rest: LIVE 0/0/0 ms;
DEEP failed one round at 88 ms because the eight 24 KiB Twrites
themselves took 130–290 ms and the ~415 ms queue was nearly drained at
issue. **Run 6** deepened the queue (3 triangles per draw) and added the
census of OTHER console work per round: `slot-presents +1` in EVERY round —
the console renderer's cursor-blink present — and the sends took 478 / 794 /
1062 ms. That named the deterministic blocker: on egl-headless a present's
`RESOURCE_FLUSH` is the display backend's `glReadPixels` of the screen (the
C-4 lane cost), queued behind the compositor's blit, behind the client's
draws on V3D's one hardware FIFO; the single-threaded loop waits there for
everyone, and my own sends waited behind it too, so a readback issued after
them met a drained queue. **Run 7** halved the send exposure (4 submits × 6
triangles) and made a round self-validating — issued into a queue with less
than the floor left = UNCONSTRUCTED, retried, never judged — and the gate
went green: `WARP-C C-6 GATE: VERIFIED`, issuing present 0/0/0 ms,
readback-wait 497/1001/1027 ms, draw 1199/1200 observed every round, two
unconstructed rounds retried; DEADLINE 10/10 alive; F2B max 1034 ms mean
119 ms. The final artifact re-ran green (805/1005/1005 ms, F2B max 267 ms).

**Sabotage S1** — the issuing present made to WAIT for the readback (the
pre-C-6 arm): first run read as `deep-unconstructed`, because the prover
stamped the issue time AFTER the present returned; stamped before it, the
sabotage fails LIVE with the issuing present at 269 / 969 / 1017 ms — the
arm discriminates the defect and nothing else. Not run: a sabotage of the
deadline widening — no stale wakes were observed during ~1 s stalls on this
lane, so the old 500 ms deadline may never have fired here; the widening is
correct by construction and the DEADLINE arm is its net where wakes arrive.

What the run says about C-6 under QEMU/virgl, honestly (AS-BUILT 3 in
4.5.13): the console never waits inside the present that issues the
readback, and one readback is in flight at a time — but any sync step the
console issues while a client's queue is deep inherits the stall, and on
egl-headless every present is such a step. C-6 removes the per-present
multiplication and the false dead-latch; the stall itself is the host's
(F2b) until Venus / v3d.

### The bar

Local: suite 1424/1424 + arc gates 2/2 + clade 3/3 + G-4 CONSOLE VERIFY OK
(kernel byte-unchanged; SMP 40/40 @401d4b27 carries). thyla-pi (KVM, V3D,
virglrenderer 1.1.0): `readback` VERIFIED on the final artifact; `reject`
C-0d DETECTOR VERIFIED; `prove` WARP-2 VERIFIED; `quake` WARP-4 VERIFIED
(969 frames 44.2 fps; `comp-rb issued 0`); `decomp gl` PASS (composed gpu
1106 cpu 0; `readback 0`, `readback-wait 0` — the blit arm untouched). LS-CI
gfx subset (ls-ci + 15 ls-gfx-*) 16/16, 0 retries, run alongside the Pi's
final gate (the mac idle otherwise). Every ramfs verified by content before
each sync (`cpio` extract + `strings`), and the `cd usr` trap paid three
more times before I split the build from the bake.

## 2026-08-18 — the C-0d Fable close: C-4's lesson had been applied to one pair and not the other, and the readback arm's remedy is not what it looked like

Resumed from the self-compaction at `401d4b27` (the merge pushed; the C-0d
Fable verdict in hand: 0 P0 / 2 P1 / 1 P2 / 2 P3, nothing fixed). The mac was
aux's for the first ~1.5 h of the run (its viv-run LS-CI legs), so this run
did all its reading, editing and design with no cores and queued on the lease
for the build — which is what the leases are for.

### The close (F1 / F5 / F6 fixed, F3 recorded) — `ef58d639`

**F1 was C-4's own residue.** §4.5.12 had measured that a texture transfer
or readback on a tiled renderer is a blit job behind everything the *device*
has queued, and moved the compositor's health pair to buffers — and left the
per-ctx #240 probe (`warp_probe_build`) a texture pair, because the
compositor's helpers (`health_upload` / `health_readback` /
`comp_copy_region`) had `COMPOSITOR_CTX` hardcoded and the client verify kept
its own texture-only transfers. So every client `verify` was still the drain
C-4 had just priced, and — the part the round added — one client's verify
paid for *another* client's queue, which the verify admission gate (F7's
`fences-in-flight`/`poisoned`, reading only the caller's gauges) cannot see.
The fix is structural rather than local: `CtxProbe.buffer`, the buffer mint
first for every ctx (`warp_hprobe_build`), the texture pair only where that
mint fails and counted (`probe-texture` on the global ctl — a say line at
ctx-create rate would be a storm), and ONE helper set for both pairs
(`probe_upload` / `probe_readback` / `probe_copy_region`) so the compositor
and the clients cannot drift again. The prover's C0-F1 leg had to change with
it: it attacked from a TEXTURE BO, and a texture->buffer
`RESOURCE_COPY_REGION` is not a legal copy — the renderer would have dropped
it and the leg would have printed DEFENDED for the wrong reason (a control
the operation erases). The attack source is a buffer of the probe's own
shape now (`mint_buffer_bo`, `rcr_stream` with a width).

**F5** (`present-to N bo`/`off`/`N bo` re-running the whole import witness on
the SHARED compositor context at 9P-write rate): the `verify_tick` shape,
one witness per ctx per compositor tick — but DEFERRED, never dropped: a
same-tick second consent sets `import_pending` and `frame_tick` replays the
import of whatever `present_to` names by then. The winsys re-consents only
when its front buffer changes, so the only legitimate second write in one
frame is a resize storm, and coalescing those onto ticks costs it one tick of
the readback arm.

**F6** (warp-prove printed `C0-REJECT DONE` unconditionally, so a blind
detector passed the scenario and only the host-side 5-term grep gated it):
DONE is a verdict now — every C0 arm records pass/fail and the token prints
iff all three passed, else `C0-REJECT INCOMPLETE(<arm>)`, which
`warp-reject.exp` hard-fails on through a new `lc_run_expect_hardfail_re`
(a regexp fail arm, so the prover's own `FAIL --` shares it). The 5 terms
stay as the belt: a scenario that passed for a reason the list does not know
about should still fail there.

**F3** recorded on #171 with a comment at `warp_probe_res_kind`: the probe's
two page mappings ride the never-rewound `weave_va_next` bump — a ctx-churn
driver on the same monotonic-VA class. Also noticed while writing it: the
detach names `size` while the bump rounds it up to pages — equal today (both
PAGE), and written down so a differently-sized probe cannot silently leak.

**Also found: the #240 detector's four rounds were never in
`AUDIT-TRIGGERS.md`.** r1–r3 lived in phase7-status rows and memory files
only. The tapestryd row now carries the addendum (all four rounds, this
close's fixes, five prosecute-on-change items).

### F2, and the design that came out of reading QEMU before writing it

F2 [P1] is the composed-GL present's readback fallback: `transfer_from_3d_
sync(g.dev_ctx, ...)` of the whole frame on the compositor's SYNC slot, so
the console's dispatch waits for the frame — for everything the client has
queued ahead of it, a length the client picks — and `fence_poisoned` cannot
guard it (the poison comes from `reap_abandoned` on the loop that is
blocked). The pickup note prescribed "the fenced / bounded readback". Reading
QEMU's `virtio-gpu-virgl.c` + vrend before designing it (the §4.5.4c habit)
changed what "fenced" buys: **vrend executes `TRANSFER_FROM_HOST_3D`
synchronously at DECODE time on QEMU's serial main loop** — `glReadPixels`
into the guest iov, returning only when every job writing the resource has
completed, which on V3D's in-order queue is every job queued before it — and
`FLAG_FENCE` changes only when the *response* is written. So a readback of a
busy resource stalls the DEVICE (every other client's commands, the
compositor's own sync steps, QEMU's display refresh) for the resource's GPU
backlog; fencing it frees the *guest* thread and nothing else; and a sync
step queued behind it inherits the stall — which makes `submit_and_wait`'s
"pending fences ahead cannot delay this chain" comment (true for fenced
SUBMITs, a decode) false for fenced readbacks (a GL wait), and its 500 ms
`SUBMIT_DEADLINE_MS` a false-`dead` hazard on a merely busy device.

That reframed the goal from "make the readback free" (impossible under
QEMU/virgl by construction) to three narrower things: the console's dispatch
never blocks on a client-chosen duration; the compositor never latches
`dead` because a device was busy; the compositor's OWN contribution to
device stalls is bounded and coalesced. GPU-DESIGN 4.5.13 (C-6, RESERVED) is
that design: the fenced readback with DEFERRED present completion, one in
flight per surface / latest wins, a reserved fenced slot (compositor-wide
bound of one, which loses nothing against a device that executes them
serially anyway), counted in the owning ctx's `fences_in_flight` for retire
safety but subtracted from admission so the client's share and its #210
ledger are untouched, and the sync-slot deadline widened to
`FENCE_ABANDON_MS` while any readback — ours or a client's — is in flight.
Two forms rejected on the record: a bounded sync wait (the command is already
in the device's queue; the next sync step waits behind it — bounds the wrong
thing) and gating on quiescence (a single-buffered client at its throttle
depth never quiesces; the §4.5.9 safety net would compose it once and never
again). The spec extension is named (`ComposeReadbackIssue`/`Complete`
behind `ALLOW_COMPOSE`, the retire guard generalized from `DrainedOfBlits`,
a `buggy_readback_free` cfg) and the Pi gate legs with it.

**And a new finding fell out — F2b.** Consequence 3 of the reading: *any*
client already holds the device-stall lever through its own `transfer_from`
of its own busy BO (the fenced verb every winsys has), repeatedly. F2 was the
compositor doing to itself what a client can do to it. Filed
(`memory/bug_f2b_readback_stalls_the_device.md`; GPU-DESIGN 4.5.13's F2b
paragraph): guest-side it can be not-added-to (C-6), not-mistaken-for-death
(the deadline half), and MEASURED (a warp-prove leg — client A reads back its
busy BO while surface B presents — owed with C-6's gate); it is removed for
real only by Venus (transfers become VkCommandBuffer copies the client
fences) or v3d-native (the queue is ours). Recorded under §9.2's host-side
exposures precisely so "trusted host" never reads as "no client can reach
it".

### Two things the bar found before it passed

**The C0-F1 leg's DEFENDED was a negative assertion with no positive
control** — "verify-ok still advanced after the attack" is satisfied by an
attack that never landed (the aux#215 class), and the texture-era leg had
leaned on a one-time host-log measurement for that; the buffer form did not
inherit it. Added in-guest before the first Pi run was trusted: after the
attack the client copies the mark BACK into its own buffer (the same command
the other way), reads its buffer back through the fenced verb, and requires
its own green. It printed `C0-F1 ATTACK LANDED -- the mark read back through
our own buffer as 0xff00ff00` — so the leg now proves a client can WRITE and
READ the probe's resources (the finding, re-measured on the buffer pair)
before it claims the repaint held; an unlanded attack is INSTRUMENT and F1
counts as not-defended.

**`warp-host.sh sync`'s uncommitted-scripts list omitted
`tools/interactive/lib.exp`** — the library every warp `.exp` sources. The
first sync shipped the new `warp-reject.exp` (in the list) against HEAD's
`lib.exp` (not in it), so the scenario would have died on `invalid command
name lc_run_expect_hardfail_re` — a list that claims to carry your edits and
does not carry the one file they all depend on. Caught by checking the Pi's
copy for the new proc before running (`grep -c` on both files, 1 vs 0);
`lib.exp` is in the list now.

### C-6a — the spec first (`tapestry_present.tla`, same run, after the push)

With the close pushed and ~100k of context left before the checkpoint line,
the next chunk was opened at its spec-first step rather than its code, so
that a compaction lands on a boundary and C-6's code has a model to be
audited against. `ComposeReadbackIssue`/`ComposeReadbackComplete` (a fenced
host DMA-WRITE into the client BO's pages, one in flight per generation),
`NoTornReadback`, `DrainedOfReadbacks` on `ServerRelease` + `Free`, and
`BUGGY_READBACK_FREE` as an omitted conjunct — the C-1 house style, for the
C-1 reason (a twin action drifts in more ways than the one under test). Two
deliberate absences, argued in the header: no `FillLanded` guard on Issue
(the device serializes the read against the fill — the very side effect P2
credits the sync readback with, now read in vrend 1.1.0 rather than
assumed) and no `attached` (the readback runs under the CLIENT's ctx; it is
the arm for the un-imported BO). `check-tapestry.sh`: ALL 12 CFGS AS
CLAIMED — the six direct-path cfgs at **5413** states exactly (the
additivity control, held twice now), the composed clean cfgs at 94680 with
liveness, and `buggy_readback_free` violating `NoTornReadback` in 11 states
(… `ClunkMap` → `ComposeReadbackIssue` → `Destroy` → `ServerRelease` →
`Free`: the pages freed with the device still writing them). SPEC-TO-CODE
names the sites the impl binds at; ARCH §28 I-40 / CLAUDE.md say 8 buggy
cfgs now.

### The bar

Local (mac): `cargo build -p tapestryd -p warp-prove --release`; ramfs
rebaked with `THYLACINE_BAKE_CLADE=1 THYLACINE_MKFS_PRESERVE=1`, verified by
CONTENT (`C0-REJECT INCOMPLETE` ×3, `probe-texture` ×1, `ATTACK LANDED` ×1
in `build/ramfs.cpio`); `tools/test.sh`: 1424/1424, arc gates L-6c/D-5 PASS,
clade 3/3, the G-4 console gate `CONSOLE VERIFY OK`. The kernel is
byte-unchanged (userspace + tools + docs only), so the SMP gate 40/40 at
`401d4b27` carries. thyla-pi (KVM, V3D, virglrenderer 1.1.0): `reject` →
`C-0d DETECTOR GATE: VERIFIED` (ANSWER=REPORTED-AS-SUCCESS as measured
before; DETECT PASS; STICKY PASS; C0-F1 first res 83 → mark 81 (the buffer
pair minted exactly two ids), ATTACK LANDED, DEFENDED; DONE; LS-CI PASS);
`prove` → `WARP-2 GATE: VERIFIED`; `quake` → `WARP-4 GATE: VERIFIED` (969
frames 21.7 s 44.7 fps on the egl-headless lane — 44.4/44.8 before;
`comp-attach witnessed 5 refused 0`; `comp-health verify on buffer pair`;
`probe-texture 0`). Both leases released the moment the resource freed;
the mac was aux's for the first ~1.5 h and its LS-CI legs were never
contended.

## 2026-08-17 — the aux-2 merge: two tracks fixed one UAF, and 23 conflicts said which one to keep

Resumed from the self-compaction at `a9a4a4fe` (Warp-C closed). The note said
"merge aux-2 first", and the reason it was first is the interesting part: the
main#243 Fable round had found a P1 (exec leaves `in_handler` set) plus two P2s,
and every one of them was ALREADY FIXED on aux-2 — aux had found the same UAF
(`#254`) the same week, from the other direction. Two independent proofs of the
same defect are worth more than one; two independent FIXES of it are a merge
conflict, and the conflict is where the decision lives.

### The merge itself (`8a58112d`)

104 aux commits over the common base `72ab319d`; 216 main commits the other
way; 23 conflicted files. The rule for every conflict was "which side's version
is the RATIFIED one", not "which is mine":

- **The sigtab UAF, twice.** main `a41fc9eb` reset the table in place through a
  public `proc_exec_reset_dispositions`; aux `c2a09473` + `8690cfb3` + `d3a11c8e`
  did the same through a static `proc_exec_drop_image_state` that ALSO clears
  the in-handler latch (#247 = main F1) and applies the operator-voted
  phenotype rule (F4). Aux's is the superset and is kept as THE one place; main's
  function is gone. What main had that aux did not was the per-8-byte-FIELD
  paragraph and an every-byte-zero test — folded into aux's comment, and the test
  ported onto aux's `_for_test` hook rather than deleted, because it asserts a
  property aux's test does not (a reset that stops early passes aux's).
- **`cons.c`'s mode write.** main's side was a COMMENT change (#233: login must
  set the mode before the prompt); aux's was a semantics change ratified in
  PTY-DESIGN and audited (a write clearing ICANON DELIVERS the pending line).
  Aux's code, plus main's corollary — the disclosure half of #233's race exists
  under either semantics, so the sentence still binds.
- **The bin lists** (`tools/build.sh`, `usr/Cargo.toml`): the union, verified
  programmatically against the base — no member dropped by either side.
- **AUDIT-TRIGGERS.md** was an add/add (both trees created it from CLAUDE.md's
  table on the same day and each appended rows): resolved ROW BY ROW against the
  base row, so main's vault-#170 path fixes and pipe escapes and aux's addenda
  both survive; the LS-8 row carries both sides' addenda in order.
- **147-execve.md's sigtab row** was stale on BOTH sides (main said "zeroed in
  place", aux said "zeroing is exact POSIX because SIG_DFL == 0" — aux's own later
  commit had made the reset phenotype-conditional). Rewritten to the MERGED rule
  rather than picking a stale side; the note-mask and in-handler rows added.
- **Seven ragged doc rows** (six pre-existing on both tips, one in aux's newest
  addendum) escaped with the two controls `85c1ee9c` used: the checker to zero,
  and de-escaped-line == original with only the named lines differing.

**One thing the resume note did not say and the build did:** aux's DISTRO gates
are pool-resident and SOFT-SKIP without the Alpine tarball, which main's cache
did not have. A green `tools/test.sh` with two skipped arc gates is a gate not
run — so the fixtures were copied from aux's cache and the pool + ramfs re-baked
PAIRED (`PRESERVE=0`, fresh key both sides). `arc gates: 2/2 ran -- L-6c=PASS
D-5=PASS` on the merged tree; suite 1424/1424; clade 3/3.

### The main#243 residuals, on the merged tree (F2/F5/F6/F7/F8)

The round's F6 was the sharpest: the 8-byte store width that the whole lock-free
argument rests on was a MEASURED codegen property (a struct assignment happened
to give `stp`), not a construction. It is a construction now — every entry field
is one `__atomic_*` op on an aligned u64 (`_Static_assert`ed), the install
publishes `handler` last with release and readers acquire it, the reset zeroes
`handler` first; objdump shows `str xzr` per field and `stlr`/`ldar` on the
gate. F2 wrote the load-bearing sentence AT `notes_proc_has_live_handler`
("a cross-Proc reader that acts on `handler` alone; the copy is discarded"),
which is the sentence the three earlier statements of the argument had each
left implicit. F5's discrimination was checked the only way that counts: two
sabotages (a reset one entry short; the gate field only) each went RED on the
named assertions, and the tree was reverted with text replacement, not
`git checkout`. F8 clears `clear_child_tid` at exec beside `in_handler`. F7
retired four stale sentences (three of them "X is not a table row" claims that
the LINEAGE arc had falsified without anything failing).

### The C-0d Fable round came back while the bar ran: two P1s the three Opus rounds could not see

The #240 detector's first read from a different lineage (98 of 101 model
turns Fable; the last three, the write-up, fell back to Opus 4.8 — recorded):
**0 P0 / 2 P1 / 1 P2 / 2 P3, dirty on the P1 criterion.** Both P1s are the
same blind spot from two sides, and it is exactly the one family independence
exists to buy: three Opus rounds gated the synchronous lane on the CALLER's
fence gauges, and none re-asked the cross-context question after C-4 measured
that a texture readback on a tiled renderer drains the whole device queue.

- **F1**: the CLIENT-ctx probe is still the TEXTURE pair. C-4 moved the
  compositor's health pair to buffers for precisely this cost and left the
  client detector as it was — so a `verify` on client A drains behind client
  B's queue while the gate reads only A's gauges, and 149-warp.md promises
  clients the opposite. Fix: the buffer pair for clients too (the C0-F1 leg's
  attack source has to become a buffer BO, or it "defends" for the wrong
  reason — a texture-to-buffer copy is refused, not repainted away).
- **F2**: the composed READBACK arm — the CPU fallback — is a synchronous
  full-frame readback of the client's render target on the client's own
  queue; the client picks its length; and `fence_poisoned`, round 3's gate,
  cannot protect it because the poison is produced by the reaper on the very
  serve loop that is blocked. Only READBACKS carry this (a blit's SUBMIT_3D
  response is written at decode time, before the GPU runs it), so the fix is
  not a gauge but the fenced form C-4 measured its way past — a bounded or
  deferred readback: **Warp-C C-6**, the next chunk. Gating the fallback on
  `fences_in_flight == 0` was weighed and rejected: it would collapse the
  safety net GPU-DESIGN 4.5.9 keeps for every continuously-rendering client.
- F3 (probe VA rides the never-reclaimed `weave_va_next`, a second driver
  for #171), F5 (`present-to` re-import witness storm on the shared ctx, no
  rate limit), F6 (the reject scenario's pass token is printed unconditionally;
  the real 5-term gate lives only in `warp-host.sh`). Dispositions in
  `memory/audit_c0d_fable_closed_list.md`; the close is the next chunk after
  the push, then the dirty-close follow-up round.

### The bar found one more thing, and it was ours from the merge

The merged tree's first LS-CI (JOBS=3) came back 37/37 — with **three attempt-1
failures at t=0-1 s**, every one `-qmp unix:build/qmp-gate.sock ... Failed to
bind socket: File exists`, every one classified INFRA by aux's failure-time
probe ("the VM never started, so this attempt says NOTHING about the guest").
aux's #230 had given run-vm.sh a SECOND QMP monitor for test.sh's screendump
gate — a fixed path — and test-interactive.sh's per-slot export list, written
for #127's lesson that "a fixed host resource is a DETERMINISTIC collision at
N>1, not a flake", predates it. Three VMs launched in one batch interleave
run-vm.sh's `rm -f` and bind, and the loser dies before boot. A retry budget
turned a deterministic collision into three green retries; the count is what
gave it away. `e680fdd5` exports `THYLACINE_QMP_SOCK2` per slot; the re-run
was **37/37, 0 retries, wall 1744 s** against 2569 s before — and the SMP gate
on the merged kernel: **40/40, 0 corruption / 0 external-kill** across
default+UBSan x smp4/smp8. Pushed to both mirrors at `e680fdd5`.

---

> **Two tracks, one thread.** Entries marked `(aux)` were written on `aux-2`
> and merged into this file when aux-2 merged into main (2026-08-17); the two
> tracks ran concurrently, so a main run entry and the aux entries beside it
> overlap in wall-clock time. The `(aux)` block below is in the order aux
> wrote it -- oldest first, `c8ab2744` to `01f076f2`; main's run entries
> below it are newest-first as the convention says.

---

## 2026-08-17 (aux) — the c8ab2744 audit close, and the positive control that caught a second bug

Resumed from aux's **first** self-compaction (the change-of-watch scripts had
been main-only until `4525023a`; the operator had compacted this track by
hand). The nudge fired and the resume note said, correctly, "execute the plan;
do not re-derive it" — the Fable 5 round on `c8ab2744` had reported the audited
change CLEAN and four PRE-EXISTING findings three lines above it, and the fix
plan was already written in `memory/audit_15_closed_list.md`.

### The four fixes (`93a91c6c`)

- **F1 [P1] — both class scans read the sigtab per note.** The terminate scan
  gated on `handler_va` (0 for every Linux guest) and returned the first
  latch-class name at ANY index, so a `SIG_DFL` candidate that fell through
  from the phenotype branch let it name a CAUGHT `tty:hup`/`interrupt` behind
  it and the guest died with its handler installed. #251's per-Proc predicate
  had reached three sites and not this one — the fourth "site N+1" on the row
  (V-8 F2 → #251 → maskstop → F1). Fix: `notes_proc_default_applies(p, name)`
  INSIDE both scans; the fixed-name outer gate on the stop scan retired.
- **F2 [P2] — a `SIG_DFL` `pipe` on PHENO_LINUX reached no arm** (no native
  latch, #237) and sat as the dispatcher candidate for life. Fix,
  phenotype-scoped: `viv_signote_default_is_terminate` + `exits(canonical)`
  from the phenotype branch on the candidate. Native `pipe` untouched; #237
  stays the ABI question it is.
- **F3/F4 [P3]** — the dead drain call deleted with its reasoning; three "an
  uncaught susp is never queued" sentences reworded (caught / all-masked /
  thread-less).

### The wrong turn worth recording: J and L passed on an empty capture

The E2E for F2 is three L-6c legs sharing one fixture — `err=$( { WRITER 2>&3
| head -n 1 ...; } 3>&1 )` — J and L asserting the writer printed NOTHING (killed
by SIGPIPE), K the positive control (`trap "" PIPE` in the writer's own process
→ EPIPE returned → `write error` reported). Boot A: **J green, L green, K red,
`L6C-K-RAW:` empty**, and once per leg on the console:
`/gate/run.sh: line 9: fcntl(3,F_DUPFD,10): No file descriptors available`.

busybox ash's `redirect()` probes the TARGET fd of every `N>&M` with
`fcntl(N, F_DUPFD, 10)` to learn whether N is open — `EBADF` means "not open,
nothing to save"; anything else is "strange" and aborts the command. The
vivarium's `VIV_FCNTL_DUPFD` arm answered `EMFILE` for BOTH of
`handle_dup_posix`'s folded failures, on a comment arguing that a guest which
just used the fd knows it exists. True about the wrong caller. So the whole
capture never ran, the substitution yielded "", and two negatives were
satisfied by a broken fixture — aux#215's class, caught by the remedy aux#215
prescribes. Without K this would have shipped as two green legs proving
nothing. Fixed in the same commit (a liveness re-check after a failed dup:
closed → `EBADF`, residual → `EMFILE`; `vivarium.fcntl_dupfd_errnos`).

Boot A2 then showed a second fixture wart: `head -n 1 >/dev/null` printed
`can't create /dev/null: Function not implemented` — ash opens `>` with
`O_CREAT|O_TRUNC` and `O_CREAT` is a KNOWN unserved openat flag (#201, designed
around). The legs still measured SIGPIPE correctly (the reader slot died before
reading instead of after one line), but a fixture must not lean on a known
gap: the reader now writes its one line INTO the capture, so J's assertion is
the sharper "the capture is EXACTLY `y`" — the reader really read, the writer
was silent.

### The bar

Suite 1405/1405 (+2). Sabotages, each reddening its named assertion and
nothing else: S1 (terminate gate dropped) → `A: the terminate scan does NOT
name the CAUGHT interrupt`; S2 (stop gate dropped) → `D: the stop PREDICATE
declines a caught susp`; S3 (phenotype `exits()` disabled) → suite green,
L-6c `first-missing=L6C-J`, L missing, K present. pty + pty_stop: 4 clean/
liveness cfgs green, 6 buggy cfgs violate (rc 12/13) — after fixing the runner,
which first "passed" all ten legs in 0 s because `/usr/bin/java` is the macOS
stub and every rc was 1 for the wrong reason (the buggy legs read as
violations). Keyed on the exit code AND the `TLC2 Version` banner now. SMP gate
40/40 (default+UBSan × smp4/smp8, N=10, 0 corruption). LS-CI 33 PASS + 2 SKIP (GL not
baked) — and pty-4 burned a retry AGAIN, this time INTO the failure-time probe
landed at `11173762`: see the next entry, because the probe answered.

### Still open leaving this run

- #237 (native `pipe` has no latch) is sharper, not closed: the phenotype
  answers SIG_DFL SIGPIPE for its own Procs; a native handler-less, fd-less
  program still keeps a stranded `pipe` note.
- The tail's delivery-time SIG_IGN discard arm is reached by nothing (second
  unconstructed state on this row); its own chunk.
- `>/dev/null` from a Linux shell under viv fails on `O_CREAT` (#201) — the
  most common redirection in existence; the L-6c fixture routes around it.
- pty-4's burned retry: instrumented, not diagnosed.

## 2026-08-17 (aux) — pty-4's burned retry, diagnosed on the probe's first miss: the ldisc flushed type-ahead

The failure-time probe landed at `11173762` the day before, on the theory that
INPUT truncation and OUTPUT loss are indistinguishable in a plain capture and
only the guest can say which. Its first miss (LS-CI batch 6 of the c8ab2744
close bar) said, in order: `[listen]` — the raw stream showed `sle` as PLAIN
echoed text after `PTY-INNER`, then only SIX empty editor redraws where the
passing attempt shows NINE (`sleep 30\r`); `[jobs]` — nothing listed;
`[channel alive?]` — the editor answered; VM alive, bridge alive. The editor
never echoes typed text (the harness header says so), so plain `sle` can only be
the pts line discipline echoing in cooked mode.

So: `lc_run_expect` returns the instant `PTY-INNER` is SEEN — before `ut` has
reaped the pipeline, restored PROMPT_MODE and redrawn — and `lc_send "sleep 30"`
fires at once. On TCG the window is sometimes wide enough that `s`,`l`,`e` land
in CHILD_MODE (+icanon +echo): assembled, echoed, then ut writes PROMPT_MODE and
ptyfs `ctl_apply` does `p.line_len = 0; // TCSAFLUSH: a mode change resets the
assembly` — the three bytes are gone and `ep 30\r` reaches the raw editor. A
race, and a real one — but the DEFECT is the guest's: Plan 9's `devcons` `rawon`
pushes the partial line to the reader ("flush output on rawoff -> rawon", the
clumsy-hack zero byte), Linux's `n_tty_set_termios` never discards on a canon
change, and TCSAFLUSH is a caller-chosen flush that bash/readline deliberately
do NOT use (`TCSADRAIN`). Thylacine's ctl grammar offered no choice: every mode
write flushed. Type-ahead across a job's end — a paste of two lines, a script
driving a pts, LS-CI — lost the HEAD of the next line and executed the TAIL.

The posture came from the LS-8b audit's F1 remedy ("a fragment stranded across
canonical→raw→canonical prepends the next line"), copied per-pts by PTY-2c, on
the stated premise that "no current consumer flips mid-line". The premise was
falsified by the one consumer that flips around every foreground job. Both
ldiscs now DELIVER on ICANON-clear and touch nothing otherwise
(`c62eb738` scripture, PTY-DESIGN "Mode writes deliver, never discard"; the impl
`ccb597b8`): the F1 hazard stays closed because canonical→raw delivers, so nothing is
stranded, and I-20's byte conservation now holds across a mode write. A
delivery into a full ring is a real drop under a new counter
(`rx_drop_modeflush`, the #95 rule). Not built: an explicit flush verb — pouch's
`TCSETS/SW/SF` all map to the one write, which now behaves like `TCSANOW`.

Two things worth keeping from this: (1) the instrument earned its keep on its
FIRST miss, and the reason it could is that it asked the guest in a fixed order
with a control at the end (`channel alive?`); (2) a "posture" chosen as an audit
remedy is still a claim about consumers, and consumers change — the sentence
"no current consumer flips mid-line" was true when written and had no test.

## 2026-08-17 (aux) — the "reached by nothing" discard arm, and why the right fix moved the mechanism instead of reaching it

Resumed from aux's **second** self-compaction (`05708496`). The resume note's
first item was to ask the operator for the owed prosecutor round on `ccb597b8`;
the ready-to-paste prompt was written first
(`memory/audit_ccb597b8_prosecutor_prompt.md`), my own self-audit of the
mode-write delivery found nothing, the operator said yes, and the round is
running in the background while this chunk lands (its files — `cons.c`,
ptyfs — are disjoint from this one's).

### The chunk: Stream 4's "delivery-time SIG_IGN discard arm is reached by nothing"

The open item was the second member of the unconstructed-state class found by
sweeping after the maskstop one: `notes_deliver_at_el0_return`'s phenotype
branch discards a candidate whose disposition is `SIG_IGN`, and its own comment
named the only way in — "a note queued BEFORE the install is still sitting
here … this is the only place that can happen." The post-time hook prevents the
state in every ordinary ordering, so the arm needed a CONSTRUCTED one.

Constructing it in-guest turned out to be easy — `viv-pheno-probe` has raised
SIGPIPE at will since V-6c (fd 0 is a reader-less pipe write end), so
`block → write → SIG_IGN → unblock` is deterministic. But writing the legs
forced the question the arm's comment had skated over: **what does POSIX say
happens to a pending, blocked signal when its disposition becomes `SIG_IGN`?**
It is discarded AT THE INSTALL, "whether or not it is blocked" (2.4.3; Linux
`do_sigaction` → `flush_sigqueue_mask`). Thylacine discarded at the next
delivery instead. Same answer for `pending → SIG_IGN → unblock`; a DIFFERENT
answer for `pending → SIG_IGN → handler → unblock` — Linux fires nothing, the
tail ran the handler for a signal POSIX says had died. So the arm was not merely
uncovered; the mechanism it implemented was wrong on the ordering nobody had
tested, and the honest fix is not to reach the arm but to move the discard.

What landed: `notes_discard_name(p, name)` — under `q->lock`, remove every
queued note of one name, mask-blind, each removal draining the class latch as a
dequeue does (an `interrupt` armed under `SIG_DFL`, then ignored while blocked,
must not leave a Proc whose every sleep is `*_INTR`), `kill` refused; the
phenotype `rt_sigaction` shell calls it after the store whenever the new
disposition ignores (`SIG_IGN`, or `SIG_DFL` for a default-ignore signal — the
no-table `SIG_DFL` shortcut now skips only the store); and `notes_post`'s
disposition read moved UNDER `q->lock`, so store-then-lock against
read-under-lock leaves no interleaving with a stale ignored note. The tail's
arm stays as defense-in-depth — its absence would hand a stale note to the
`SIG_DFL`-terminate arm — with its comment rewritten to say exactly that.

The proof: `notes.discard_name_purges_pending` (mask-blind, per-CLASS latch
drain — tty:hup out leaves the TTY latch armed for tty:quit — survivor order,
`kill` refused, a purged FULL ring really empty: 16 out, 16 in) and probe legs
L205–L216. Round A: pending → `SIG_IGN` → unblock survives with nothing fired
(L209 is PRE-STAMPED and rewound so a death names its leg instead of leaving
joey's `??` — the marker channel is fail-only by design, and this is the one
place a marker is written before the verdict is known), then a handler
installed after is not handed a stale note (L210). Round B: pending →
`SIG_IGN` → handler → unblock fires NOTHING (L215 — the install-vs-delivery
leg; red on the tree before this chunk). Each round ends with a fresh SIGPIPE
delivered exactly once, so a queue wedged by the experiment cannot read as
"nothing fired".

### Found on the way, enqueued not fixed

Reading `proc_exec_drop_image_state` for the exec-time sigtab reset: it zeroes
every row and the mask, and its comment says "Zeroing is exact POSIX". True of
CAUGHT handlers; false of `SIG_IGN` and of the blocked mask, both of which POSIX
and Linux keep across `execve` (`nohup`, `sh -c 'cmd &'`, `trap '' INT; exec`
all depend on it). ARCH §7.6 names the clear as the NATIVE rule, so the fix is
phenotype-conditional and a scripture decision — surfaced with options in
`memory/bug_exec_resets_sigign_and_mask_phenotype.md`; recommendation:
phenotype keeps `SIG_IGN` + mask.

### The bar (`7580c1f7`)

Suite 1406/1406 (+1); V-1b PASS (L205–L216 green); L-6c PASS. Sabotages, each
reddening exactly its named assertion: S1 (the shell never purges) → V-1b
`marker=L215` — and NOT L209, because the tail's arm still saved that ordering,
which is the whole reason the arm stays; S2 (S1 + the tail's `SIG_IGN` disjunct
deleted) → `marker=L209` — the guest died at the unblock and the pre-stamp named
the leg; S3 (purge without the latch drain) → the unit test at "removing the
last interrupt drained the latch", 1405/1406. SMP gate + LS-CI ran over the tip
together with the round close below (see the fixup).

## 2026-08-17 (aux) — the ccb597b8 round came back: sound delivery, an unwitnessed counter

The operator said yes to the round while the chunk above was being built; the
prosecutor (Fable 5, read-only) ran ~20 minutes and reported 0 P0 / 0 P1 / 2 P2
/ 6 P3 — every finding on the NEW DROP SITE's witness, none on the delivery it
was asked to break. It re-derived the I-9 wake pairing, the poll relay, the SMP
ordering under `g_cons.lock`, the hook/production parity and ptyfs's
single-threaded ordering line by line and found them as claimed.

What it found instead is worth keeping. **F1**: the fifth drop site's counting
path had only a NEGATIVE test in both ldiscs — leg B "it fit, no drop counted"
against an empty ring — so a misattribution to `rx_drop_ring` (the must-stay-
zero witness) or not counting at all read green. The tree's own
`test_cons_rx_drop_counters` header says exactly why that is worse than no
counter, and I had shipped one anyway because the negative FELT like coverage.
Legs (d)/(e) now drive the site (512 filler + 10 pending → 10 counted here,
every sibling asserted unmoved, filler intact; 507 + 10 → the 5-byte PREFIX
delivered in order); the ptyfs selftest drives its site on a fresh pts.
**F2**: ptyfs had folded that drop into `drop_flush` — against PTY-DESIGN,
which named "its own counter" for BOTH ldiscs, and against `drop_flush`'s own
documented shape (a short cooked flush loses tail + newline so the line never
runs; a short mode-flush loses the tail and the terminator arrives raw, so the
truncated command RUNS — #95's exact shape, hidden under a name whose doc said
it could not produce it). One of two twins diverged from a rule written for
both, and a re-read of the scripture would have caught it. **F3/F4/F6/F7**: the
one-shot report did not name the new site; the "reachable only by a wedged
reader" claim was false (ut re-arms before it drains, so a paste can reach it);
three comments still said TCSAFLUSH; 111-cons.md carried the deleted test with
the reversed semantics. **F8**: pty-4's type-ahead leg had no ARMED witness —
bytes landing raw before CHILD_MODE or after the re-arm satisfied the cursor-35
anchor too, under the old posture as well; it now first requires the pts's
cooked echo as plain text directly after the CRLF, which only CHILD_MODE cooking
produces. **F5** stays open as a scripture vote: an ISIG-consumed ^C/^\/^Z does
not flush the pending canonical line (POSIX and Linux do; Plan 9 does not) —
the old reset masked it, delivery makes it visible; recommendation: adopt POSIX
in both ldiscs.

Closed at `56b5a412`: suite 1406/1406; S7 (kernel misattributes to
`rx_drop_ring`) → "(d) modeflush counts exactly the 10 bytes the full ring could
not take"; S8 (ptyfs folds into `drop_flush`) → `ptyfs: selftest FAIL:
modeflush-drop-not-counted`, boot-fatal.

### The bar over the tip (`56b5a412`, both commits)

One run for both (disjoint surfaces): SMP gate 40/40 — default + UBSan ×
smp4/smp8, N=10, 0 corruption / 0 external-kill / 0 other, in two halves —
then LS-CI in six batches on TCG: 33 PASS + 2 SKIP (the GL half is not baked
into this pool; not a guest result, not coverage). pty-4 passed WITH the new
armed witness (the pts's cooked echo matched before the cursor-35 anchor — the
delivery path was exercised, not merely reached). Pushed to both mirrors after
the fixup.

## 2026-08-17 (aux) — the votes came back: ISIG discards, fork/exec goes POSIX, and the 7580c1f7 round

The operator answered all three questions in one round: spawn the 7580c1f7
round (yes), F5 (adopt POSIX — an ISIG character discards the pending line in
both ldiscs), and the exec item (the phenotype keeps `SIG_IGN` + the mask). Each
landed scripture-first.

**F5** (`e69e9baf` scripture, `4df51c30` impl): the kernel ISIG arm and the
ptyfs ISIG arm zero the pending assembly when ICANON is set — a disposition like
an erase, not a counted drop, deliberately narrower than POSIX's full flush
(committed lines in the ring stay; output is never flushed — the console TX ring
carries kernel diagnostics). The PTY-3 pouch probe's leg H had pinned the OLD
posture (`x` ^C `y` CR → `xy\n`) and went red on the first boot — the fixture
that encoded the divergence, found by the change that removed it; updated to
`y\n` as on Linux. Sabotages S9/S10 each red on the named check.

**fork/exec** (`c484a7d1` scripture): reading `proc_exec_drop_image_state` for
the exec half surfaced the fork half too — task #127, recorded at L-3d as "two
behaviours and a design decision", never landed. So the chunk is the pair:
`rfork` copies the parent's sigtab into the child's OWN table (before the child
is postable) plus the caller's `note_mask`; `execve` resets caught rows only and
keeps `SIG_IGN` + the mask; native keeps the Plan 9 clear. Probe legs L217–L228
drive a real fork and a real exec (the children name the first wrong fact
through the report dup); the unit test pins the two primitives.

**The 7580c1f7 round** (Fable 5, 0/0/0/4) re-derived the install-time discard
SOUND — the linearization, the primitive, the shell, the pre-stamp arithmetic —
and found the one ordering nobody had tested: `block; SIG_IGN; raise; handler;
unblock`. Linux queues a blocked ignored signal ("the handler may change by the
time it is unblocked") and discards at dequeue; Thylacine drops at generation,
mask-blind. POSIX 2.4.1 permits both, so it is recorded as a stated divergence
rather than matched — but the docs had said "exactly as Linux", and the lesson
worth keeping is that "exactly as X" is a claim about every ordering. F1: the
SIG_DFL/default-ignore purge disjunct had no driver → L229–L232 with a positive
control (S13 reddens only the negative). F2/F4: an over-claiming comment and two
stale sentences.

### The bar over the tip (`d3a11c8e`: F5 + fork/exec + the round close)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves); LS-CI 33 PASS + 2 SKIP (GL not baked);
suite 1408/1408 per commit; sabotages S9/S10 (F5) and S11–S15 (fork/exec)
each red on the named check — S14/S15 are the WIRING witnesses (the unit test
cannot see proc.c; the probe legs L223/L226 can, and they went red). Pushed to
both mirrors after the fixup.

## 2026-08-17 (aux) — the d3a11c8e round: the fork rule was one field short

The operator said spawn; the round (Fable 5, read-only, 0/0/1/6) re-derived
both mechanisms sound — the fork copy is published before the child is
reachable and aliases nothing, the exec reset uses the same "caught" predicate
delivery uses, the ISIG discard is one field under the right lock in both ldiscs
— and found the one place the voted RULE was short. "fork copies everything
(POSIX fork(2))" copied what POSIX names: dispositions and mask. This design has
a third piece of thread signal state POSIX never has to name, because Linux
keeps it on the user stack: the kernel-side handler-execution snapshot (the
sigframe here is written for reading; `rt_sigreturn` restores from the
per-Thread save block). A `fork()` issued from INSIDE a handler — async-signal-
safe, POSIX-permitted — therefore produced a child whose user stack said "in a
handler" while its KP_ZERO thread said "not"; its handler return was refused
and it ran on past the svc into whatever followed the restorer (musl: silent UB;
the probe: `brk #0`). Fork+exec and fork+`_exit` from a handler were fine, which
is why nothing had surfaced. Fixed by copying the block with the mask
(`in_handler` written last, before `ready()`); phenotype only — a Plan 9 child
is not notified. Lesson: enumerate what the RESTORE path reads, not what the
standard lists.

The witness leg cost two extra boots for a reason worth keeping: its first
draft had the child exit 3 and the parent reap "exactly 3", and it went red on a
WORKING fix — v1.0's phenotype exit path collapses every non-zero
`exit_group(N)` to 1 (VIVARIUM task #91, "`exit(N)` is boolean"). A diag with
`exit(5)` read as 1 too. So the oracle is exit 0 versus anything else, and the
child's own marker (re-emitted by the parent on failure) carries the why. A
status oracle must be a value the status channel can carry.

Six P3s: a pre-#254 "known hazard" paragraph in `proc_exec_replace` that
contradicted the in-place reset it now calls; a phantom `viv_sigtab_copy_into`
in 145; PTY-DESIGN naming leg (f) for (e4); the ptyfs (e4) leg with no witness
for "m2s/s2m are NOT flushed" (both were EMPTY at the VINTR, so an over-broad
discard passed — it now commits `x\n` unread and leaves the echoes unread and
asserts both survive); the fcntl test's header comment migrated onto the sigtab
test; and the ISIG-DISCARD + ccb597b8-ROUND addenda living only on the
AUDIT-TRIGGERS rows that declare ARCH 25.4 authoritative (mirrored). Enqueued
from the observations: `Proc.socktab` is not cloned at fork (the fork half of
the LINEAGE dup3 note — a real L-6 gap for fork-per-connection servers), the
handler mask discipline (sa_mask|sig never applied during a handler; sigreturn
does not restore the mask), and `pty.tla`'s CookSignal echoing a char neither
ldisc echoes.

## 2026-08-17 (aux) — the console TX ring pushes UNITS now

Main handed over the byte-atomic tear it measured on thyla-pi: `proc: orphan
pid=2119 name="ttaappeessttrryydd"` — the kernel's orphan-adoption burst and
tapestryd's posture line on another CPU, byte for byte, because every producer
pushed each byte under its own `g_cons_tx.lock` hold and the writer role cannot
serialize a diagnostic emitter (IRQ context; the role sleeps). ARCH 23.5.2 had
already named the missing piece — "full echo-exclusion via a bulk-push fast
path" was #79, a documented v1.x item withdrawn from an earlier draft because it
"carries a two-ring lock-ordering design". The design point resolved as: never
nested. Tap under the drain lock, release, push under the ring lock, release.

The rule now: every producer pushes a UNIT under one hold. A kernel diagnostic
is a line assembled on the caller's stack (`struct cons_diag_line`) and pushed
once, all-or-nothing — the per-token trio is gone, because a per-token API
cannot be line-atomic without hidden state, and a per-CPU accumulator would
splice an IRQ handler's line into the process-context line half-assembled below
it; a caller-owned object is nesting-safe by construction. Echo pushes its
staged unit whole (half a `\b \b` walks the cursor over the prompt). The role
writer stages a 512-byte chunk, cuts it back to the last NL when the input
continues, pushes what fits and room-waits for the rest — so a ring-fitting
write, which is every console line, is whole against every producer. The
residual is named and Linux-equivalent: a long write spans chunks; a FULL ring
splits at a chunk boundary, because progress beats atomicity under congestion.

Three tests, one of them the tear's own witness: two kthreads hammer a STALLED
ring with 64-byte units from two CPUs, the ring is read back through a new peek
hook and parsed as frames, and every frame must be one producer's unit — with an
overlap witness so the test says whether the interleave was exercised (it was).
The other two pin the boundary deterministically on one CPU: room = len-1 moves
the count by zero and `dropped` by exactly len; room = len lands whole.

### The bar over the tip (`277b02cc`: the round close + the TX-ring unit)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed, so the whole
matrix re-ran); LS-CI 33 PASS + 2 SKIP (GL not baked; six batches, TCG); suite
1408/1408 (`920bbfca`) and 1411/1411 (`277b02cc`) per commit; sabotages
SF1/S16/S17/SP5 (the round close) and S1–S3 (the unit rule) each red on the
named check. Pushed to both mirrors after the fixup.

A number corrected on the way: three earlier bar stanzas and four status rows
said "LS-CI 34 PASS + 2 SKIP". Every bar today measured 33 + 2 over the same 35
scenarios, and so did the two before it; the 34 came from the c8ab2744 close's
"36 scenarios" — an `ls tools/interactive/*.exp` count that included `lib.exp`
— minus the two SKIPs. A derived figure propagated as a measured one, six
times, before a run's own tally was set beside it. The tally is now taken from
the harness's `==> LS-CI:` lines only.

## 2026-08-17 (aux) — the handler-time mask is Linux's; three socket findings; a file count that was not a scenario count

Item 7 of the notes line was the smallest thing on the queue and the only one
without a vote in front of it (the #237 `pipe` default and the socktab posture
both alter user-signed scripture), so it went first while the votes ride the
report. The d3a11c8e round had recorded two permissive-direction divergences:
delivery never applied `sa_mask | sig` while a handler ran — N-3's blanket
`in_handler` guard stood in for it — and `rt_sigreturn` did not restore
`note_mask`, so a handler's own `rt_sigprocmask` outlived the handler, and an
`execve` from inside a handler handed the image the PRE-handler mask where
Linux hands it mask | sa_mask | sig.

The change is three lines and a field. `notes_deliver_linux_locked` saves the
pre-handler mask into a new `Thread.note_saved_mask` and stores Linux's
`signal_delivered` value — mask | sa_mask | sig, sig omitted under
`SA_NODEFER`, both additions through the same coarse translation as
`rt_sigprocmask` (a tty-family `sa_mask` entry blocks the family; SIGKILL is
dropped); the phenotype's `rt_sigreturn` restores the saved mask, gated on
`t->proc->phenotype` because a PHENO_LINUX Proc reaches delivery only through
the Linux path and a native Proc never does; and the fork-from-inside-a-handler
copy from the round's F1 gained the field — the round's own lesson, "enumerate
what the restore reads", applied to the next field. Delivery is untouched: the
guard still holds every note for the handler's duration (VIVARIUM 6.22's stated
conservative imprecision), so what changed is the mask a handler OBSERVES and
PASSES ON. The frame's `uc_sigmask` still carries the pre-handler mask and is
written for reading — a handler that edits it changes nothing, which Linux
would honour; recorded as the conservative-direction divergence of this frame
design. Native `noted` keeps the as-built rule.

Two things the witness taught. A signal with no note (SIGUSR1/2) reads back
CLEAR whatever is blocked — the translation has nothing to set — so a
`sa_mask = {SIGUSR1}` witness would have proved nothing; the legs use SIGINT,
SIGCHLD, SIGWINCH and SIGPIPE, one note bit each. And the pre-handler mask is
{SIGCHLD}, non-zero on purpose: a restore that puts back ZERO is
indistinguishable from a correct one against an empty pre-handler mask, and
the fork leg (the child forked from inside the handler restores at ITS
sigreturn) is exactly the leg a missing copy would pass with zero. The Thread
grew by a u64 and its size did not change — the 8 bytes landed in the pad
before the 16-aligned FP area — and that was measured with
`-fdump-record-layouts` before the size assert's message said so, not derived.

The first boot reddened the "handler's own block undone" leg on a WORKING
restore, and the reason is the reusable part: probe leg L26, far above, blocks
SIGWINCH to assert the tty family's honest over-report — and nothing since
unblocks it. So the pre-handler mask carried the tty bit, the restore put it
back exactly as it should, and the leg read that as "the block persisted". A
premise assumed is a premise that can be false without anyone's fault; it is
now asserted as its own leg (L237: the pre-handler mask is exactly {SIGCHLD}),
with the tty family unblocked first and re-blocked after so the legs below run
under the state they always had.

Sabotages, each red on exactly its named check: SM1 (no handler-time store) →
probe L239 (the mask inside lacks sa_mask|sig; 1413/1413); SM2 (no restore) →
`notes.phenotype_sigreturn_restores_mask` leg A (1412/1413 — the suite fails
first, so the probe is not reached; L240/L241 had already shown they
discriminate, on the premise failure above); SM3 (the fork copy skips the
field) → probe L244 only (the child forked from inside the handler restores
zero, and zero is not {SIGCHLD}).

### Three socket findings, from reading before touching

The socktab item (fork does not clone it) was researched instead of started,
and the research moved it. The enqueued plan said "a refcounted entry"; a
refcounted ENTRY cannot carry the ctl->data handle swap `connect` performs in
one table, so it reproduces Linux no better than a per-process copy — and a
per-process copy is Plan 9 APE's own posture (rocks live in process memory;
fork copies them). Every fork shape that occurs (accept-then-fork,
prefork-accept) works under a copy; the divergence — a state mutation through
one alias not seen through another — is the one LINEAGE already published for
dup3. VIVARIUM 5.5.2 states today's "not rfork-inherited" as design, so the
flip is the operator's vote (`memory/design_socktab_across_images.md`).

Alongside it, two defects verified in the tree. `handle_close_on_exec` closes
a close-on-exec socket handle and pays no socktab drop, and `fcntl(F_SETFD,
FD_CLOEXEC)` is a served row — so `socket; fcntl; execve` leaves a stale
(proto, N) entry keyed on a number the new image's next fd-creating call is
handed: the "dial verb to a stranger" class the V-5 header names as the
sharpest this table can have, reached through exec rather than dup. And the
reach is wide, because of the third finding: `socket()` answers EINVAL for
`SOCK_CLOEXEC|SOCK_NONBLOCK` "rather than masking them off", and EINVAL is
exactly musl 1.2.5's fallback trigger (`third_party/musl/src/network/socket.c`):
it retries without the flags, then issues `fcntl(F_SETFD, FD_CLOEXEC)` — served,
so every musl `SOCK_CLOEXEC` socket reaches the stale-entry path — and
`fcntl(F_SETFL, O_NONBLOCK)` — unserved, ENOSYS, and musl ignores the result. The
guest ends up holding a BLOCKING socket it believes non-blocking, the very
failure the refusal's comment says it prevents. A refusal is only as honest as
the libc that receives it; the claim was verified on the artifact, not on the
kernel's return value. Both enqueued (memory + AUX-ROADMAP), main told
(V-5 is theirs).

Also to verify, not yet verified: holotype R5-F9 (longjmp out of a handler
wedges `in_handler`) was registered against pouch programs, but busybox ash's
`raise_interrupt` longjmps out of the SIGINT handler when interrupts are
enabled, and the phenotype population is every musl-static shell. One VM
experiment settles it; if real it is a P1 for interactive shells and needs an
abandoned-frame rule (design).

### The count that was a file count

The push bar over `277b02cc` measured LS-CI at 33 PASS + 2 SKIP; the record —
three JOURNAL stanzas, four status rows, this session's own resume note — said
34 + 2. Every bar today measured 33 + 2 over the same 35 scenarios, and the two
full runs before them said "32/34; 2 SKIPPED" in the harness's own words. The
34 was the c8ab2744 close message's "36 scenarios", an `*.exp` count that
included `lib.exp`, minus the two SKIPs: a derived figure that propagated as a
measured one six times before a run's tally was set beside it. Corrected
everywhere; the tally now comes from the harness's `==> LS-CI:` lines only.

### The bar over the tip (`01f076f2`: the handler-time mask)

SMP gate 40/40 (default + UBSan × smp4/smp8, N=10, 0 corruption / 0
external-kill / 0 other, two halves — the kernel byte-changed); LS-CI 33 PASS +
2 SKIP over 35 (GL not baked; six batches, TCG); suite 1413/1413; sabotages
SM1/SM2/SM3 each red on the named check. Pushed to both mirrors after the
fixup.

---

## 2026-08-16 — Warp-C C-1, the per-slot decision, and one third of the extinction tear

Resumed from a self-compaction at the 600k checkpoint. **The nudge fix worked
on its first live test** — the detached watcher fired behind `/compact` and the
far side woke itself, which is the loop the operator had been closing by hand at
every boundary.

### Warp-C C-1 — the composed present, modelled (`ee581fbd`, fixup `ae9a25df`)

GPU-DESIGN §4.5.6 is binding here: `tapestry_present.tla` is model-first, so the
model is extended *before* the impl. Added the GPU-composed present behind
`ALLOW_COMPOSE` — `Attach`/`Detach` (P1b's authority-conferral point),
`ComposeBlit`/`ComposeComplete`, `DrainedOfBlits` on `ServerRelease` + `Free`,
and two invariants repeating T-1's own LIFETIME/CONTENT split: `NoTornCompose`
and `NoStaleCompose`. Eleven cfgs, gated by the new `specs/check-tapestry.sh`.

**The control was set before the work, which is the only reason it meant
anything.** I recorded every cfg's distinct-state count *before* touching the
module, so "this extension is additive" became checkable: with `ALLOW_COMPOSE =
FALSE` the six pre-existing cfgs must reproduce 5413 exactly. They do — and the
check earned its keep, catching that tracking `filled` unconditionally cost the
direct path 5413 → 10413 states.

**Two measurement traps, both mine, both caught by controls rather than by
reasoning:**

- My first comparison harness reported all six cfgs as DIFFERING. The harness
  was broken (`set --` inside the loop clobbered the positionals, lagging every
  expectation by one row). But under the bad labels the raw numbers still said
  something real, and chasing *that* was the right move.
- The buggy cfgs genuinely did differ — and it turned out **the metric was of
  the instrument**. A buggy cfg halts at the first violation, so with parallel
  workers "states explored before tripping" is scheduler noise: measured
  129/141/155 across three *identical* runs. Buggy cfgs are now judged on exit
  status plus the *name* of the invariant reported. (Never on TLC's prose — it
  writes both "is violated" and "was violated" depending on property kind.)

**Then TLC refuted my model, and the tree refuted the premise under it.** I had
carried the in-flight blit as the *slot* it reads, reasoning that a client
filling a *different* slot during a composition is legitimate pipelining — and I
wrote that justification into the module header as though it were established.
It is false. `usr/tapestryd/src/gpu.rs:1515-1518`: tapestryd allocates one 2D
resource per surface, attaches the whole weave as backing, and transfers at a
per-present *offset* that selects the slot. Guest-side slots buy **no** host-side
concurrency. The guard also had the shape of a known trap — `intransfer = 0` is
a gauge reading zero, equally true of "the fill landed" and "no fill was ever
issued" — now closed by an explicit `filled`.

The exclusion is symmetric, so it gets a sabotage *per direction*
(`buggy_blit_during_fill`, `buggy_fill_during_blit`) rather than one flag opening
both gates, which would only ever demonstrate whichever end TLC reached first.

Non-vacuity was measured, not assumed: coverage shows the composed actions fire
`0:0` with the switch off and `ComposeBlit` 2264 / `ComposeComplete` 7328 with it
on, so the green sits over a constructed state.

**Verification:** 32 spec modules green + the 11-cfg tapestry gate. `corvus` and
`handles` deliberately not re-run — 87 minutes, and nothing `EXTENDS`
`tapestry_present`, so they cannot be reached by this change. Zero build inputs
changed (proved by `git diff --name-only`), so the full bar's other legs carry
from `ca50a164` by construction rather than by assertion.

### The design fork it forced — and the operator's vote (`14f8c1ed`)

C-1 surfaced an obligation **the prose did not have**: the D1 recycle gate does
not survive the composed path unchanged. In the direct path a present's terminal
CQE genuinely means "the host has finished reading" — until the compositor
becomes a second, async reader of that one host resource, at which point the CQE
stops meaning the resource is free and nothing in the old rule notices.

Researched before posing it (Wayland `wl_buffer.release` + `drm_syncobj`, Android
BufferQueue acquire/release fences, Fuchsia buffer collections), which showed the
SOTA answer is *two* mechanisms, not one: buffer-release semantics for software
clients, explicit fences for GPU ones. Posed the fork with that attached.

**Operator chose one host resource per slot (3×).** Landed as a scripture commit
with no code, per the design-conversation pattern: GPU-DESIGN §4.5.8, with the
two rejected alternatives and their reasons, and the cost stated rather than
buried (3× host VRAM; ~100 MB at 4K, against a 64-MiB weave cap that already
cannot hold a triple-buffered 4K weave). The landed model does not change with
the vote — `NoStaleCompose` is whole-generation, correct today and merely
conservative once slots become distinct host objects.

### The extinction tear — one third of it (`44a8d53f`)

A surfaced soundness defect outranks the perf arc, so I stopped C-2 and took
this. The `EXTINCTION:` ABI line is emitted as four separate unlocked
`uart_puts` calls; every consumer anchors its match (`^EXTINCTION:` in
`tools/test-fault.sh`, and bare-token matchers elsewhere). A torn banner is
therefore not cosmetic — it is **a real extinction the harness cannot see**,
fail-open on the one channel the whole test discipline trusts.

**The vault already carried an adjacent seam, and I nearly conflated them.**
There are **three** tearing sources with confusingly close names:

1. extinction vs extinction — the re-entrancy guard is per-CPU *by design*, so
   two dying CPUs both print. **Fixed** (`extinction_claim_console`).
2. extinction vs a peer's *normal* console write — the vault's
   `seam-extinction-line-unserialized`. **Open.**
3. `IPI_HALT` — would subsume both. **Open**, a commented-out reservation.

The fix is one `__atomic_exchange_n`: a raw atomic rather than a kernel spinlock
(this runs on a dying machine, often inside a fault handler, and a primitive
carrying lock-order assertions could itself fault), try-once rather than spin
(the winner never releases, since every path ends in `_torpor`), and losers park
emitting nothing — because the failure modes are asymmetric: a torn line can be
read as a clean boot, a missing one leaves the guest visibly hung. Take the loud
failure.

**The fix introduces its own fail-open, and that is what most of the design
guards.** Nothing releases the console, so anything claiming it spuriously
silences every later extinction in the boot — the same defect from the other
side. Hence the deliberate interface split: the claim core is exported to be run
on a *caller-supplied* word, and nothing exports a way to claim the live one. A
test that took the real console would disable extinction reporting for every
test after it, silently.

**Both new tests were sabotage-verified** (1367/1367 → 1365/1367, each failing
on its own distinct assertion message). And the first one is documented for what
it does *not* cover: it is sequential and the property is a race, so a non-atomic
`if (*w) return 0; *w = 1; return 1;` passes it identically. Covering the real
regression needs a multi-CPU fault-injection arm with a **forced** interleaving —
without forcing it the pre-fix build garbles only sometimes, and a discriminator
that fails only sometimes is not a regression test. Tracked, not skipped quietly.

Also corrected a phantom that had propagated into two files: both
`kernel/extinction.c` and the header told readers to co-update
`tools/agent-protocol.md`, which was planned in Phase 1 and never written, and
`tools/run-vm.sh`, which matches neither literal because it only launches QEMU
and never reads boot output. Both now point at the vault's `abi-boot-banner`
mirror set instead of a transcribed list.

**Verification (the full bar, since this is a kernel change):** build clean;
suite 1367/1367 (was 1365; +2); SMP gate 40/40 with 0 corruption across
default-smp4/smp8 + ubsan-smp4/smp8; LS-CI 35/35 PASS; v8.0 floor OK.

**A killed gate is not a green gate.** The first LS-CI run was stopped by the
harness (`Terminated: 15` on its scenario subprocesses) after I ended a turn
while it ran; the SMP gate had survived the identical foreground → background
migration earlier in the same run, so what differed was ending the turn. Re-run
as a tracked background task, staying in-turn.

**And then I got the reasoning for that right conclusion wrong, twice, the same
way.** I first wrote that the killed run "recorded zero verdicts", inferring it
from a stdout log containing only `==> start:` lines. Then, waiting on the
re-run, I read the same channel and concluded it had produced no results after
eight minutes. Both readings were of the wrong channel:
`tools/test-interactive.sh` says so in its own comment — *"The verdict is a
FILE, not a counter"* — and writes results to per-slot `timings.tsv`, never to
stdout. The re-run was healthy the whole time (`go8d PASS` already on disk).

So: **a pattern that matches the wrong thing returns a confident wrong answer,
never an error** — a lesson already pinned in memory, re-learned twice in one
hour on one command. What makes it worth writing down again is that the wrong
instrument produced a *plausible* story both times (a killed gate really had
been killed; a slow gate really can be slow), which is precisely why it was not
self-correcting. The fix is to find where a tool actually writes its verdict
before reading any verdict from it.

### Before C-2 wrote a line: the composed path cannot run on the dev loop

Checked the precondition rather than assuming it, and it changed the arc. The
boot log of the very run I had just gated says
`tapestryd: gpu up -- 1280x800, pci intid=35, virgl=0 capsets=0`, and
`tools/run-vm.sh` defaults to `virtio-gpu-pci` — a device with no GL. So
`CTX_CREATE` / `RESOURCE_CREATE_3D` / `SUBMIT_3D` are unavailable on the primary
dev loop, and with them every mechanism §4.5 describes.

Three consequences, recorded as GPU-DESIGN §4.5.9. C-2/C-3 must be verified on
**thyla-pi**, not here. The composed path must be capability-gated on the
negotiated feature bit — a tapestryd that assumed GL would take the console dark
on the default device. And the third corrects the roadmap: **"C-4 retire the
readback path" cannot mean delete it.** That is forced twice over — by the plain
`virtio-gpu` that is the default here, and more fundamentally by bare metal,
where there is no virtio-gpu at all and virgl is a *virtualization* transport
with nothing to negotiate. The CPU path is the universal one; GPU composition is
the accelerated path where a GPU seam exists.

The cost is stated rather than left to be discovered: tapestryd carries **two
composition paths permanently**, and they must stay behaviourally identical from
the outside or the gate that proves one is silent about the other.

### The C-2 verification host, proven rather than assumed

Having established the dev loop *cannot* run the composed path, the next
question was whether anything can. Synced HEAD to thyla-pi (all 80 pool chunks
hash-verified, artifacts paired) and booted `virtio-gpu-gl-pci` under KVM on
real V3D:

```
tapestryd: gpu virgl -- num_scanouts=1 num_capsets=2
tapestryd: gpu capset[1] id=2 max_version=2 max_size=1384
tapestryd: gpu up -- 1280x800, pci intid=35, virgl=1 capsets=2
CAPSET GATE: VERIFIED
```

So C-2 has a working verification host, and the two figures — `virgl=0` here,
`virgl=1` there — are the whole argument for §4.5.9 in one line each. Worth
doing before the implementation rather than after: had C-2 been written first,
its first symptom on the dev loop would have been a dark console, which is a
long way from its cause.

### C-2a — the capability gate and the compositor context

The first landable piece of C-2: a reserved compositor virgl context
(`COMPOSITOR_CTX = 0x100`, far above the client `slot + 1` range so a client's
stream can never author against the screen), minted only where `virgl`
negotiated, and a startup line reporting which composition path the host can
actually take.

**The first cut reported nothing, and the boot passed anyway.** I had hung the
posture report off `ensure_screen`, beside the other display resources — but
`ensure_screen` runs only under `Scanout::Composed`, a state a normal boot never
enters, so the line sat behind an unconstructed state and printed on neither
host. The suite went 1367/1367 with the feature effectively absent. Which
composition path is *available* is a property of the HOST, fixed at feature
negotiation, so it now reports where the host is brought up.

**Verified on both arms, differing in exactly one variable** — a negative
assertion alone would have been satisfied by a broken fixture:

| Host | Negotiation | Posture |
|---|---|---|
| dev loop, `virtio-gpu-pci` | `virgl=0` | `composed path = CPU (virgl=0)` |
| thyla-pi, `virtio-gpu-gl-pci` | `virgl=1 capsets=2` | `compositor ctx 256 up` → `composed path = GPU` |

Getting the positive arm took one correction of its own: the `capset` verb
filters its output at the capset markers, so the Pi run *looked* like it lacked
the line when it had simply not been shown it — `boot-probe.sh` keeps the full
log on the host, and the line was there. A truncated capture and a missing
feature are the same reading until you check which one you have.

### C-2b — the 3D screen, landed gated and HONESTLY UNPROVEN on its own arm

The screen becomes a host-side 3D resource attached to the compositor context
where GL exists, falling back to the 2D resource everywhere else. Guest backing
stays on both paths, because at C-2b the screen is still CPU-filled — only its
host-side representation changes. `screen_push` grows a 3D arm, and there the
sync transfer moves the whole surface rather than the damage rect: a deliberate
trade, since C-3 deletes the CPU fill outright and building a rect path for a
mechanism already scheduled for removal is waste.

**What is verified, and what is not — stated because the gap is the finding.**
The FALLBACK arm is verified: suite 1367/1367, and LS-CI 35/35 where the
`ls-gfx` scenarios assert exact pixels via screendump and therefore cannot pass
without a working composed screen. **The 3D arm has never executed.**
`alloc_screen` runs only under `Scanout::Composed`, and neither the dev-loop
boot nor the Pi's `capset` boot enters it, so `screen res N 3D (compositor ctx)`
printed on neither host. `prove` produced no new boot log to grep.

So this lands **gated off on every host I could exercise** — dead on the dev
loop by capability, unproven on the Pi by opportunity — and the commit says so
rather than calling a clean boot a verification. Booting green proves the gate
did not fire, which is exactly what an `if (false)` would also prove.

**Then I found why, and it is a tooling gap rather than a code problem.** The
Pi logs say `tapestryd: scanout direct 0 (1280x800)`: every existing Pi verb
drives a SINGLE display-sized GL client, and that takes the **Direct** path —
scanning out the client's own resource and bypassing the compositor screen
entirely. §4.5.1 spells out the condition: Direct demands one visible surface
AND one visible leaf AND an exactly display-sized surface. So composed scanout
needs two surfaces, or one smaller than the display, and **no verb in
`warp-host.sh` produces either.** `capset` and `smoke` both land in Direct;
`tri` and `prove` left no new boot log at all.

That is worth more than a failed check: it says the composed path — the entire
subject of the Warp-C arc — has no driver on the only host that can run its GPU
half. Building one (two surfaces, or a mode change that un-sizes a single one,
which is what `ls-gfx-mode` does locally) is the next task, and it gates C-2b,
C-3, and the arc's exit criterion alike.

### The driver — C-2b's 3D arm finally executes, and my own note was wrong

The task I left myself was "build a Pi driver that forces Composed scanout."
Before building anything I checked the claim under it, and **it was false**. The
section above says "no verb in `warp-host.sh` produces either" — but
`glq-virgl.exp`, which `quake` runs, opens GLQuake in a window and its very
first assertion is `-re {scanout composed \((\d+)x(\d+)\)}` with the label
"composed entry (two leaves)". `decomp` and `wedge` split the layout too. What
was actually true is narrower and duller: the verbs I had *read the boot logs
of* — `capset`, `smoke` — boot with no client at all, so aurora alone is
display-sized and lands in Direct. I generalised from the two logs I had to a
claim about all ten verbs, and wrote it into two documents.

Worth noting how cheap the catch was: one grep for `composed` across
`tools/warp/*.exp`, run because the note asserted a negative over a set I had
not enumerated. **The evidence that a thing is absent has to come from the whole
set, not from the members that happened to be in front of me** — and a note
written confidently at a compaction boundary is exactly where that error
survives, because the far side inherits it as established fact.

I still did not use `quake`. It drags in the pool's `tyr-glquake`, S3TC quirks
(#216), the #198 storm, and 900-second timeouts — a lot of machinery that can
fail for reasons having nothing to do with C-2b. `/bin/tapestry-battery` brings
up two surfaces, lives in the ramfs, and needs no GL of its own, so **the only
GL object in the experiment is the compositor's own screen**. That isolation is
the reason to pick it, not availability.

`tools/warp/composed-screen.exp` boots, takes the posture line between boot and
login (it prints at bringup, which is where a host property belongs — a lesson
this arc already paid for), runs the battery, and asserts the screen mint. **The
control is the device**, which is why the scenario takes one as a parameter
instead of hardcoding the GL model: two legs, one host, one variable, each
asserting the other's outcome is wrong.

```
virtio-gpu-gl-pci -> composed path = GPU -> screen res 67 3D (compositor ctx) (1280x800)
virtio-gpu-pci    -> composed path = CPU -> screen res 67 2D (1280x800)
```

**C-2b's 3D arm has now executed**, on real V3D silicon through virgl. The
second line is what makes the first mean something: a GL-only leg would pass
identically against a tapestryd that ignored the negotiated bit and always
minted 3D. Two legs that *disagree* are stronger evidence than two that both
pass — the control produced a different answer rather than merely staying quiet.
Both legs minting `res 67` is a small corroboration on the side: everything
upstream of the branch is identical, so the arm is the only thing that moved.

The gate keeps two claims separate rather than collapsing them — posture matches
the device, screen arm matches the posture — so a host that had silently lost
its GL could not satisfy the second by making both sides equally wrong. And
`tools/warp-host.sh composed` requires each leg's scenario-completion line as
well as its screen line, because a leg that died immediately after printing its
screen line would otherwise still show the gate everything it greps for. That
term is not hypothetical caution: the `reject` verb in this same file shipped
grepping `C0-REJECT` while its producer printed `C0-DETECT`, and exited 0 on the
exact failure it existed to catch.

### Then C-2d refuted itself before it wrote a line (§4.5.8a, OPEN)

With the driver landed I went to implement §4.5.8 — the per-slot host resources
the operator voted for — and read the present path first. The decision does not
survive it, for a reason nobody had in view at the vote.

Three facts, each one grep:

1. Every client rotates slots on every present: `cur_slot = (cur_slot + 1) %
   nslots`, `libtapestry/src/lib.rs:525`, unconditional, both scanout modes.
2. Nothing copies content from slot *N* to slot *N+1*. `pixels()` hands back
   the raw current slot; there is no carry-forward anywhere.
3. **The single per-generation host resource is therefore doing a job nobody
   wrote down: it is the accumulation buffer.** A damage-only present transfers
   only its rect, so the host resource keeps the rest of the previous frame and
   the stale guest slots never reach the host.

Give each slot its own host resource and that job has no owner. A damage-only
present would render a three-frames-stale background around each fresh rect —
in Direct immediately, and in Composed at C-3. And the client this lands on is
**aurora**: it repaints only rows `r0..r1` and presents that rect
(`aurora/src/main.rs:1027-1038`), and it is the default Direct client on every
boot. The very line I have been reading all session, `scanout direct 0
(1280x800)`, is that client.

What makes this worth recording is not the catch but where the load was.
§4.5.8's analysis compared 3× / 2× / 1× VRAM and serialization — a complete
comparison of the properties anyone had *named*. The single resource's real
function was invisible because nothing declared it; it was an emergent
consequence of "transfer only the damage rect", and it had been load-bearing
for the console for as long as the console has existed. **A design comparison
can be sound over every property you listed and still miss the one the code is
actually relying on.** Only reading the path surfaces those.

I recorded it as **§4.5.8a** with four options rather than picking one, because
the vote is the operator's and this changes the terms they voted on. The
recommendation is buffer age — `EGL_EXT_buffer_age` and Wayland's
`wl_surface.damage_buffer` exist for this exact problem, Android's BufferQueue
exposes the same, and it keeps the per-slot vote intact at no VRAM cost while
retiring the latent hazard instead of routing around it. C-2c and C-3 both wait
on the answer: every option changes what gets attached and what gets blitted.

### The vote, and C-2d-a (`0a0e0fbb`, `931bf15a`)

The operator picked buffer age. Implementing it immediately hit a constraint the
option sketch had assumed away: I had written "present CQE now carries: age",
and it cannot. A present is a 9P write over the Loom ring, so its CQE is
**kernel-owned** — `result` is the write's byte count, `flags` is `LOOM_CQE_*`,
and `struct loom_cqe` is `_Static_assert`-pinned at 16 bytes. Putting a
compositor payload there is a kernel ABI break for a compositor convenience.

The way out was to notice who already owns the information. `libtapestry` owns
the rotation — `cur_slot` advances only after a present's own CQE — so it knows
exactly when each slot was last presented and can derive the age itself. A
`TEV_AGE` event was rejected (async to the present, so it races the rotation) and
a control word in the weave was rejected (a client-visible layout change for
something the client can compute).

**The interesting part is what the derivation costs, because it is the same
trap again.** A derived age is correct only if the client hears about every
server-side invalidation — which is exactly the kind of undeclared dependency
that produced §4.5.8a two hours earlier. So it is written down as a named
invariant this time rather than left to be rediscovered: tapestryd must not skip
a transfer without the client subsequently getting a redraw request, and a
redraw invalidates **every** slot, so the client repaints full for `nslots`
presents, not one. Both arms are wired in `libtapestry`.

Then aurora handed back independent corroboration of §4.5.8a. `main.rs:988`
already routes any OSD pass through the full-frame branch, with the comment
*"a partial rect could transfer stale panel pixels from an older slot"*. The
symptom had been understood locally, for one widget, and worked around — the
general statement just never got made. That is what an emergent load-bearing
property looks like from the inside: not unknown, merely un-generalized.

I split the chunk, because the halves are not symmetric: per-slot resources
without age break every accumulator, but age without per-slot resources is inert
and harmless. So the client half went first — and **its honest gate is that
nothing changed.** `ls-gfx` PASS, `ls-gfx-panes` PASS (exact pane-centre
pixels), suite 1367/1367. Its actual effect is unobservable until C-2d-b removes
the accumulator, and the commit says so rather than dressing a green boot up as
verification.

**Then I got the prerequisite list wrong, in the commit message, within twenty
minutes of writing the lesson that prevents it.** I swept for clients that
present partial damage with `grep 'present(Some\|present_rects'` and reported
three. That greps **API shape**, not the property that matters — *damage
smaller than the full surface*. Checked properly, it is one:

- `tapestry-battery` needs **nothing**. Every present is `present(None)`, and
  its one `present_rects` tiles the whole surface with two rects after writing
  every pixel. Its own header says so: *"presents FULL-FRAME only."* I had
  called it "the one with teeth."
- `tapestry-demo` is the real one, and is the sharpest example in the tree: it
  paints the quadrant background **into slot 0 only**, at frame 0, then draws
  just the plasma box into *rotating* slots forever after. Slots 1 and 2 never
  receive the background at all — they hold alloc-time zeros. Under per-slot
  resources, two frames in three would show black around the plasma.

"A pattern that matches the wrong thing returns a confident wrong answer, never
an error" is pinned at the top of my own memory index. It still went into a
commit message, a scripture section and the handoff, because a grep that
*returns results* feels like a sweep that *finished*. Corrected in §4.5.8b and
the handoff; the commit body stands as written, with this as its correction.

### The stop hook guarded the wrong stop, and the guard was never needed (`b3632942`, `cd0b3390`, `b61ca929`)

The operator noticed the Stop hook fired once in the long run and then went
quiet at a second stop it should have caught, and asked aux and me to work out
why. It is the third instance this week of the same family, and the sharpest.

**The measurement.** Replaying the hook's own parser over the real 805 MB
transcript: the silent stop sat at **530k / 73 turns** — inside the window on
both axes. So "it was correctly silent above the checkpoint" is dead. Isolating
the logic with synthetic input showed it behaves exactly as written. The cause
was upstream, and the pattern repeats: every firing is followed by silence for
the rest of the continuation, re-arming only when the user speaks or a
compaction lands.

**What I got wrong, and it was not the code.** `stop_hook_active` means "this
hook already triggered a continuation" — per-continuation by definition. I
exited early on it, which made the hook a once-per-*run* nudge guarding the
first stop and nothing after, i.e. the stop most likely to be earned and none
of the ones that follow. I kept that early exit because I believed it was the
loop guard.

**It never was.** aux fetched the contract: the harness overrides a Stop hook
after **eight consecutive blocks** (`CLAUDE_CODE_STOP_HOOK_BLOCK_CAP`). The
belay already existed one level up. So I had built a guard against a loop
something else was already preventing, and paid for it with the exact behaviour
the hook exists to provide. That is a different failure from a bug: **the code
did what I meant; what I meant rested on a contract I had not read.** No amount
of testing my own intent would have found it — only reading someone else's.

**The instrument came before the fix, and earned it twice.** The hook had nine
silent exits, so "correctly silent", "suppressed", and "crashed" were one
observation and any diagnosis could only be a guess — the same shape that had
just cost the vault a stranded day. So a ledger row on every path landed first.
Then it caught two things I would not have:

- Its own blind spot: the `stop_hook_active` parser printed `"1"` on exception,
  so a malformed stdin logged as `silent-stop-hook-active`. **The instrument
  built to separate those two causes could not separate those two causes.** The
  malformed-stdin test leg printed the wrong row, which is the only reason I
  looked.
- On its first *real* output: three rows in 24 seconds with incoherent context
  jumps, because the ledger is shared by main/aux/vault and I had dropped the
  session field from aux's spec. An interleaved log with no writer is worse
  than no log — it invites a confident reconstruction of one impossible session
  out of three real ones.

**And the fix validated itself in production before I finished writing it up:**
the reworded stem ("fires once per stop") came back in a live firing that
re-armed mid-continuation after real work — something the old version could not
do — with the ledger row `588458ctx/44t/27b/flag1` showing exactly why.

### C-2d-b landed, and the sabotage that proved it unverified (`f86177b6`)

The server half went in as voted: each generation mints `WEAVE_SLOTS` host
resources instead of one, backed per-slot instead of whole-weave. The
consequences were all followed rather than found later — `res_stale` becomes
per-slot; Direct binds the presented slot's resource and therefore rebinds every
frame (a KMS page flip, carrying the #57 post-bind flush); transfer offsets lose
their slot base, which the compiler confirmed by reporting `slot_stride` newly
unused; retire and `release_gen` unref all three or leak two per surface in the
process that IS the console.

`Held::Direct(Rect)` was the one that needed design rather than editing, and it
is why I stopped the first attempt at it. A rect union is well-defined only
while every held present lands on one resource; presents rotate slots, so two
held presents sit on different resources and `release` must flush each against
its own. Now `[Rect; WEAVE_SLOTS]` — bounded by construction, since a client
cannot hold more presents than it has slots.

**Then the sabotage passed, and that is the result worth the whole chunk.** I
disabled aurora's age handling with per-slot resources live — `stale_slot =
false`, `back = 0`, exactly the pre-C-2d-a client against a non-accumulating
server — and **`ls-gfx` still reported PASS.**

So the two gates I had been treating as verification are not. `ls-gfx` asserts
the frame *looks like* a console and that dumps *differ* after a command;
neither notices a stale background around fresh rows. `ls-gfx-panes` drives the
battery, which presents full-frame only and never exercises the accumulator path
at all. Between them they cover everything about the compositor **except the
property C-2d changes.**

That is the same trap as C-2b at the start of this run — a green result that
proves the gate did not fire — except this time I was the one about to be
fooled by it, having written the C-2b version into scripture that morning. The
difference between the two is not insight, it is that I ran the sabotage. Had I
not, this would have landed as "green on both pixel gates", which is *true* and
means nothing.

C-2d is therefore **implemented, not verified**, and the commit says so. §4.5.8c
records what the missing gate has to do: paint a region, damage a *different*
region, rotate all slots, sample the first region. `ls-gfx-panes` already has
the sampling machinery, so it is a scenario to write, not an instrument. The
focused audit is owed too — `usr/tapestryd` is an I-40 trigger surface and this
is the live scanout path — and could not run here because agent spawning is off.

### The self-compaction slot had two keys that did not agree (`7061115a`)

aux found this by reading the ledger nobody reads, and it is the best kind of
find: the mechanism had been quietly half-broken since it was built, and the
evidence had been sitting in a file the whole time.

`~/.claude/thyla-selfcompact/log.tsv` has vault's `allow` at 2026-08-16
10:44:32Z with **no `consumed` and no `nudge`**, and its `.note.pending` still
in the slot dir a day later. Every `main` row is paired; only vault's is
orphaned. That session compacted itself and was never handed its own resume
note — it sat at a prompt for the rest of the day.

The cause is a key mismatch, and **the comment is the interesting part.**
`tools/thyla-selfcompact.sh` said, in as many words: *"Two independent
derivations of one key, no shared config to drift."* The producer keys on `git
rev-parse --show-toplevel`; the consumer on `basename(dirname(transcript))`,
which is where the session was **launched**. Those coincide for main and aux
and do not for vault, which is launched from the thylacine tree and works in
thylacine-vault. So the comment **named the hazard and then asserted it away**,
and that assertion is what kept it unexamined for the mechanism's whole life.
It is every "keep these in sync" note that has ever rotted, except this one had
the confidence of sounding like an argument.

The fix needed no new identity, because one was already there and unused: the
arming script has always stamped `pane=$TMUX_PANE` into the meta, and a hook is
a child of the same claude, so it reads the same value. Pane match first, path
key as fallback.

**But the half that mattered was the silence.** The old failure was not doing
the wrong thing — it did *nothing*, and left no evidence, so `allow` without
`consumed` was the only trace. There is now an `orphan-note` row whenever a
pending slot goes unmatched, plus a 30-minute staleness discard.

**Then the test caught a bug in the fix that was worse than the bug.** The
first age check used `time.mktime` on a UTC stamp — `mktime` reads a
`struct_time` as *local* — so a note stamped that same second measured as an
hour old and was **discarded**. In any non-UTC zone that breaks every
legitimate resume: the repair would have converted a vault-only silent miss
into a universal one. I saw it only because leg 1 of the test printed
`stale-discarded` on a note written a moment earlier. Four legs, with legs 3–4
as the controls that make leg 1 mean anything — same note, same path-key
mismatch, only the pane varies:

```
1 pane matches, fresh    -> INJECTED,     consumed
2 pane matches, 25h old  -> not injected, stale-discarded
3 CONTROL no TMUX_PANE   -> not injected, orphan-note
4 CONTROL wrong pane     -> not injected, orphan-note
```

aux also retracted something in the same message, which is worth recording
because the retraction is worth more than the claim was: the "fourth
unregistered session" cited in the yip lease rationale **was aux itself** —
`ps -o ppid` on its own tool shell resolved to the process it had been reading
as a stranger. A census needs a control, and the control was its own identity.
Same family as `ps` matching its own command line, from the other end.

### Found in passing: `docs/REFERENCE.md`'s snapshot block died in Phase 5

The doc-update step sent me to `docs/REFERENCE.md` to refresh its Snapshot
block, which `CLAUDE.md` calls non-negotiable per chunk. **The newest "Tip"
bullet in it is a Phase 5 chunk** (`P5-stratumd-stub-bringup` audit close), and
there are 101 bullets behind it. The file's last commit of any kind is
`418688cf`, 2026-08-01. It contains **zero** occurrences of "Warp", "Tapestry",
"Clade" or "PTY-" — three whole arcs and a subsystem that do not exist as far as
the as-built technical reference is concerned.

So a binding per-PR obligation has been quietly unmet across roughly two phases,
including by me, several times this week. It is the "*a status field whose flip
is nobody's step stays unflipped*" shape: every chunk's author is told to
refresh it, no chunk's work makes them, and nothing fails when they do not.

**I deliberately did NOT patch my own bullet onto the top.** A dead list with
one fresh entry reads as maintained, which is worse than one that visibly
stopped — the reader trusts it again. The real question is what that block is
*for* now that `docs/phaseN-status.md` carries per-chunk rows and this journal
carries the narrative; answering it is a scripture-shaped decision, not a doc
edit to slip into a tooling commit. Enqueued rather than fixed in passing, and
enqueued in memory because the tracker is down this session.

### The gate that sees C-2d, red under both sabotages — and the defect building it found (after the self-compaction at `a733402e`)

Resumed from my own note with one instruction: build the §4.5.8c gate on aurora
in Direct, and validate it by re-running the sabotage that had passed `ls-gfx`
and requiring red. That is what happened, with two things the note did not
anticipate.

**The gate** (`tools/interactive/ls-gfx-age.exp` + `gfx_region.py`). Fill three
times with `yes … | head -n 200` so every slot carries glyphs; a POSITIVE
control — the same region assert, four keystroke-rotated dumps, each must show
text (a negative with no positive twin is satisfied by a broken fixture); then
`clear`, which blanks every cell in one all-rows present into ONE slot; then
eight rounds of keystrokes + dump, region exactly Bonfire, every pixel read.
The region is in cells (rows 6..rows-3, cols 2..cols/2) off aurora's own
`console up` line, so a font change moves it rather than breaks it.

**What the note left to the author, and how it was decided.** The detector is
slot-phased: the screen shows the slot presented LAST, so one dump samples one
slot. I had written "probabilistic — require N consecutive dumps". Working it
through, the honest model is *driven*, not sampled: each keystroke is a
row-0-only redraw, i.e. one present into the next slot, so the rounds advance
the phase deterministically plus whatever blink presents fall in the round.
That reframing exposed the real trap: **a broken client can have ONE stale
slot, not two** — an off-by-one in the union (`back = age-2`) leaves exactly one
— and the 1,2,3,1,2,3,… key pattern I first sketched (meant to break any
phase-lock with the blink) visits residues 1,0,0,1,0,0,1,0 under `b=0`: it never
reaches residue 2 and would pass an off-by-one every time. A plain one key per
round does reach it (1,2,0,1,2,0…) but is the pattern a 60 Hz blink can
phase-lock. So the negative leg types 1,1,2,1,1,2,1,1 keys, which visits all
three residues for *any* constant blink count per round (checked for b=0,1,2 in
the header); the
independence bounds — 3^-8 for the no-age class, (2/3)^8 = 3.9% for the
one-stale-slot class — are the fallback if the blink rate varies mid-leg, and
the header says which claim is load-bearing.

**Measured** (HVF, 128×36 cells, region 368 280 px). Fixed build: positive
63 882/368 280 non-bg on 4/4 dumps (identical counts — every slot holds the
same fill, as a correct client guarantees), negative **0/368 280 on 8/8**,
43 s. **S1** — the §4.5.8c sabotage, `stale_slot = false` + `back = 0`: **red
3/3 attempts**, at rounds 2, 1, 2 (63 882 stale px, i.e. the pre-clear fill
verbatim). **S2** — `back` off by one: **red 3/3**, at rounds 2, 5, 2. The
five-round attempt is the 1,1,2 pattern paying for itself: four dumps landed on
the two good slots before the fifth reached the one stale one. Restore green.
Both sabotages applied and reverted with `Edit`, and `grep SABOTAGE` empty
before the restore build.

**The defect the gate found — in C-2d-a, not C-2d-b.** Reading aurora's damage
branch to predict the sabotage outcomes, I traced what `931bf15a` records into
`dmg_hist`: **the WIDENED range** ("this is what actually reached the slot, and
the next union reads it"). That reasoning conflates *repaint* with *damage*.
The union answers "what changed since slot X was last presented"; what changed
between two presents is the dirty span, and the widening only says how much of
it THIS slot had to catch up on. Recording the widened range makes any
full-rows entry — every scroll — re-enter every later union, so every present
after it repaints all rows, forever. Aurora has been repainting the whole grid
on every cursor blink since C-2d-a landed: correct pixels, dead damage path.
Fixed to record the dirty span (`dirty0, dirty1` captured before the widening);
a full entry now falls out of the window after `nslots` presents. Two things
follow that are worth having in writing: S2 is a sabotage only against the
*fixed* recording — under the widened one an off-by-one is masked, since any
`back ≥ 1` propagates the full-rows entry (the old code had slack precisely
because it had no damage path); and the tight recording is guarded by the gate
that was built in the same chunk, which is the right order.

**Wrong turns, caught:** the first run failed on my own Tcl (`gfx_dump` takes
two args and I passed one) — three attempts, ~30 s each, all on the harness
side, before a pixel was read. And the resume note's "the sampling machinery is
in `ls-gfx-panes`" was true and unhelpful: `ppm-sample.py` reads one pixel; the
gate needs a region census with a positive control, which is a 40-line tool.

**Owed, unchanged:** the focused audit on `usr/tapestryd` (I-40; agent spawning
still off). The vault-owned prose (`sub-aurora`, `sub-libtapestry`,
`sub-tapestryd`) for C-2d and the recording fix goes over yip; the local
reference carries the gate.

### The device's OK was never the renderer's verdict — C-2b's "3D" word re-earned

Found while designing C-2c's gate, and by the one move that keeps saving this
arc: reading the source of the thing making the claim before repeating the
claim. My C-2c draft was about to say, for the third time in a week, that a
`CTX_ATTACH_RESOURCE` answered OK "attests the host accepted it". Before
writing that I fetched QEMU v10.0.0 `hw/display/virtio-gpu-virgl.c` (thyla-pi
runs 10.0.11) and read the handlers. **They ignore the `virgl_renderer_*` return
value** — for `CTX_CREATE`, `RESOURCE_CREATE_2D/3D`, `CTX_ATTACH/DETACH`,
`TRANSFER_TO_HOST_3D`, `SUBMIT_3D`, `CTX_DESTROY`; `ATTACH_BACKING` checks it
only to clean up the iov. `RESP_OK_NODATA` means "QEMU parsed it": nonzero,
non-duplicate id, valid iov. Only `SET_SCANOUT` (`resource_get_info_ext`) and
`RESOURCE_UNREF` (QEMU-side existence) consult anything.

**So three of my own documents were false in the same sentence.** C-2b's gate
header, `149-warp.md` and (by reference) the status row said the screen's "3D"
word was "the conjunction of four response-checked round trips the host
answered OK — a claim about the host accepting the object". Those four are
exactly the ignored ones. And it was not only prose: `alloc_screen`'s "a 3D
failure is NOT fatal — it falls back to 2D" was dead for a renderer-side
refusal — `is3d` reduced to `comp_ctx`, "3D" printed, and the failure landed
later, silently, as `INVALID_RESOURCE_ID` at the composed `SET_SCANOUT`, whose
result the code dropped after printing "scanout composed" *before* the bind.
The display would have kept the previous scanout, and the C-2b gate would have
said VERIFIED. #240 had measured this exact shape for `SUBMIT_3D` four days
earlier; the finding was filed against one command and never checked against
its family — the same lesson as the C-2d gate pattern that morning, one level
up.

**The repair is #240's own technique**: make the producer prove it with pixels.
`alloc_screen` writes 16 sentinel pixels into the fresh screen's backing,
`TRANSFER_TO_HOST_3D`s them through the compositor context, clobbers the
backing, `TRANSFER_FROM_HOST_3D`s back, compares, restores the zeros. Only a
resource the renderer holds, has attached to `COMPOSITOR_CTX`, and moves pixels
through can pass; a refused create or attach makes both transfers renderer-side
no-ops and the clobber survives. A refusal now falls back to 2D for real, the
screen line says why, the composed line prints after the bind with its verdict,
and `composed-screen.exp` grew a fifth term (the bound resource IS the minted
screen; the verb requires it on both legs).

**Measured on thyla-pi** (KVM, real V3D, boot-ms ~212 000), one variable —
the format the renderer will accept — two runs. *Sabotage*, `VIRGL_FORMAT`
`0x7FFF` in the 3D create: GL leg `screen res 71 2D (1280x800) -- 3D refused:
renderer round trip`, then `scanout composed (1280x800) res 71 bound` — so
`CREATE_3D`, `CTX_ATTACH_RESOURCE` and `ATTACH_BACKING` all came back OK from
the device under a format the renderer cannot accept (the reason would have
named the step otherwise), the renderer refused, the fallback was real and the
display got a working screen; the scenario went RED on the arm and the verb
reported three GATE FAIL terms; the non-GL leg was unaffected. *Clean*: GL leg
**`screen res 71 3D (compositor ctx) (1280x800)`** + `res 71 bound`, non-GL
`2D` + `res 71 bound`, all five terms, rc 0. The half that says the OLD code
would have printed 3D under the sabotage is inferred from the measured OKs and
the old boolean (`comp_ctx && create.is_ok() && attach.is_ok()`), not itself
measured — I chose not to spend a third Pi cycle on a one-line inference and
say so here.

**What this changes downstream**: `CTX_ATTACH_RESOURCE`'s response witnesses
nothing, so C-2c cannot be verified by its attach at all — its gate is P1b's two
arms in-guest (attach + one blit + readback; no-attach control red), which means
C-2c lands WITH the first blit witness. The C-2c design draft
(compositor-side import on host, bounded by hosting, no client verb — every
compositor in the prior art does it that way) is written and waits on that
correction; it goes into GPU-DESIGN as §4.5.10 with the next chunk.

### C-2c — the compositor imports what it composes, and the import is witnessed (after the self-compaction at `8c20b1f8`)

Resumed from the second self-compaction of the run (`8c20b1f8`, all pushed;
the note said "next is C-2c WITH its blit witness", and that is what this is).

**What C-2c is, in one line:** at `alloc_weave` tapestryd now
`CTX_ATTACH_RESOURCE`s every slot resource of a generation into
`COMPOSITOR_CTX`, and at `present-to` it imports the GL adoption's consented
BO — the client handing its buffer to the compositor is the whole grant, no
client verb (§4.5.10) — and every import is revoked BEFORE the resource's
unref on every death path (`release_gen`, `retire`, `wbo_retire`, `present-to
off`/replace, the consented surface's retire).

**The witness, and why it is not the one the design paragraph drew.**
§4.5.4c had already established that `CTX_ATTACH_RESOURCE`'s OK attests
nothing, so C-2c had to land with a pixel witness. The design said "blit a box
of the slot into the screen and read the screen back". Built instead: the
compositor context's own #240 mark/sentinel pair (`warp_probe_build
(COMPOSITOR_CTX)`, minted with the ctx), and per slot: seed tokens into the
slot's host copy through the present path's own `TRANSFER_TO_HOST_2D` (the
guest pixels are borrowed while NO client mapping of the weave exists yet —
`alloc_weave` runs before the Tweft that maps it is answered — then zeroed),
poison the sentinel, `RESOURCE_COPY_REGION` slot → sentinel inside
`COMPOSITOR_CTX`, read the sentinel back. A 1×1 compositor-owned target
instead of the screen: same claim (pixels through the compositor context or
nothing), the direction C-3 will use (the slot as SOURCE), no screen pixels
to save/restore, no question about the screen's coordinates — and it made
import time the natural site, since the reason the design gave for composed
entry ("the screen may not exist yet at import") no longer applied.

**A health copy runs before every witness, and the reason is the latch.** A
copy naming a resource the renderer does not hold in the context reports
`ILLEGAL_RESOURCE`, and vrend then refuses every later command buffer on that
context (§4.5.4a). So a genuinely refused import kills GPU composition for the
process lifetime, silently — which is (a) why `comp_attached` fails closed and
C-3 must never blit from a resource without it, (b) why the mark → sentinel
health copy runs first, so a REFUSED is attributable to THAT import and later
generations read `SKIPPED (compositor ctx unhealthy)` as a measured state, and
(c) why the witness runs at a rare structural moment (~16 controlq round trips
per generation) and never per frame.

**What the Pi taught before it answered the question it was asked** (six
`composed` cycles; the sixth is the one that counts). (1) The clean build read
`REFUSED (slot 0 copy did not land)` on its first run — the witness's own
seed was at guest row 0 and the compositor's copy of a y=0 box on a `Y_0_TOP`
source lands from texel row **h−1** (vrend's FBO copy path measures such boxes
from the bottom; the texel-exact copy-image path was not the one taken). The
instrument needed a control of its own: it now seeds rows 0 and h−1 with
distinct tokens and REPORTS which came back — `witnessed 3/3 (copy read texel
row 799)` — a measured convention C-3's blit boxes inherit rather than a
guess. (2) The posture anchor came out `ttaappeessttrryydd`: the kernel's
`proc: orphan` burst at warden's exit and tapestryd's SYS_PUTS interleaved
BYTE for BYTE — the console TX ring is byte-atomic, not line-atomic, and my
probe mint had moved the anchor into the burst. Not fixed here (LS-8 surface,
aux mid-change in `cons.c`, and it costs the kernel-byte-unchanged property);
the anchor is printed first again, the armed state moved to its own line, the
defect enqueued (`bug_console_tx_ring_byte_atomic.md`) and handed to aux on
yip. (3) The gate script then cost three cycles of its own: a say-line format
change under an anchored regexp; three `-re` arms — pattern ORDER beats buffer
position, so the arm listed first ate a later comp-attach line and discarded
the screen/composed pair before it; and one ordered pattern that matched
PARTIAL lines (serial arrives in chunks) — three GL-leg hangs ending on the
battery's own later FAIL, while an offline replay of the same log passed. The
anchored single-pattern form went green: `WARP-COMPOSED ATTACH: witnessed 2
surfaces (copy read texel rows: 799 797)`, both legs PASS, verb VERIFIED on
seven terms.

**The sabotage measured more than it was asked to.** Skipping the slot
attaches: the first import `REFUSED (slot 0 copy did not land)`, then every
later import `SKIPPED (compositor ctx unhealthy)` — the latch is now a
measurement, not a recollection of vrend — **and the screen's own 3D mint fell
back**: `screen res 73 2D (1280x800) -- 3D refused: renderer round trip`. The
§4.5.4c fallback, built two chunks ago against a hypothetical, ran for real:
the display kept working on the CPU/2D arm while GPU composition was loudly
gone. Verb RED, 2D leg unaffected.

**The quake gate found a C-2d-b leftover.** `glq-virgl.exp`'s eviction leg
waits for `scanout direct N (WxH)`; C-2d-b (`f86177b6`) changed that say line
to `scanout direct N slot S (WxH)` and the check made then enumerated the
`scanout composed` consumers and missed the `scanout direct` ones — five
patterns across `glq-virgl` / `glq-decomp` / `glq-wedge-probe`, all silently
broken since, all failing CLOSED (a false RED on the console-restore leg after
^C, the first time any of them ran after that commit). Fixed to take the
`slot S` token as optional. #230's lesson again: a mirror set is enumerated by
what its members MEAN, not by the substring one happened to grep.

**Gates.** `composed-screen.exp` grew a third claim (GL leg: ≥ 2 per-surface
`witnessed n/n` lines — the battery's two surfaces — none refused; 2D leg: the
import declared skipped, no per-surface line — the control), the `composed`
verb terms six/seven, and `glq-virgl.exp` gates the ctl census (`comp-attach
witnessed W refused R`: R must be 0) after the game dies — the BO import
through the SDL shim's real `present-to`.

**Coordination.** Aux held the mac all afternoon (its pty-4 root-cause fix:
builds + suite + LS-CI + the SMP halves); the C-2c cargo check/build ran at
`-j2` under an explicit yes on yip 0024, everything else waited for the
release; the Pi lease was mine (`hold pi`) for the whole verification.

### C-3 — the compositor composes by blit, and the pixel oracle caught the model on its first probe (`7296bf07`; after the self-compaction at `115cbc5a`)

Resumed from the third self-compaction (`115cbc5a`, everything pushed; the
note said "next is C-3, a large chunk", and it was).

**What C-3 is** (`usr/tapestryd/src/server.rs` + `gpu.rs`; GPU-DESIGN §4.5.11).
Where the host has GL, a Composed present of a software surface no longer
fills the screen on the CPU: it transfers its damage into the presented
slot's own resource (the direct arm's transfer, per slot since C-2d-b) and
composes by `VIRGL_CCMD_BLIT` slot → screen inside `COMPOSITOR_CTX`, then
flushes; a witnessed GL adoption composes by one blit BO → screen — no
readback, no CPU pass, no upload. The blits ride the compositor context's
SYNC slot (`submit_blits`, chunked at the widened `REQ_REGION_LEN`), so a
present is still one dispatch and `ComposeBlit`/`ComposeComplete` close
inside it: the in-flight blit set is empty at every retire point by
construction, exactly the shape stage-0 synchrony gave `intransfer = 0`, and
detach-before-unref (C-2c) stays the whole ordering. The pipelined form
(fenced blits, flush riding fence completion, a real drain) is the C-4+
evolution the spec is cut for; §4.5.11 records why the sync form was chosen
(µs per present against the ~8 MB round trip it deletes; the GL-completion
residual is P2, measured 0/500) and what a FENCE-flagged sync command would
buy if it is ever needed. Chrome stays CPU-painted and uploaded on damage on
both paths — a focus-only repaint now uploads only the frame/strip rects,
because on the GPU path the screen buffer holds chrome and not client pixels
(the whole-buffer push that used to serve focus changes would have blanked
every pane). `Held::Composed` splits into `cpu` (upload + flush at release)
and `gpu` (flush only) regions. The compositor runs its own #240 health copy
once per tick after a GPU-composed present and latches GPU composition OFF,
sticky, with a structural repaint deferred to the next tick (never inline
in the dispatch: the CONFIGURE fan can wedge-retire the surface mid-present).
`res_stale[slot] = !covers_full` on the GPU arm, decided per §4.5.8c rather
than ported. The CPU path is untouched wherever the GPU one does not apply.

**The screen is `Y_0_TOP` now, and C-2b's flags-0 screen was displaying
inverted.** Every 2D resource QEMU creates carries `Y_0_TOP` and is flipped at
scanout (Linux fbcon upright under egl-headless); a flags-0 resource is shown
unflipped (Weston upright). C-2b minted the 3D screen flags 0 and filled it
top-down from the CPU — inverted on a GL display, from the day it landed, and
nothing could see it (#195, and a gate that read a say line). Named in
§4.5.11 as the defect it was; the display half stays an anchor, since the
oracle reads the resource, not the display.

**Conventions are measured, and the measurement was wrong once — the oracle
caught it on the first probe.** A blit box is a request in the renderer's
coordinates; C-2c had measured that a copy box on a `Y_0_TOP` source counts
from the bottom here. So C-3 measures at bring-up, on throwaway contexts
(`CONV_PROBE_CTX_BASE`+, one fresh per attempt — a refused request latches
its context, and the probe tries requests whose acceptance is the question),
with seeded 1×4/1×16 probes of each kind. The first probe measured ONE
request — unscaled, 1×2 → 1×2 — derived flips (both sides), confirmed them
(unscaled again), and applied them to every blit. The battery's panes are
both SCALED (A 1280×800 → 638×398, B 640×400 → 636×398 — the 1-px frame inset
makes every "matching" pane the scaled class), and virglrenderer routes an
unscaled same-format nearest RGBA blit to the texel-exact copy-image path
and a scaled one to `glBlitFramebuffer`, which hold OPPOSITE conventions for
a `Y_0_TOP` pair whose transfers invert rows: copy-image wants both boxes
flipped, blit applies the flip itself and wants the raw boxes. Run 1: the
panes composed vertically swapped; the first `probe-screen` read `(960,200) =
#0000ff` for A's red — `LS-CI FAIL` — while the probe's own confirmation had
read CONFIRMED. The measurement of the renderer was right about the class it
measured; the measurement of the SYSTEM (the battery at real geometry + the
oracle) is what caught it. Redesigned per (source shape: `Y_0_TOP` slot /
flags-0 BO) × (size class: unscaled / scaled ×2), request variants tried in
order (plain, negative source height, negative destination height) until the
landing has the ORDER the shape needs (slot straight; BO mirrored — its GL
row H−1 is its visual top), flips read off WHERE it landed and WHICH rows it
carried, each CONFIRMED at an asymmetric offset, each fail-closed per class,
every landing SAID as a 16-character row map. Run 2 on V3D: `slot U plain
sf1 df1, S plain sf0 df0; bo U plain sf0 df1, S src-neg sf0 df0` — the plain
scaled BO request landed straight (`.0011…`), the negative-source-height
idiom mirrors it — all four CONFIRMED, then 9/9 pixel probes exact. The
compose path picks the class by the op's own box sizes (the renderer's
predicate) and issues through the same builder the probe used. Lesson filed
(`memory/bug_c3_convention_per_request_class.md`): a convention measured on
one request class is not a convention; two recollections of vrend/QEMU's flip
code were wrong in opposite directions this arc, and the measurements were
right both times.

**The oracle.** `probe-screen X Y` (tapestry global ctl; test-mode, ungated
like the determinism verbs, rate-limited) makes the compositor read texel
(X,Y) of the SCREEN back and say it — `via readback` (TRANSFER_FROM_HOST_3D
through the compositor ctx, the only place a GPU-composed pixel exists) on
the 3D screen, `via backing` on the 2D one, with the scanout mode and the
`composed gpu G cpu C` census. The battery probes its own sample points at
every pixel stage and grew `multirect-v` (B split TOP/BOTTOM green over yellow
— the vertical asymmetry a mirrored or displaced box cannot fake, which a
solid fill and a left/right split never show) and `tab-cycled ready` (A
hidden by the tab, revealed by the cycle, presented red, probed — the C-2d
redraw contract on the composed path). `composed-screen.exp` claim 4 + verb
terms eight/nine: 9/9 exact `via readback` with `gpu ≥ 1` on the GL leg (a
build whose GPU path silently routed everything to the CPU one composes
CORRECT pixels; only the census tells that apart), 9/9 exact `via backing`
with `gpu 0` on the non-GL leg — the same coordinates and colours on both,
the first pixel witness that the two composition paths agree from outside.

**Measured (thyla-pi, KVM, V3D).** Run 3, the final binary, both legs:
`WARP-COMPOSED PIXELS: 9 probes via readback ok (composed gpu 34 cpu 0)` /
`… via backing ok (composed gpu 0 cpu 27)`, `C-2b/C-2c/C-3 COMPOSED-SCREEN
GATE: VERIFIED` (nine terms). Sabotages, GL leg: **S1** — the blit never
submitted, every other GPU-path step intact — `screen-probe (960,200) =
#101014` (the pane background) with `composed gpu 10`, RED on the first
probe; **S2** — every present routed to the CPU path — all nine pixels exact
`via readback` (so the CPU upload into the 3D screen composes right as well)
but `composed gpu 0 cpu 31`, RED on the census term, which is exactly the
sabotage the census exists for. Run 1 stands as the third: the natural
convention error, RED at the first pixel. Then `quake` and `decomp gl` on the
final binary — the standing GL gates and the only driver of the BO composed
arm: `quake` `WARP-4 GATE: VERIFIED` (969 frames, 44.9 fps; `comp-attach ctx 1
bo 1 res 82 -> surface 1: witnessed`, and — the BO arm's first live execution
— `surface 1 composed via GPU blit (BO res 82 -> screen res 76)` in the
Composed window before the direct switch); `decomp gl`: composed **36.9 fps
(969 frames, 26.3 s)** against the **25.4 fps (38.1 s)** measured 2026-08-10
on the same host and demo — the direct arm reads the identical 44.4 fps both
days, so the arms are comparable — the composed present's cost fell from
16.8 ms to 4.6 ms per frame (39.3 → 27.1 ms/frame), the windowed-GL overhead
from 1.75× to 1.20×. What is left in the 4.6 ms is the C-4 question (the blit
+ flush round trips, the per-tick health copy, the display readback under
egl-headless), to be decomposed rather than guessed.

### C-4 — the residual decomposed, and it was neither of the two things named first (after the self-compaction at `d591c35e`)

Resumed from the third self-compaction of the day; the note said "next is
C-4: decompose the remaining 4.6 ms, retire the readback where GL exists, the
fenced form if the sync round trips are what is left." Read §4.5.11 + §4.5.9 +
149-warp's #196/#215 decomposition first, as the note demanded, then built
the instrument before touching the mechanism.

**The instrument** (`Cost` in `server.rs`): every synchronous device step of
the present path timed where it is issued, every present dispatch timed
whole and attributed to its arm, cumulative `cost <kind> <n> <sum_us>
<max_us>` lines in the tapestry ctl; `glq-decomp.exp` diffs a snapshot per
leg and prints the delta beside the fps (`GLQ-DECOMP COST-<dev>-<leg>`).
Cheap — `Instant::now()` twice per step — and it answered on the first run.

**Finding 1 — the figure was mostly the instrument's.** egl-headless, C-3 as
landed: composed present **20.7 ms = blit 1.44 + health 8.34 + flush 11.12**;
direct present **17.0 ms = its flush**. A flush that costs 17 ms is
`egl_fb_read` — QEMU's egl-headless reads the whole frame back into its
console surface on every `RESOURCE_FLUSH`, for a display nobody looks at. Both
arms inherited it. So `run-vm.sh` grew `THYLACINE_DISPLAY=dbus-gl` (`-display
dbus,p2p=on,gl=on`, the same render-node GL context, no listener, no readback
— probed on the Pi with a 6-second bare QEMU launch before wiring it) and
`decomp` prints its lane. Under it the direct present is 2.7 ms and the direct
frame 8.8 ms (113 fps against egl-headless's 44.8) — the same guest, the same
GPU, one variable changed. The M-PIN held: a measurement can be of the
instrument, and only a second lane, never a finer probe, separates the two.

**Finding 2 — the residual was the health verify, not the round trips.**
dbus-gl, C-3 as landed: composed **62.8** vs direct **113.2** fps; composed
present **9.62 = blit 1.63 + health 8.92 + flush 0.12**. `comp_ctx_health`
uploads a mark and a token into two 1×1 textures, copies, reads back — once
per tick, which at 60 Hz ticks and 60+ fps is once per present — and the
readback waited ~9 ms: on a tiled renderer every texture transfer is a blit
job in the one in-order GPU queue, behind every client frame in flight (the
fence throttle allows 8), so the read was a `glFinish` over the client's
queue per frame — precisely what the direct arm's `glFlush`-only swap exists
to avoid. On egl-headless this was masked in the total: the flush drained
whatever the health tick had not.

**The first fix was half a fix, and the census said so.** Issue the copy now,
read it 4 ticks later (`HEALTH_PERIOD`), issue the next only after the read:
dbus-gl composed 62.8 → 84.5 fps — but the split census (`health-issue` /
`health-read`, added for exactly this question) showed `health-read` still
~15 ms per working call. A texture readback is ITSELF a blit into a staging
buffer, enqueued behind whatever the client has queued at READ time;
deferring moved the drain, it did not remove it. **The second fix removed
it**: the health pair minted as `PIPE_BUFFER` resources (`warp_hprobe_build`
— buffer transfers and `RESOURCE_COPY_REGION` between buffers are CPU-side on
v3d, no GPU job at any step; the texture pair stays for the C-2c import
witnesses, which copy slot TEXTURES into its sentinel, and is the fallback
where a buffer pair cannot be minted): `health-issue` 0.43 + `health-read`
0.19 ms per period → 0.17 ms per present; dbus-gl composed **92.8 fps vs
direct 113.0 — 1.22×, 1.9 ms/frame** (from 1.8× / 7.1 ms), composed present
**3.18 ms** vs direct 2.67. What is left is ~0.5 ms server-side (the blit's
own issue) and ~1.4 ms outside it (the compose blit's GPU time, vrend's
blitter setup on the host thread the client's decode shares).

**Finding 3 — the "blit" and "flush-direct" numbers are mostly the FIFO.**
The direct arm's 2.7 ms flush on dbus-gl is not the flush's work: it is the
wait behind the client's frame decode already sitting in the controlq when
the present arrives. The composed blit pays the same wait (1.3–3 ms). Which
is why the fenced pipelined form — the thing §4.5.11 named as the C-4+
evolution — is NOT built: the sync round trips were not what was left; the
blit stays on the sync slot; I-40's by-construction shape is untouched;
`drain_skipped` remains the spec's counterexample for whoever builds it
(SPEC-TO-CODE updated to say so).

**egl-headless after all this: 37.5 vs 44.4 fps, unchanged — the correct
result.** Health fell to 0.19 ms per call and the flush rose 11.1 → 18.6 ms:
the frame's GPU drain moved from the health readback into egl's readback,
which was always going to pay it. The 4.2 ms remaining on that lane are the
backend's. Every figure now names its lane, and the arc quotes dbus-gl.

**Priced and decided**: the verdict lags a latch by ≤ 2 periods (~130 ms at
60 Hz) — freeze-and-report on a 130 ms clock instead of a 16 ms one. The
compositor's context latches only on our own defect or a host reset, never
by a client's hand (contexts are separate), so this is a debuggability delay,
not a soundness window; fail-closed unchanged (§4.5.12).

**The self-audit added a control, and the Pi re-ran.** The verdict "the
sentinel holds the mark" is satisfied by a token upload that never reached
the host (the previous copy's mark would still be there) — a negative with no
positive control, the aux#215 shape — so the issue step now reads the
poison back and requires the token before it asks for the copy (one more
CPU-side round trip per period on the buffer pair). Re-verified on the
final binary (ramfs `207d2039…`): dbus-gl **93.1 vs 112.7 fps**, health 0.21
ms/present (issue 0.58 + read 0.20 per period); egl-headless 37.6 vs 44.8.

**Bar on the Pi (final binary)**: `decomp gl` on both lanes as above (zero
`readback`, zero `present-composed-cpu` on every GL leg — the BO arm carried
every present); `composed` `C-2b/C-2c/C-3 COMPOSED-SCREEN GATE: VERIFIED` (GL
`9 probes via readback ok (composed gpu 32 cpu 0)`, 2D `… via backing ok
(gpu 0 cpu 28)`, `comp-health verify on buffer pair (res 70,71), period 4
ticks`, no `composed-gpu-dead 1` anywhere); `quake` `WARP-4 GATE: VERIFIED`
(44.4 fps, `comp-attach witnessed 5`). Also found: GPU-DESIGN §4.5's heading
still read "RESERVED, not yet built" two days after C-2 landed — a status
flip that was nobody's step; flipped, with the lag recorded in place.

### The operator lifted the agent gate, and two owed rounds ran the same hour

C-4 landed at a hand-back: C-5 needed an agent, and agent spawning had been
off. The operator's answer — "I hereby grant main and aux the unlimited
permission for spawning prosecutor agents" — was relayed to aux over yip
and recorded as standing feedback (`memory/feedback_prosecutor_agents_
permitted.md`), and two rounds were spawned at once on `holotype-reviewer`.

**C-5 (the Warp-C round, C-2a..C-4, I-40 + I-45): 0 P0 / 0 P1 / 1 P2 / 2 P3,
plus one self-audit P3, not dirty, all fixed.** The P2 was a sentence of
§4.5.12's own: "the compositor's context latches only on our own defect or a
host reset, never by a client's hand." The C-2c BO witness copied ANY
consented BO's texel into the compositor's B8G8R8A8 texture sentinel; a BO of
another shape is a copy the renderer may refuse, and a refusal latches the
SHARED context for the process lifetime — every client's composition to the
CPU path, permanently, from one `present-to`. Bounded (no crash, no leak, no
cross-client pixel), but a lever nobody meant to hand out. Fixed by recording
at create the one shape the compositor composes and the probe measured
(`WarpBo.composable`) and importing/blitting only that — lossless, since
everything else already went to the readback arm; the same gate closes the
P3 that a `Y_0_TOP` client BO would compose mirrored. The other P3: a
`res_stale` flag left stale on a failed-blit return. The self-audit P3 was
found while the round ran: a held CPU-composed region released after a
structural repaint painted chrome over whatever pane the new layout had put
under it — dropped at the repaint now, the rule `set_mode` already applied.
Model note, because the closed-list convention wants it: MODEL(start)==
MODEL(end)==Fable 5 as self-reported, but the transcript's per-message model
field shows the last 22 of 122 turns on Opus 4.8 — the read was Fable, the
synthesis partly Opus. Recorded; the findings were re-derived before fixing.

**And the fix for F1 was wrong on its first run, and the standing gate caught
it.** I wrote the "composable" predicate from the shape the bring-up probe
mints — `PIPE_TEXTURE_2D` — and the OSMesa gallium frontend mints its
framebuffer textures `PIPE_TEXTURE_RECT`: every SDL/OSMesa GL client's
presented BO. `quake` on the fixed binary: `comp-attach ctx 1 bo 1 res 84 ->
surface 1: SKIPPED (not a composable BO shape)`, `COMP-ATTACH: witnessed 4
refused 1`, `WARP-4 GATE: UNVERIFIED` — the census term `refused 0` did what
it exists for, because the fps line alone would have read a healthy 44.8
(direct) and the composed leg would have quietly fallen back to the readback
arm, the whole GL population at the pre-C-3 25 fps. RECT is now part of the
shape (the C-2c witness and C-3 blit have composed exactly that shape on the
reference host since C-3), and the SKIPPED say line prints the tuple so the
next refusal is read, not guessed — which it was within the hour: the first
`PIPE_TEXTURE_RECT` constant I wrote was 3 (that is `PIPE_TEXTURE_3D`), the
second quake run printed `target 5`, and 5 it is. Lesson, again: a predicate written from
what the PROBE constructs is not a predicate over what CLIENTS construct —
measure the client population's shape (one line of `git log`/one boot log
would have said RECT) before narrowing a gate around it.

**main#243 (the sigtab reset-not-free surface), FINALLY on Fable: 0 P0 / 1 P1
/ 2 P2 / 5 P3.** Round 1 had been Opus-on-Opus. Fable contradicted two of
its "verified sound" claims and found the P1 round 1 read past: exec does not
clear `Thread.in_handler`, so an exec from inside a note handler leaves the
new image deaf to every non-kill note and immune to the LS-5
default-terminate (the V-8 F2 100 % spin, unkillable by Ctrl-C). Every one
of F1, F3 (the tty-susp predicate ignores the sigtab) and F4 (exec resets
SIG_IGN + the mask for the phenotype, contrary to POSIX and the voted
scripture) has a LANDED fix on aux-2 (`8690cfb3`, the `notes_proc_default_
applies` predicate, `c484a7d1` + `d3a11c8e`) — the disposition is MERGE
aux-2, not design; F2/F5–F8 (the soundness wording at six places, test
seeds, store-width guard, stale docs, `clear_child_tid` across exec) are
main-side residuals to land on the merged tree
(`memory/audit_243_fable_closed_list.md`). Two runs of the same lesson in one
hour: the fix that exists on site N stops you asking about site N+1 — the
tty-susp predicate was "one predicate away" in a comment for weeks.

### Still open leaving this run

- **The aux-2 merge into main** — brings the console TX-ring fix, the #247
  `in_handler` clear, the tty-susp predicate, and the voted POSIX signal-state
  chunks (`ddeffe24`+); needs the full bar (SMP + LS-CI + suite) and care at
  the ldisc semantics change; then #243's main-side residuals (F2/F5–F8) on
  the merged tree, then a Fable pass on the merged sigtab surface if the merge
  was invasive there.
- **The C-0d Fable re-prosecution** (the #240 client-ctx detector in
  `server.rs`; rounds 1+2 were Opus) — spawn on the C-5-closed tree.
- **C-4's named residuals**: ~1.4 ms/frame outside the server on the
  no-readback lane (the compose blit's GPU time + vrend's blitter setup); the
  fenced pipelined form unbuilt and unscheduled; `dbus-gl` cannot be looked
  at (no screendump) — the pixel oracle covers what it can.
- **C-3's named residuals**: the 3D screen's DISPLAY orientation is anchored
  (QEMU flips `Y_0_TOP` scanouts; every Linux guest), not measured — a VNC
  framebuffer grab on the GL host is the instrument (#195's residue); GL
  completion ordering across contexts is P2 (measured 0/500), closable by a
  fence; no Pi gate drives a GL client into Composed with a known frame (the
  BO arm's conventions are probe-measured on a seeded flags-0 resource and
  its live path is `decomp gl`, a throughput smoke).
- **The console TX ring is byte-atomic** (`bug_console_tx_ring_byte_atomic.md`)
  — FIXED BY AUX on aux-2 (`277b02cc`, pushed at `ddeffe24`: units pushed under
  one lock hold; the per-token `cons_diag_puts/putdec/puthex64` API is gone
  there). Reaches main at the aux-2 merge above.
- **Two thirds of the extinction tear** (the vault seam, `IPI_HALT`), and a
  prosecutor round owed on the landed third.
- **`main#228`** — Fable rounds on C-0d and #243, quota-blocked. Deliberately
  *not* run on an Opus fallback: what is owed there is lineage independence, and
  a fallback round would spend the surface without buying it.
- **`docs/REFERENCE.md`'s snapshot block** — dead since Phase 5 (above). Needs a
  decision about what it is for, not a patch.
