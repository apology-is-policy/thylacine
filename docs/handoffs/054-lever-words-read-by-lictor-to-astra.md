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
  once. A line that cannot be read, or that fills the 1024-byte buffer, reads as
  none, so the gate fails closed.
- `usr/lictor/src/backend/device.rs`: `test_declared` and `test_chained_declared`
  on `Device`, set by `test_declare` and `test_declare_chained`; each arming
  request is `BadField` without its flag.
- `usr/lictor/src/main.rs`: the probe sets both flags after `Device::new`.

aux's branch carries the same gate for `TestArmLatch` under the same names
(its handoff 053); the chained word is this branch's addition, because aux's base
predates that lever.

## What did not change

`TestStallShot`, `TestOwedDefer`, `TestOwedStall` and the context hold and abandon
verbs need no declaration. They are bounded instead (#178), and a held or poisoned
chain no longer holds a takeover up (`takeover_clear`, vote 50). The levers' code
is still in every image: `test-mode` is a default feature, the #880 class, open.

## If you add a lever

A request that sets a seat, latch or stall-next count must ask `test_declared`
(or a word of its own, read the same way). The audit row for the trusted seat has
that as a prosecution line.
