# preserve_boot_inputs: two defects found by its FIRST LIVE RUN

Written 2026-10-07 during run-20261007T140643Z, UNCOMMITTED on purpose: the
runbook records `my HEAD` in its closing provenance block, so HEAD must not move
until that is written. Commit this and the fix after the lease is released.

The step was tested 22 ways against stub trees and passed. Both defects below are
invisible to a stub tree and appeared the first time it saw a real build/. That
is the lesson before either defect: MY STUBS ENCODED MY BELIEF ABOUT THE LAYOUT,
so they could only ever confirm it.

## 1. IT MISSES THE SANITIZER KERNEL -- the artifact that motivated it

The live run reported:

    -- preserved 5 boot input(s) -> work/oct5-as-r9/boot-inputs/run-20261007T140852Z
       ABSENT, recorded and NOT substituted: build/kernel/.config
       build/kernel-undefined/thylacine.elf build/kernel-undefined/thylacine.bin

`build.sh kernel --config ci` does NOT build the sanitizer flavour --
`ci-smp-gate.sh` does, in the stage that runs AFTER my preserve point. Confirmed
live: build/kernel-undefined did not exist when the step ran and exists now.

So the cure does not cover its own motivating case. astra caught me missing
`kernel-undefined/thylacine.bin` (0161 t43); the step I wrote in response records
that exact file as ABSENT, correctly and uselessly. The matrix boots that binary.

FIX: a SECOND call after the gate, separately labelled, so the sanitizer flavour
is captured when it exists. Not a move of the existing call -- the post-build
generation is the one that holds the DEFAULT flavour's inputs beside the
suite's verdict, and the gate's own build overwrites shared paths (see 2).
Both generations then count against KEEP_INPUT_GENS, so that bound needs
re-reading as "generations", not "runs": a run now produces two.

## 2. THE .config PATH IS WRONG, AND THE FILE IS A MOVING TARGET

It looked for `build/kernel/.config`. The file is at `build/.config`. So .config
was silently recorded ABSENT -- correct behaviour on a wrong premise, which is
the quietest kind of wrong.

AND THE DEEPER HALF, which a path fix alone would get wrong: `build/.config` is
SHARED and REWRITTEN BY EVERY FLAVOUR. Measured during this run --

    preserved (default, this morning) cf91bb7fed575925
    build/.config now                 cd0200373d03e647   <- the gate's sanitizer build

cd0200373d03e647 is the SAME hash that
memory/bug_post_run_hash_describes_the_last_flavour.md already records as the
post-gate .config. My own lesson, re-learned by walking into it: a post-run hash
names the LAST flavour built, not the input you pinned.

FIX: capture .config from build/.config at the SAME point as the kernel it
configured, and name the destination for the flavour (.config-default,
.config-undefined) so no reader can mistake which build it describes. A single
unqualified `.config` in a preservation directory is a claim nobody can check.

## WHAT THIS RUN'S PRESERVED GENERATION IS GOOD FOR

Not nothing: thylacine.{bin,elf} came out BYTE-IDENTICAL to this morning's
qualified kernel (5ced18c43ae8302a / e266c931d9668a44) from a rebuild after a
full cache invalidation, which is a reproducibility datum worth keeping. Its
ramfs/pool/key are this run's (8f658d3c7fac7e09 / b9045c6eb55a99ca /
4c1eaaba391836c6) and differ from the 10-07 matrix set, which confirms the bake
replaced them and that the earlier manual preservation was the only thing
standing between those inputs and oblivion.

## BOTH FIXED, 2026-10-07 ~15:2xZ -- and what the fix is NOT

Fixed in the runbook, with the test arm the stubs could not supply.

1. A SECOND CALL, separately labelled, after the gate. The signature is now
   `preserve_boot_inputs <run-stamp> <phase> <expected-flavour>` and a run makes
   two generations: `$RUN_STAMP-postbuild` (the default flavour, beside the
   suite's verdict, still after the Stratum pin assertion) and
   `$RUN_STAMP-postgate` (the sanitizer flavour, which exists only once
   ci-smp-gate has built it). The existing call did NOT move.
   - The post-gate call sits BEFORE the red-gate exit on purpose: a failing run's
     inputs are exactly what diagnosis needs. Its failure is carried in
     POSTGATE_PRESERVE_FAILED and made fatal AFTER the verdict prints, so a red
     gate cannot hide a lost preservation and vice versa.
   - KEEP_INPUT_GENS is re-read as GENERATIONS: GENS_PER_RUN=2, and the function
     REFUSES a bound below that, because with two generations per run a bound of
     1 would have the post-gate call evict its own post-build sibling -- this
     same half-set defect, self-inflicted. `ls -1dt` is newest-first, so at or
     above the bound this run's generations are never the pruned ones; the
     refusal is the whole guarantee rather than a special case in the loop.
   - The default stays 2, i.e. ONE run. Measured: 398 MiB per complete
     generation once the originals are rebaked and the pool snapshot's clone
     blocks stop being shared, so 4 would put 1.6 GiB against the disk the floor
     checks guard. The durable archive remains the curated
     private-owner-qualified-kernel/ with its committed MANIFEST.txt.

2. THE PATH IS build/.config, AND THE FLAVOUR IS READ OUT OF THE FILE. The
   destination is `.config-default` / `.config-undefined`, named from the file's
   own `SANITIZE =` line, never from the caller's argument -- a name nobody can
   check is not provenance. The caller's expectation is kept as a cross-check
   that prints `MISLABEL AVOIDED` and preserves under the OBSERVED name.
   `system.key.baked-snapshot` now travels too: pool_restore's legality guard
   (smp-multiboot.sh:281) compares the live key against that twin, so preserving
   one of the pair left the guard uncheckable by a later reader.
   A generation is a COMPLETE boot-input set, never a diff: the gate rebakes
   ramfs, pool and key, measured within this one run --
     post-build  8f658d3c7fac7e09 / b9045c6eb55a99ca / 4c1eaaba391836c6
     post-gate   947dd838eb890f66 / b165814100fd4dbf / d98f28f3cc7d2f7f
   so a second generation that only topped up the kernels would have been a
   half-set again, in the other direction.

### THE TEST ARM, and the control that makes it worth having

preserve-inputs-test.sh is 44 checks over 8 scenarios (was 22 over 5).
S8 is the one that matters: it takes the source paths OUT of the extracted
function and asks the REAL build/ tree whether they exist, with a denominator
control (>= 8 paths extracted, or the arm declares itself broken) and a loud
SKIP -- never a silent pass -- when the tree has not been built.

POSITIVE CONTROL, run: against the PRE-FIX runbook from git, S8 goes red and
names the defect by itself --

    WRONG: S8 every non-flavour-conditional source path exists (8 checked)
           -- expected [] got [ build/kernel/.config]
    RESULT: 18 pass, 26 wrong, 0 skipped

So the arm would have caught defect 2 on day one. Two further mutants, each one
variable away, redden their named arm and nothing else:
  - name .config from the CALLER's word    -> 42 pass, 2 wrong (both S6)
  - drop the KEEP_INPUT_GENS refusal        -> 41 pass, 3 wrong (all S7)

WHAT THE FIX IS NOT: proof that the step works inside a live run. The two call
sites have not executed in a runbook run; what HAS executed is the fixed function
itself, against the real post-gate tree, preserving
work/oct5-as-r9/boot-inputs/run-20261007T140647Z-postgate -- 9 of 9 inputs, no
ABSENT line, no MISLABEL, and all five hashes astra independently named on 0161
t47 agree with its HASHES.txt. The next real run is what exercises the placement.
