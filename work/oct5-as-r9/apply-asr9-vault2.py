#!/usr/bin/env python3
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))

V = 'vault/system/kernel/memory/sub-kernel-vma.md'
L = 'vault/system/kernel/async/sub-kernel-loom.md'
WF = 'vault/system/kernel/async/sub-kernel-weft.md'

# ---------------- loom: the "deliberately chosen" window was a real defect
ed(L, "updated: 2026-10-04\n", "updated: 2026-10-05\n", 'loom dossier: stamp')
ed(L,
"""The claim happens **before** the drop, because a freeing drop takes the record
with it. If the drop turns out not to free, the claim is put back. The window
between claim and restore is a real one, and its failure mode is deliberately
chosen: a concurrent settler sees the cleared record and skips, leaving a
charge that outlives its region until the payer's next release point. An
over-charge on the payer — never a refund to a Proc that did not pay.
""",
"""The claim happens **inside the drop**, under the Burrow's own lock, through
`burrow_unref_settled` ([[sub-kernel-burrow]]). A drop that does not end the
occupancy leaves the record alone; the drop that does takes it.

This dossier previously recorded the older arrangement — claim, drop, and
restore the claim if the drop turned out not to free — and said "the window
between claim and restore is a real one, and its failure mode is deliberately
chosen: a concurrent settler sees the cleared record and skips, leaving a charge
that outlives its region until the payer's next release point. An over-charge on
the payer — never a refund to a Proc that did not pay." **That was wrong, and it
was not a chosen trade-off but an unnoticed defect (AS-R9).** The lock
serialised each of the three operations and none of the gaps. A sibling holder's
final drop inside the window freed the descriptor, so the restore wrote through
a dead pointer — usually an `extinction`, since the free clobbers `magic` and
SLUB does not zero the slot — and the holder that actually freed the region found
an empty record and refunded nothing. Both `loom_drop_pin_settling` and the
displaced registered-buffer pins now settle through the drop; the full analysis
and the repair are in [[sub-kernel-burrow]].
""",
'loom dossier: correct the "deliberately chosen" window')

# ---------------- vma: the detach's claim is now the drop's
ed(V, "updated: 2026-10-04\n", "updated: 2026-10-05\n", 'vma dossier: stamp')
ed(V,
"""an eager `ANON` whole mapping claims its charge record for `payer` BEFORE the
drop and refunds it iff the drop freed the pages or the region survives only
in another Proc (`shared_out`), else restores the claim for the drop that does
end it (#130/#131, unchanged from the exact-match detach) -- so an eager
""",
"""an eager `ANON` whole mapping settles its charge record for `payer` INSIDE the
drop -- `vma_free_settled_deferred` -> `burrow_release_mapping_settled_deferred`,
which claims iff the drop freed the pages or the region survives only in another
Proc (`shared_out`), and otherwise leaves the record for the drop that does end
it (#130/#131; AS-R9 moved the decision from three separate lock acquisitions in
this function into the drop's own critical section, because a sibling holder's
final drop between the non-final drop and the restore left this path writing
through freed storage -- see [[sub-kernel-burrow]]) -- so an eager
""",
'vma dossier: detach settles inside the drop')

ed(V,
"""claim is taken BEFORE the drop and restored when the region survives on this
Proc's own claim, and that a TRIMMED eager mapping refunds nothing;""",
"""charge is settled BY the drop, leaving the record in place when the region
survives on this Proc's own claim, and that a TRIMMED eager mapping refunds
nothing;""",
'vma dossier: the invariant list entry')

# ---------------- weft: both settle sites moved onto the settled drop
ed(WF, "updated: 2026-08-24\n", "updated: 2026-10-05\n", 'weft dossier: stamp')
ed(WF,
"""The release rule is now: the sharer settles **when the region is shared out and
this process has unmapped it**, whether or not the pages freed.""",
"""The release rule is now: the sharer settles **when the region is shared out and
this process has unmapped it**, whether or not the pages freed. Both of this
file's settle sites — the explicit share unregister and the owner orphan sweep —
take that decision inside the reference drop (`burrow_unref_settled`,
[[sub-kernel-burrow]]): AS-R9 found that claiming the record, dropping the pin
and restoring the record on a non-final drop left a window in which another
holder's final drop freed the descriptor under the restore.""",
'weft dossier: settle inside the drop')

texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None: t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times in {path.name}, expected 1'); fail = True
    else:
        texts[path] = t.replace(old, new, 1); print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path.relative_to(R)}')
