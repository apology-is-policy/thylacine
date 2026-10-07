# The magazine shared-set race: a SEVERITY measurement, not a discovery

Run 2026-10-07T18:09:13Z off-lease (one single-file clang compile,
four two-thread runs, no QEMU, no CMake). Operator-directed after I put the
options to them; astra's standing instruction to keep this defect separate from
the AS-R9 handoff is respected -- nothing here touches production or the branch's
kernel work.

## WHAT WAS ALREADY KNOWN, so this is not presented as new

The owning dossier states the mechanism and the rule:

  - `vault/system/kernel/memory/sub-kernel-mm-phys.md:113` -- "`magazines_drain_all`
    is **quiescent-only** -- it walks peer CPUs' sets with no coordination;
    test-harness and shutdown use only."
  - the same dossier's Prosecution section (:336) -- "any path that touches
    `g_percpu` without [the mask] (or from a peer CPU, as `magazines_drain_all`
    does) must prove quiescence."

The source says it too: `my_cpu()` carries the #807 note that a clustered SoC
"reopens the shared-set race", and `mag_alloc` carries an explicit "#807
regression guard". My first write-up called this an undocumented hazard; that was
wrong and is corrected in the queue entry and to all three peers.

## THE OPEN QUESTION THIS ANSWERS

`mag_alloc`'s #807 guard is `ASSERT_OR_DIE(count <= MAGAZINE_SIZE)`, and its
comment says it trips "LOUDLY instead of silently double-allocating". Does it
cover this class? Predicted no: in the interleaving at issue the count stays IN
RANGE, so the guard cannot see it.

## METHOD

The REAL bodies run. `order_to_mag_idx`, `mag_idx_to_order`, `my_cpu`,
`mag_refill`, `mag_drain`, `mag_alloc`, `mag_free` and
`magazines_drain_all` are EXTRACTED from `mm/magazines.c` by `run.sh`, never
retyped, and each extracted body is asserted to be a VERBATIM substring of the
source (8 of 8, with a denominator control that refuses if a signature moved).
ThreadSanitizer is the instrument, and the runner refuses unless `__tsan_`
symbols are present in the binary -- an uninstrumented binary reporting nothing
looks exactly like a clean one.

The one modelling decision the result rests on: `spin_lock_irqsave(NULL)` is a
BARE IRQ MASK (the dossier: "the MASK, not a lock"), which pins the local CPU
against its own interrupts and excludes nothing on a peer. A pthread has no IRQ
analogue, so the faithful model is no cross-thread exclusion. The
`both-locked` leg exists to prove that rather than assert it.

## RESULTS

| leg | exclusion modelled | TSan races | #807 fires | silent double-alloc | double-free |
|---|---|---|---|---|---|
| `cross` | none (faithful) | 1 | 0 | 0 | 0 |
| `owner-lock` | owner masked, drainer not (also faithful: that IS a mask vs a peer) | 4 | **0** | **7575** | 98 |
| `both-locked` | ATTRIBUTION CONTROL: both excluded | **0** | 0 | **0** | 0 |
| `self-drain` | FIX CANDIDATE: each CPU drains its own set | **0** | 0 | **0** | 0 |

TSan names the conflicting production lines (translated out of the generated
include, and disambiguated by enclosing function because the same statement text
appears in three of these bodies):

    magazines_drain_all  mm/magazines.c:159  while (m->count > 0)
    magazines_drain_all  mm/magazines.c:160  struct page *p = m->entries[--m->count];
  conflicting with
    mag_alloc            mm/magazines.c:113  struct page *p = m->entries[--m->count];
    mag_free             mm/magazines.c:144  m->entries[m->count++] = p;

## WHAT IT ESTABLISHES

1. **The #807 guard is NOT a backstop for this class.** 7575 double allocations
   -- a page handed to a caller while still on the buddy free list -- with the
   guard firing ZERO times, because the count never leaves its range. Anyone
   relying on that guard to make this failure loud should stop.
2. **The finding attaches to the missing cross-CPU exclusion, not to the
   harness.** Giving both sides the same lock takes races AND corruption to zero
   with everything else identical.
3. **The fix candidate holds.** Each CPU draining its own set -- what an
   IPI-per-CPU drain means, the discipline the rest of the file already relies
   on -- does the same work with 0 races and 0 corruption.
4. **A single leg's zero is a FALSE NEGATIVE.** `cross` reported 0 corruption
   while `owner-lock` reported 7575 on the same code: manifestation is
   timing-dependent, so any future attempt to test for this class needs more than
   one schedule before it may report safety.

## WHAT IT DOES NOT ESTABLISH

- Nothing about ARM weak memory: TSan models the host's C11 semantics.
- Nothing about probability on real hardware, or about reachability: today every
  caller of `magazines_drain_all` is a test, and "test-harness use" is the
  sanctioned use.
- **Nothing about whether the kernel suite is actually quiescent** at its 24 call
  sites. That is the remaining open question and it needs the guest, not reading.
- It is not a reproduction in Thylacine. No guest ran.

## TWO HARNESS BUGS, recorded because the second nearly fooled me

- `__atomic_*` builtins do not accept `_Atomic`-qualified objects; the build
  failed loudly, which is the harmless kind.
- **My first "attribution control" was not a control.** It set the owner's mask
  to a real lock but left the drainer unlocked -- and `magazines_drain_all`
  takes no lock at all, so the flag only slowed the owner. The leg came back as
  the WORST one (2177 double-allocs) where I had predicted zero. Reading it as a
  kernel result would have been a fabricated finding; the tell was that a leg
  labelled "control" behaved worse than the faithful leg. A control must be
  checked for being a control.

## REPRODUCE

    sh work/oct5-as-r9/mag-double/run.sh
