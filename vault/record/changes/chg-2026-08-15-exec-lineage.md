---
id: chg-2026-08-15-exec-lineage
type: chg
title: "exec re-swept after LINEAGE: a seam closed sideways, and a prediction that came true"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-kernel-exec]
established: []
closed: []
opened: []
mirrors-checked: [kernel/exec.c, kernel/include/thylacine/exec.h]
depth: rich
created: 2026-08-15
---
Six commits, ~1200 lines across `kernel/exec.c` and its header. Unlike the two
previous sweeps this one is mostly *additive* — the recorded mechanisms all
survived — but two of the dossier's forward-looking statements resolved, and
resolving is more interesting than aging.

## The seam closed, and not the way it was written

The recorded seam said there is no `exec(2)`: both bodies reject a target that
already has mappings, so exec is spawn-only. `execve` now exists and **the reject
did not move.** The new loader still refuses a non-empty target — its own comment
reads *clean target only* — because the target is no longer the process. It is a
freshly allocated **detached address space**, and the swap happens one layer up
after the load has entirely succeeded.

That is the better answer to the problem the seam described. Replacing in place
would mean tearing down a live image *while* building its successor, with a
failure in the middle leaving neither. Supplying a clean target instead means a
failed load has touched nothing observable: the detached space is the only
reference, and releasing it drains whatever got mapped.

## The prediction attached to it came true exactly

The dossier's Concurrency section recorded that nothing rejects an exec on a
process that already has threads — the clean-address-space test excludes the
multi-thread case only *as a side effect* — and warned this was "a real guarantee
arrived at sideways ... **exactly the guarantee replace-in-place removes.**"

It did remove it. Two explicit checks replaced it: the exec-alone gate in the
syscall's shared core, and a re-check of the live-peer count *inside the same
critical section as the swap*, which extincts if a peer appeared. The source
gives the second's reason plainly — proving the property at the moment it matters
is cheaper than asking a future reader to reconstruct why no peer could have
appeared.

Worth recording as evidence about the dossiers themselves: a caveat that names
*which* guarantee a future change would remove is the kind that pays. This one
was written six weeks before the change and describes it precisely.

## Three additions, each with its reasoning intact

- **A segment no longer has to be page-aligned.** A real binary arrived whose
  `PT_LOAD` did not start on a page boundary, and ELF never required one. The
  mapping now starts at the page floor below the segment address with an
  intra-page `lead`. That one change propagates into four separate calculations
  — the copy destination, the file-page count in a sparse backing, the
  instruction-cache span (deliberately taken from the Burrow base *because* the
  copy pointer is `lead`-offset), and the fault arm, which still requires offset
  0 to equal the segment address. So the eager and file-backed arms now differ in
  a property they used to share.
- **Non-executable segments are sparsely backed**, on a measurement: init's
  writable segment is 8 bytes of data behind 345 KiB of zero, the identity
  daemon's 128 bytes behind 24 MiB. Executable segments stay eager because the
  coherency sweep runs from the Burrow base over the whole page-rounded segment,
  which is only valid for an eager allocation — the gate and the sweep are two
  hundred lines apart and each names the other.
- **The environment vector joins the startup frame**, bounded on a pointer count
  and a data size independently, with the zero case still asserted equal to the
  fixed frame size.

## One new finding

`exec_map_segment`'s block comment still states the alignment rule the page-floor
change removed — step 1 says "round vaddr range up" when the start now rounds
down, and the copy comment says *"vmaddr_start corresponds to BURROW offset 0
(the segment is page-aligned)"*, whose parenthetical is exactly the deleted
assumption and whose variable no longer exists.

The sharper half: the **conclusion survives** (the floor really does correspond
to offset 0, because the mapping is installed at the floor) and only the stated
reason is false. A reader who checks the justification finds it untrue and cannot
tell whether the claim above it died too — which is worse than a wholly-wrong
comment, because a wholly-wrong one gets deleted. Its own correction sits four
lines below saying the opposite in plain terms, so the file carries both readings
and marks neither as current. Task #178; no behavioural risk.
