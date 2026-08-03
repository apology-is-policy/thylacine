---
id: chg-2026-08-03-libdriver-discovery-sweep
type: chg
title: "libdriver's discovery half — a chain of distrust, and a test that cannot see its own property"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-libdriver-discovery
  - moc-userspace-runtime
  - inv-i34
established:
  - sub-libdriver-discovery
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 47: libdriver's discovery + lifecycle half — source, dtb, supervise,
readyline. 4 files, 1630 lines. The second slice of 57d, completing libdriver.

**THE ORGANIZING FACT IS A CHAIN OF DISTRUST, AND EVERY LINK WAS TRACED RATHER
THAN INHERITED.** Identity flows *up* from a source that may be lying; resources
flow *down* from a view that cannot be; and the driver believes neither until it
reads the device register. The middle link is the scripture's own rule — the
warden binds on the identity, never the transport, and never reads a device
register itself — and it is enforced, not merely asserted: a bus source that must
poke hardware runs as a **separate sandboxed Proc** and pipes its findings back,
and the supervisor rebuilds every resource from its own trusted view before using
them. This is live, not hypothetical: the virtio-mmio bus source is a real
program that claims the bank, reads each slot's DeviceID, and reports typed
records.

**THE THREE QUESTIONS THE HANDOFF CARRIED ALL RESOLVED AFFIRMATIVELY, WHICH IS
WORTH RECORDING BECAUSE TWO OF THEM WERE HYPOTHESES OF A DEFECT.**

- **Does the bind matcher pick the manifest the grant assumes?** Yes, by
  construction: `best_match` and `resolve` walk the node's ids most-specific-first
  with the *same* predicate, so the identity recorded in the grant is exactly the
  one that won the bind. There is no path where the warden binds on one identity
  and grants under another.
- **Is the node-record codec as strict as the descriptor codec?** Yes — and the
  split between its two ends turns out to be principled rather than incidental.
  The **encoder** rejects what would produce a valid-but-*wrong* record: a
  delimiter that would silently re-frame the line, and a bus function the format
  cannot represent at all (rejected **loudly**, rather than dropped to degrade
  downstream into a grant with no PCI axis). The **decoder** rejects what can be
  cleanly refused: unknown version, unknown or duplicate key, bad number, and
  every count over its cap. The one deliberately lenient parser is the PCI
  topology reader, whose reporter is the kernel, and it carries an explicit
  contract naming the caller it is valid for.
- **Is the readiness bound sound against a driver that never sends a newline?**
  Yes, and its history is the better artifact — see below.

**AND ONE HYPOTHESIS OF A HOLE WAS WRONG, WHICH IS THE RIGHT OUTCOME FOR HAVING
CHECKED.** `reconcile_reported_node` deliberately permits one thing: a hostile
source may **mis-identify** a real slot. Its safety argument defers to "the
driver's own device re-validation". Verifying that, seven standalone virtio
programs do re-read the DeviceID — but those *scan* all 32 slots for their own
id, predate the framework, and never take a grant. The one program that does take
a grant, the Menagerie-bound net driver, shows no such check and its header
explicitly says it brings the device up *entirely from the grant*, with no
hardcoded base. That looked exactly like the shape where a fix at seven sites
stops you asking about the eighth.

It is not. The check lives one crate further down, in the netdev library's
`open_slot`, which re-reads magic, device-id and version before driving anything,
with the principle stated better than this note could: **the grant is
information; the device registers are ground truth.** Reading only the driver
would have produced a false finding; the containment chain is complete, and its
residual is an *availability* failure (the wrong driver refuses, the right one
was never bound) rather than an authority one.

**F1 -- THE BIND MATCHER'S ONLY REASON TO EXIST IS UNTESTED (task #138).**
`best_match` is more than a linear scan for exactly one purpose: the node's
**most-specific** identity must win over database order. It tracks a
(manifest, id-position) pair and keeps the earliest position for that reason
alone.

All three of its tests are satisfied by an implementation that returns the first
manifest matching *any* id. Two use single-identity nodes, so the ordering never
engages at all. The third is *named* for the property —
`best_match_most_specific_id_wins` — and arranges the database so that order and
specificity point at the same answer, which is precisely the arrangement in which
the property cannot be observed. Substituting the broken body changes no
assertion. A non-vacuous test needs the database reversed relative to
specificity.

