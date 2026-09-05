---
id: chg-2026-08-02-stratum-sweep
type: chg
title: "vault sweep: the Stratum integration surface"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-stratum-bdev
  - sub-stratum-boot
  - sub-stratum-server
  - sub-stratum-session
established: []
closed: []
opened:
  - seam-kobj-handle-release
  - seam-proxy-coord-eof
  - seam-stratum-notify-peercred
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 12. The last empty area in the spine. Read from code on both sides of
the boundary: `bdev_thylacine.c` in full, the stratumd daemon
(peer_creds / proxy_9p / run / the listen helper), the 9P server's
`h_attach` + `h_walkgetattr` + Twalk fast path, joey's bringup and pivot,
and login's DEK lifecycle + home bind. Four dossiers under
`system/stratum/`; address-space still deferred behind the unmerged L-1
`AddrSpace` extraction.

THE ORGANIZING FACT is that neither side trusts the other with what it
holds. Stratum enforces DATASET SCOPE ONLY -- per-file rwx is the kernel's
job at its own chokepoint -- and `login` never holds a raw DEK, forwarding
an opaque corvus token so the coordinator does the unwrap. Each side holds
exactly what the other must not, which leaves IDENTITY as the one thing
that must cross, and it crosses by exactly one channel.

THE F-4 CORRECTION, CONFIRMED FROM BOTH SIDES. Scripture once specified 9P's
`n_uname` as that channel. Stratum's `h_attach` reads it and DISCARDS it --
"we already have peer-creds" -- and ignores `afid` too. The channel that
works is `SO_PEERCRED`, which pouch marshals onto `SYS_SRV_PEER`
underneath, filling `ucred.uid` with `principal_id` since A-3. Stratum's own
source carries the warning back across the boundary in as many words: this
is LOAD-BEARING, do not "simplify" the marshal back toward 0, because the
/ctl SYSTEM gate keys on that uid. Thylacine still forwards `n_uname`
correctly, but as the foreign-server path; at v1.0 nothing consumes it.

THREE WORKED FAILURES on the block backend, each a different shape:

  1. The partial-sector write ROUNDED UP AND ZERO-PADDED. Stratum's extent
     layer writes 4096+32 (plaintext + AEAD tag), never a sector multiple,
     and packs an adjacent object into the same sector -- so the pad
     destroyed a NEIGHBOUR's bytes and surfaced as STM_EBADTAG on an
     unrelated later read. RMW is mandatory on a device that cannot write
     sub-sector; the posix backend never had this because pwrite is
     byte-granular.
  2. The failure latch was UNREACHABLE BY DESIGN. It latched permanently on
     the stated assumption that "Stratum tears down + re-opens" -- and no
     caller ever drove that, so one transient virtio hiccup killed the FS
     for the life of the process. Now a bounded in-place reinit (full VIRTIO
     reset resyncs both ends to idx 0) recovers first; only an exhausted
     budget latches.
  3. Durability SILENTLY DEPENDED ON THE LAUNCH CACHE MODE until
     VIRTIO_BLK_F_FLUSH was negotiated. The commit's write-then-fsync
     barriers are now real on-device regardless of how QEMU was invoked.

READINESS IS AN EVENT, NEVER A DURATION -- the boot half's central fact.
joey once polled /srv on a fixed retry budget, which was a GUESS at how long
a crypto-heavy mount takes: fine under HVF, and under TCG it raced a HEALTHY
stratumd and declared the boot fatal mid-mount. It now blocks until stratumd
emits `bound and ready`. The token choice is the sharp part: stratumd also
prints a `serving ...` line, and that one is OPTIMISTIC -- emitted BEFORE
the mount and bind -- so keying on it would reproduce the original bug with
extra steps. This is haz-harness-fail-open's rule stated positively: verify
the artifact, not the intent. The optimistic line reports intent.

`--single-session` IS NOT AN OPTIMISATION; it is the only teardown
available. login runs as PRINCIPAL_SYSTEM and the kill axes are owner /
CAP_HOSTOWNER / CAP_KILL -- NONE of which it holds against a user-owned
Proc. With no ambient root to fall back on, serve-one-and-exit is the
mechanism rather than a preference. A pleasing consequence of the
capability model showing up as a lifecycle design.

NEW HAZARD: [[haz-unread-pipe-wedge]] -- a reader that stops reading wedges
a writer that must not block. Two independent instances in two layers:
stratumd's stdout (#370; joey read only until readiness, a full pipe blocked
an FS thread mid-write and wedged the whole system -- proven live) and the
LS-CI serial relay (#78; blocking stdout stopped the relay draining QEMU,
whose buffer filled, whose guest UART ring filled, so the GUEST dropped the
token the test was waiting for). Both are diagnostic paths that became
control paths; both are provable with a paused reader and no VM.

CORRECTING BATCH 11, twice, both the same class. The first `abi` note was
filed in a top-level `abis/` directory that THE SCHEMA DOES NOT DECLARE --
its layout block puts ABI notes under `system/boundary/registries/`, which
has existed empty since the commit-0 scaffold. And this batch's note claimed
that directory was "the registry the schema declares and `workflow.md`
names": `workflow.md` does not mention it at all. Both errors are writing
from a memory of the schema instead of from the schema -- the exact mistake
recorded as the gate-smp lesson IN THE SAME COMMIT. Recording a lesson does
not inoculate against it. `abi-boot-banner` is moved (its id-based
wikilinks resolve unchanged); the Record plane keeps the false claim visible
above this correction, which is what an append-only record is for.

AND THE SAME ERROR AT 28x SCALE, found by pulling that thread. The seam
registry was SPLIT across two directories: 48 notes in the schema-declared
top-level `vault/seams/` and 28 in an undeclared `vault/system/seams/`,
which nothing in `meta/` or `home.md` mentions. Invisible to lint (it keys
on the id prefix) and invisible through wikilinks (id-based), so it drifted
for six batches -- and this batch extended it by three more, because I
followed the precedent instead of the schema. All 76 consolidated into
`vault/seams/`; no link changed. The lesson generalizes past both instances:
an id-keyed linter cannot see a filesystem-layout error, so LAYOUT IS
EXACTLY THE CLASS THAT MUST BE READ FROM THE SCHEMA RATHER THAN COPIED FROM
A NEIGHBOUR.

The move is NOT recorded as a `touched` edge, and the linter is why. R6
fires on any chg touching an `abi`: it demands `mirrors-checked` cover that
surface's mirrors. This batch verified none of the boot banner's three
consumers -- the note only changed directories -- so claiming the edge would
have meant either a stale-mirror FAIL or, worse, listing a check that was
never run. First real exercise of that rule (abi-boot-banner is the corpus's
only ABI note), and it earned its keep by refusing an assertion the system
could satisfy while broken.

Recorded seams: [[seam-kobj-handle-release]] (no syscall releases a hardware
kobj handle; safe only under the single-bdev process model, and the
narrowing is invisible at the call site), [[seam-proxy-coord-eof]] (a
coordinator closing mid-request surfaces as a mount failure rather than a
per-op EIO), [[seam-stratum-notify-peercred]] (the notify socket is trusted
by path, not by peer -- small today because a forged frame buys only a
teardown, and it should be closed BEFORE the parser learns the rotate/evict
verbs).
