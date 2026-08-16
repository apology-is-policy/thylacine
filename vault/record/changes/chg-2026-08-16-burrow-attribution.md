---
id: chg-2026-08-16-burrow-attribution
type: chg
title: "The Burrow re-swept: the type says SHAPE, and that lesson has now arrived three times"
date: 2026-08-16
arc: arc-vault
commits: ["0fd20c64"]
touched: [sub-kernel-burrow]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
342 lines across five commits, and the sweep's value is not any one of them —
it is that two unrelated arcs landed the **same move** on this object within
days of each other, neither citing the other, and neither noticing that a third
instance was already in the file.

## The through-line the dossier now leads with

A Burrow's type tells you how its pages are *arranged*. Three times it has been
asked to answer a question about something else, and it was wrong every time:

- **May userspace map this executable?** — inferred from "it is anonymous".
  Fixed by `BURROW_TYPE_CODE`: a type that is not a different allocator (the
  backing is byte-identical to anon) but a different **admissibility**.
- **Did *this* unmap free the pages?** — inferred from the type plus a sampled
  count, neither of which is the drop's own effect. Fixed by making the
  operation report what it did.
- **Who paid for these pages?** — inferred from the region's shape. Shape stops
  naming a payer the moment two Procs can reach the region.

Each was fixed identically: **the property moved onto the object, minted by the
kernel, unforgeable by the caller.** Three arcs, three reasons, one shape. The
dossier could describe the first two before this sweep and did not connect
them; the third is what made the pattern legible.

## The charge record

`struct Burrow` gained `charge_pid` / `charge_pages` / `shared_out`, with
record / claim / restore. Two defects in **opposite directions** motivated it,
and the pairing is the argument:

- A Loom registered-buffer refund went to the Loom's owner, justified by
  "registering requires a loom fd from that Proc's own table". That argument
  proves who owns the **Loom** and says nothing about who paid for the
  **buffer** — so a consumer could be refunded for a sharer's pages. An
  under-count, inflating a non-exempt Proc's budget, reachable through the
  public API.
- Nothing settled the sharer's charge at all. The last drop was the *guest's*
  teardown: generic code, in another Proc, holding that Proc's lock, and
  structurally unable to name the payer. Pages leaked per closed flow.

Neither is visible from the region's shape. That is the whole case for
recording the payer rather than deriving it.

Three details worth keeping:

- **The claim is read-and-clear**, so exactly one settler ever refunds. Two
  racing paths cannot both win, and a double refund is an under-count — the
  direction that breaks the bound rather than merely wasting budget.
- **`charge_pages` is the sentinel, not `charge_pid`.** Pid 0 is a legitimate
  identity: `proc_alloc` stamps 0 and the fork path assigns later.
- **The restore window's failure mode is asymmetric and stated.** A concurrent
  settler seeing the momentarily-cleared record skips, so the charge outlives
  its region until the payer's next release point — an over-charge on the
  payer, never a refund to a Proc that did not pay.

The release rule was put to the user rather than assumed, and the survey is why
it needed a vote: **Linux memcg keeps the charge with the allocator and
reparents on death, seL4 lets it follow the capability holder, Zircon counts
shared pages in every mapper.** Three real systems, three different answers, so
there is no "obviously correct" default to fall back on. Thylacine's dual axis
takes seL4's answer for the sharer half — the charge follows the sharer's own
*claim*, not the pages.

## A bound that holds by coincidence is not a bound

The leak had no live bound breach only because the leaking daemon happens to
run as the system principal, which is exempt — **a coincidence of two
independent gates rather than an enforced property.** The first non-exempt
driver on that path converts it to a real monotonic leak.

Worth carrying as a reasoning pattern in its own right, because the safe-today
reading and the correct reading differ here in a way that is easy to miss: the
measurement ("no breach") is true, and the conclusion it invites ("so it is
bounded") is false.

## Cross-Proc sharing widened, and the dossier's rule was honoured

The Prosecution section said sharing is anon-only and that widening it "needs
the hardware-isolation analysis that was explicitly deferred, not just a
relaxed type check."

The widening happened — and it arrived **carrying exactly that analysis**.
Admissible now: ANON, or a DMA Burrow whose hardware object holds one of two
kernel-minted, create-immutable, mutually-exclusive bits. Plain DMA and MMIO
remain structurally unshareable.

The part that makes it right rather than merely documented: **the two bits are
not one relaxation with two names.** `weave` is device-*read* only (pixels
outbound); `gpu_bo` is device-*written* (render target, readback), and its
safety argument is that what the GPU may write is bounded by GPU-side
translation only the trusted owner programs — a claim about hardware the kernel
does not itself enforce. Different claims, separate fields. Collapsing them
into one "shareable" flag would extend the weaker argument over the stronger
case silently.

And both bits are set **only** by their own minting syscall — the same
discipline `BURROW_TYPE_CODE` exists to enforce for executability, arrived at
independently. That is the fourth instance of the pattern above, in the same
file, in the same fortnight.

A prosecution rule that named a precondition, and a later change that met it,
is the outcome these rules are for. Recording it as a *success* matters: the
rule now binds the new boundary rather than being quietly deleted as
overtaken.

## The fixture nearly proved nothing

The charge record's test initially gave both its Procs pid 0 — the value
`proc_alloc` stamps before a real one is assigned — so the payer check matched
by **coincidence** and the test would have passed without exercising
attribution at all. Distinct pids are what turned it into a test.

Nothing about the assertion looked wrong; the defect was entirely in the
fixture's default state happening to satisfy it. The two regressions were then
revert-probed on **distinct** assertions — undoing one fix fails only its own
leg, and neither masks the other — which is the bar a single test covering both
would have quietly missed.

## What did not change, and was re-verified rather than assumed

Six backing types, not seven: the Warp GPU-BO is a **DMA-handle subtype bit**,
not a new Burrow type, so the dossier's title and constructor table stand. The
header's opening summary still says "`BURROW_TYPE_ANON` only" sixty lines above
an enum defining six.

That last one earned an addition. Two arcs landed through this file in this
window, both writing careful comments **at their own sites**, both leaving the
preamble alone. That is locally correct behaviour every time, and it is exactly
why an opening summary decays faster than anything else in a file: every author
is drawn to the line they are changing, and nobody's change is ever *about* the
preamble.
