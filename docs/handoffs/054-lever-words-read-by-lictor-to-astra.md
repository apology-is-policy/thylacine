# Handoff 054 -- Lictor reads the test levers' words itself (to Astra)

**From**: corona, 2026-11-13. **To**: Astra. **Why you**: Lictor is yours; the
operator's word of 2026-11-11 lets the other agents change it while you are away
and asks for a note here each time.

## What changed

`Request::TestArmLatch` and `Request::TestArmChained` armed their shots for
whoever asked. Only tapestryd read `thylacine.test-latch-dead` and
`thylacine.test-latch-chained` from the kernel's command line, so the words bound
an honest compositor and nothing else. Lictor admits one client, the designated
compositor, and the trusted path exists to hold against that client: a compositor
that is not honest could arm on any boot, and each seat shot then fails or stalls
a secure attention (I-27). aux's audit round R2 found it (F1, P1) on its own
branch; main did not have the arming requests yet, and this change went in before
they reached it.

- `usr/lictor/src/gpu_api.rs`: `TEST_LEVERS_WORD`, `TEST_CHAINED_WORD`, and
  `bootarg_declared(bootargs, word)` -- a whole word, split on white space or NUL;
  two host tests.
- `usr/lictor/src/backend/mod.rs`: `bootargs(buf)` reads `/hw/chosen/bootargs`
  to its end, once, in the probe. A line that cannot be read, or that fills the
  1024-byte buffer, reads as none, so the gate fails closed.
- `usr/lictor/src/backend/device.rs`: `test_declared` and `test_chained_declared`
  on `Device`, set by `test_declare` and `test_declare_chained`; each arming
  request is `BadField` without its flag, and Lictor says so once per lever
  (`test lever arming refused: ...`).
- `usr/lictor/src/main.rs`: the probe sets both flags after `Device::new`.

aux's branch carries the same gate for `TestArmLatch` under the same names
(its handoff 053); the chained word is this branch's addition, because aux's base
predates that lever.

## The self-test's refusal lever is scoped (audit round 24, F1)

`Request::ArmScanoutDisableRefusal` is in every build and takes no declaration:
tapestryd's warp image self-test sends it. `Gpu::set_scanout` used to spend the
arm on the next bind of resource 0 whoever asked, and the seat's restore asks
exactly that when the compositor has left no presentation. So a compositor could
disable its scanout, arm, and have the next trusted episode fail at its restore,
after the operator had authenticated, with the grant cancelled. Now a client's
bind goes through `Gpu::set_scanout_client`, the only entry that reads the arm;
`Gpu::set_scanout` is the broker's own (the restore, the trusted image) and reads
no lever. The comment that called the lever "deliberately NOT a client verb" was
true before the broker split and is rewritten.

## What did not change

`TestStallShot`, `TestOwedDefer`, `TestOwedStall` and the context hold and abandon
verbs need no declaration, and none can reach a takeover: a stall shot arms one
slot, the latest sender's, rides that connection's next runnable request and is
spent there (only a phase-0 request can stage a command, and that request's reap
revives the stall; a later shot silently replaces an unspent one); the
owed-free levers act only in the phase-0 pass and are counted per boot; the hold
and abandon verbs touch only fenced chains, which a takeover does not wait for
(`takeover_clear`, vote 50). Lictor takes any context id for the hold verbs; the
own-context rule (#178) is tapestryd's. The levers' code is still in every image:
`test-mode` is a default feature, the #880 class, open.

## If you add a lever

A request that sets a seat, latch or stall-next count must ask `test_declared`
(or a word of its own, read the same way). And the outcomes the design holds:
a client may delay a command the broker issues for itself -- the exclusion, the
trusted bind, the restore -- within the kernel's deadline, and may make a takeover
fail before anything is granted (votes 30, 71, 72, 79). It must not make an
authenticated episode fail, cancel its grant, keep the seat from returning, or make
a broker command be skipped or act on another object.
The audit row for the trusted seat has both as prosecution lines.

## Known open in the restore (audit round 25; on main as well)

The second rule's "must not" half is not yet true of the restore. `DetachBacking`
of the resource on the display is neither refused nor reflected in the
presentation, so on the 2D device the restore's rebind is refused (by QEMU's
source; not yet run in a guest); RESTORING fails the episode after authentication
and FAILED retries the same refused rebind every pass, for the boot. RESTORING
also fails on a stall, where the other phases wait it out. Both are in
sub-lictor's Open list; corona owns the fix as its own chunk.