Reachable whenever two manifests bind different compatibles of one node, which
DTB nodes routinely carry (`["arm,pl061", "arm,primecell"]`); not live at five
compiled-in manifests. This is the assertion-satisfiable-by-a-broken-system shape
applied to the *evidence* rather than to a claim: the code is right, and nothing
would notice if it stopped being.

**F2 -- THE CRATE'S FRONT DOOR DESCRIBES AN EARLIER CRATE (task #139).** The
header presents a two-part split — one pure module pair, one libthyla-rs layer —
for a crate that has six modules, and its claim that `driver` is "the only
libthyla-rs layer" is **false**: `source` holds two concrete sources behind the
same feature flag, both importing libthyla-rs. Every *module* header is current
and cross-references its siblings correctly; only the crate header is frozen at
the point where the crate had three modules. Same family as the libutopia and
parley front doors.

Worth noting against myself: this is the sentence I used to **cut the previous
slice**. It was accurate about what it named, and named half the crate.

**THE COUNTERWEIGHTS ARE A BOUND ON THE WRONG LOOP, AND A DISTINCTION BETWEEN TWO
KINDS OF FAILURE.** The readiness accumulator's header records a fix worth
carrying past this crate: the original read the pipe **one byte at a time with
blocking reads**, and the give-up budget lived in the *outer* poll loop — so a
driver that wrote a partial line and then simply held, alive with its write end
open and nothing more to say, stalled the supervisor **forever** on the next
byte's blocking read, escaping the budget entirely. A hang there is a boot
denial-of-service by a misbehaving driver, which is the exact threat the
framework exists to contain. The fix moves the blocking out of the loop the
budget does not cover: one bounded read of whatever is *available*, fed into an
accumulator that persists across polls. **A bound only holds on the loop it is
written on.**

The supervisor's counterweight is a distinction: a driver that crashes and
exhausts its restarts leaves *its device* unavailable while the system is fine,
and must not fail the boot; only a structural failure — the supervisor could not
spawn the binary at all — is hard, and the exit code keys on that count alone.
Back-off is overflow-safe three ways over (shift capped below the word width,
saturating multiply, clamp) where one would have done. A driver killed with no
exit code counts as a crash rather than a clean one-shot.

Both consumers honour their pure module's contract exactly, checked rather than
assumed: the supervisor reads into a buffer sized to the line cap, treats garbled
as give-up, and drives the restart machine with its published limit.

**TWO CANDIDATES DISSOLVED ON MEASUREMENT.** The FDT cell widths are hardcoded
while the DTB publishes its own — but the **kernel's** decoder hardcodes the same
convention in the same words, so it is one documented platform assumption held in
two places, not two answers to one question. And the encoder bounds no list
counts while the decoder bounds all three; that also dissolves, into the
encode/parse principle above (a count overflow is cleanly refusable at the far
end, a delimiter is not). Neither is a defect; both are recorded because a reader
would otherwise ask.

LEDGER, read off the rendered view before being written here. Corpus 848 ->
**850**. Coverage 261 -> **265 owned of 421**, 61% -> **62%**; unswept lines
45793 -> **44264**. libdriver is now fully owned, 8 files of 8.

**The unswept delta is 1529 against 1630 swept, and the 101-line gap decomposes
exactly.** Main moved for the second time in eight batches (the console
receive-admission round-two audit), and the merge grew two unowned code files:
the serial driver by 71 net lines and the compositor's front end by 30. 45793 +
101 - 1630 = 44264, to the line. The in-kernel test files it also grew fall in
the harness-excluded set and do not move the figure. Third distinct behaviour
from this check across four batches — matched, disagreed-and-explained, matched,
disagreed-and-explained — which is what a check rather than a ritual looks like.

**AND THE MERGE WAS CHECKED FOR FALSEHOOD IT MIGHT HAVE INTRODUCED, WHICH IS THE
DUTY LAST BATCH ESTABLISHED.** The round-two audit touches the console files this
vault owns, and its P1 was in the *holdback* — the exact mechanism
[[sub-kernel-cons]]'s receive-back-pressure section describes. Checked
paragraph by paragraph: the dossier claims the holdback "parks that byte and
re-offers it before touching the FIFO again", which the fix **restores** rather
than contradicts (the strand was one exit where it did not). Nothing on the
Present plane became false, so nothing was corrected. Recording the check with a
null result, because last batch the same check found two wrong tables and the
value of running it lies in doing so either way.
