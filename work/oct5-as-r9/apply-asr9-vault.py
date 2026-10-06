#!/usr/bin/env python3
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))

B = 'vault/system/kernel/memory/sub-kernel-burrow.md'

ed(B, "updated: 2026-09-23\n", "updated: 2026-10-05\n", 'burrow dossier: updated stamp')

ed(B,
"""- **`burrow_charge_restore`** puts a claim back when the caller decides not to
  settle. Callers must claim **before** the drop, because a freeing drop takes
  the record with it. The window's failure mode is stated and asymmetric: a
  concurrent settler that sees the momentarily-cleared record simply skips, so
  the charge outlives its region until the payer's next release point — an
  over-charge on the payer, never a refund to a Proc that did not pay.
""",
"""- **`burrow_charge_restore`** puts a claim back when the caller decides not to
  settle. It is **not a general-purpose primitive**: see the settled drops
  below. Exactly one caller is still allowed to use it — `SYS_JIT_DESTROY`,
  which holds an independent reference for the whole interval.
- **`burrow_charge_claim_locked`** is the claim with `v->lock` already held, so
  the settled drops can take the record inside the critical section that decides
  finality. `burrow_charge_claim*` is that function plus the lock, which is why
  there is one claim implementation rather than two that can drift.

#### AS-R9: the claim/drop/restore window was a use-after-free, not an over-charge

This dossier previously recorded that the claim-then-drop-then-restore window's
"failure mode is stated and asymmetric: a concurrent settler that sees the
momentarily-cleared record simply skips, so the charge outlives its region until
the payer's next release point — an over-charge on the payer, never a refund to a
Proc that did not pay." **Both halves of that were wrong**, and the sequence has
been replaced for every caller that cannot prove a surviving reference.

The Burrow's lock serialised each of the three operations and none of the gaps
between them. After a drop reported non-final, another holder could make the
**final** drop before the restore ran. Two independent failures follow:

1. **The restore writes through a dead pointer.** `burrow_free_internal`
   clobbers `magic` and returns the slot to SLUB, which does not zero it, so the
   write lands on a slot that is either still free (`magic == 0` →
   `extinction`, i.e. a whole-system kill reachable from an ordinary pair of
   concurrent closes — the dominant outcome), already reissued with no charge
   recorded (the payer's charge is **planted on an unrelated region**; it becomes
   a wrong refund only if that region is never `burrow_charge_record`'d, since
   that call overwrites unconditionally, and is later settled against the same
   AddrSpace id — reachable for the backings that take no record, but not the
   likely case), or already reissued **with** a charge (the `charge_pages != 0`
   arm fires `extinction("re-charged mid-settle")`, whose own comment asserted
   that case cannot happen — a **fabricated** fault). It can also race a
   concurrent `burrow_create` initialising that slot.
2. **The settlement is lost.** The holder that actually frees the region reads
   the momentarily-cleared record, claims nothing, and refunds nothing — so the
   payer stays charged for pages that no longer exist.

The repair is **`burrow_unref_settled_in` / `burrow_unref_settled` and
`burrow_release_mapping_settled_deferred`**: the decrement, the `{0,0}` dual
decision and the charge claim run in **one** hold of `v->lock`. A drop that does
not qualify leaves the record **alone**, so the holder that does qualify still
finds it; a drop that qualifies takes it under the lock, so settlement is
exactly-once. Neither form touches `v` after the reference it dropped is gone,
which is why no caller needs a surviving reference. The refund comes back as a
scalar precisely so the caller can apply it **outside** the leaf lock. `payer` is
the exact AddrSpace incarnation; `NULL` means "settle nothing", which is how a
caller whose policy predicate fails opts out.

`burrow_release_mapping_settled_deferred` keeps its twin's deferred contract —
hand back the dead Burrow rather than free it under the caller's `as->lock` — and
qualifies on `freed || shared_out`, reading `shared_out` under that same lock.
Being monotonic false → true, observing it at drop time rather than before the
drop can only **add** a reason to settle, never miss one that mattered.

Five callers were migrated: `loom_drop_pin_settling`, Loom's displaced
registered-buffer pins, Weft's share unregister and owner orphan sweep, and the
eager-ANON arm of `vma_detach_range_in` (through the new
`vma_free_settled_deferred`, of which `vma_free_deferred` is now the no-payer
wrapper, so the Vma validation is not duplicated). `SYS_JIT_DESTROY` keeps
claim/restore, and the premise is now written at the site instead of inherited:
every failure return in `burrow_unmap_reporting` precedes that function's first
mutation, so a nonzero rc is **no** teardown rather than a partial one and leaves
its alias attached; the restore arm runs only when one of the two unmaps failed,
so at least one alias still holds a mapping ref; and both aliases live in the one
address space whose lock is held across the interval, while refs from any other
space only **add** to the counts. Premise one is the fragile half — a failure
return added below the mutation point would silently make that site a UAF — so
`burrow.unmap_failure_leaves_mapping_attached` pins it, with a
correctly-shaped unmap as the positive control one variable away.
""",
'burrow dossier: AS-R9 section')

ed(B,
"""**The release rule is user-voted and is not "follow the pages".** A detach
settles on `freed || shared_out`.""",
"""**The release rule is user-voted and is not "follow the pages".** A detach
settles on `freed || shared_out`, now decided inside the drop (above).""",
'burrow dossier: point the release rule at the settled drop')

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
