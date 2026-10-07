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
| `self-drain` | local-ownership exclusion (NOT an IPI drain) | **0** | 0 | **0** | 0 |

Every count in that table is an OBSERVATION IN THIS SIMPLIFIED BUDDY DOUBLE --
"a page was on the double's free list when the double's mag_alloc returned it" --
and NOT a count of corruption in the Thylacine guest, which never ran.

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
3. **The LOCAL-OWNERSHIP EXCLUSION IDEA is supported in the modelled
   schedule -- which is NOT the same as qualifying an IPI drain** (astra, 0161
   t59, correcting my first wording "the fix candidate is validated"). The
   `self-drain` leg serialises each thread's own alloc/free/drain. It implements
   no IPI delivery, no completion rendezvous, no offline-CPU handling and no
   stable global measurement, so it says nothing about whether an actual
   IPI-per-CPU drain would be correct, and nothing about making a global
   before/after page gauge sound while peers keep allocating. It supports the
   idea; it qualifies no implementation.
4. **Manifestation is schedule-dependent.** `cross` observed 0 aliases while
   `owner-lock` observed 7575 on identical code, so a count of zero from one
   schedule is not evidence of safety. Stated precisely, because my first
   wording ("a false negative") was wrong in a way that maligned the instrument:
   TSan DID report the race in the `cross` leg. What failed to manifest there was
   my own ALIAS COUNTER, not TSan's detection.

## WHAT IT DOES NOT ESTABLISH

- Nothing about ARM weak memory: TSan models the host's C11 semantics.
- Nothing about probability on real hardware, or about reachability: today every
  caller of `magazines_drain_all` is a test, and "test-harness use" is the
  sanctioned use.
- **Nothing about whether the kernel suite is actually quiescent** at its 24 call
  sites. That is the remaining open question and it needs the guest, not reading.
- It is not a reproduction in Thylacine. No guest ran.
- It qualifies no fix. See point 3 above.

## THE RUNNER JUDGES EVERY LEG'S TERMINATION (added after astra's 0161 t59)

The first version wrote `|| true` after each leg and only warned when the
summary line was missing, so a leg that CRASHED EARLY would contribute zero
races and zero aliases and read as clean -- fail-open evidence, the same class
as an unverified pattern check. Now each leg's exit status is retained in
`<leg>.rc` and a leg counts only if it produced a completed summary line AND
terminated acceptably; otherwise the runner REFUSES TO REPORT (exit 5).

Making that rule crisp needed one more thing: on Darwin TSan ABORTS after
reporting (SIGABRT, status 134), which is indistinguishable from a genuine
crash -- the first run of the new check rejected two good legs for exactly that
reason. With `abort_on_error=0 exitcode=66`, a reporting leg exits 66
deterministically, so "reported a race" and "died" are different statuses.

CONTROL, run: with `CC` pointed at a wrapper that compiles a program which traps
immediately in place of the double, the build and the `__tsan_` check still pass
and all four legs are REJECTED ("no completed summary line -- the leg did not
finish, so its zero counts are not evidence"), exit 5. So the acceptance rule
discriminates rather than decorates.

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
