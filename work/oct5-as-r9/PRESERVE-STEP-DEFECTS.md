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
