---
id: sub-substrate-build
type: sub
parent: moc-substrate
title: "The build — targets, the ledger, and the four guards on a stale artifact"
code:
  - tools/build.sh
  - tools/mkcpio.py
  - tools/mkdisk.py
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/TOOLING.md"]
created: 2026-08-01
updated: 2026-08-15
---
## Purpose

Produce the bootable image: kernel ELF + flat binary, the native and Rust
userspace, the pouch POSIX sysroot, the Go GOROOT, the Clade toolchain, the
`ramfs.cpio` initrd, and the Stratum `pool.img` + `system.key` twins.

## Contract

`all` is an ALIAS for `kernel` — the same chain:

```
all -> kernel -> { userspace, pouch-progs, stratumd, pool-fixture, ramfs, disk }
```

Sub-targets build one stage each. `clean` is the only true from-scratch
reset.

**There are nineteen targets and three lists of them, no two of which
agree.** The dispatcher's `case` arms are ground truth at nineteen; the
"Unknown target" error advertises fifteen; the header comment block names
ten. The two graphics/toolchain families — the Clade compiler stages and the
ported-game builders — are the bulk of what the shorter lists omit. Nothing
is advertised that does not exist, so there is no phantom; the drift is one
directional, and it is toward silence.

**Every run ends with a `SUMMARY for target ...` block listing exactly what
was BUILT / REUSED / PRESERVED.** That block is the contract: read it to
know the resulting state rather than inferring it from the target name.

## Mechanism

**`build.sh kernel` is `build.sh all`, and this is the tree's most-repeated
footgun.** It pulls the whole chain including a pool re-bake driven by the
*ambient environment* — so running it without carrying `THYLACINE_BAKE_CLADE`
/ `THYLACINE_BAKE_GOROOT` / `THYLACINE_MKFS_PRESERVE` silently produces a
pool missing payloads a previous invocation had baked in. The gate learned
this the expensive way (#101); the countermeasure lives at the chokepoint,
in [[sub-substrate-gates]].

**Two staleness checks make the cache trustworthy, and both were added
after a stale artifact shipped.**

`sysroot_is_stale` — the pouch libc is cached and reused by `all`, so
editing a boundary-line patch used to link every pouch consumer against a
STALE libc. That is exactly what masked the A-2a `t_stat` 72→80 growth: the
kernel wrote 80 bytes into stratumd's stale 72-byte buffer, a silent stack
overflow that a "passing" boot hid. The check rebuilds when any file under
`usr/lib/pouch/{patches,compiler-rt}` is newer than the built `libc.a`.

`compiler-rt` was **not** watched when it landed, so `all` happily reused a
sysroot predating it and the change looked inert — the same
enumerate-what-you-expect failure as #91. A change to the *recipe*
(`build.sh` itself) still needs an explicit `sysroot`, deliberately:
watching build.sh would rebuild on every unrelated edit. The durable
backstop is a boot probe — a stale `libclang_rt` fails `/pouch-hello`'s
outline-atomics agreement check. Cheap mtime check on top, in-guest proof
underneath.

`stratum_host_tools_stale` mirrors it for the host-native Stratum tools. Its
predecessor rebuilt only when a *binary was missing*, so a Stratum source
edit shipped a stale host `stratumd` and the pool bake failed with "unknown
option".

**The pool and its key are coupled, and the coupling is the fix for a
year-long ghost.** `system.key` is random per regeneration, so a pool
re-bake without a ramfs re-bake leaves `/system.key` (baked into the initrd)
pointing at the wrong pool → `STM_EBADTAG` at mount. That mismatch is "the
year-long 'AEGIS corruption' ghost." The `pool` target therefore couples the
two, and the `kernel`/`all` chain regenerates both together so they always
agree.

**`ramfs.cpio` is not re-baked by `disk` or `userspace`.** The devramfs
holds the PRE-PIVOT binaries — joey's boot chain and every probe that runs
before the pivot to the disk-backed FS. So after editing a userspace binary,
`userspace` + `disk` boots the STALE pre-pivot binary and the change reaches
only the post-pivot image. The tell is precise and worth memorizing: *a
probe's self-reported count or output does not move though you "rebuilt"*.

**The snapshot twins are minted at bake time.** `populate_stratum_pool`
finishes by cloning `pool.img` and `system.key` to `.baked-snapshot`
siblings (`cp -c`, APFS clonefile, plain copy elsewhere). Those twins are
what every downstream harness restores from per boot or per attempt — and
why the key twin can be compared to prove a restore is coherent.

**A third check now runs before any target does, and it is the file's best
structural argument.** A hand-written patch series is validated for
unified-diff hunk line counts at one unconditional chokepoint ahead of the
dispatcher — ~50 ms for 281 hunks. Its comment states the reason plainly:
there are several `patch` loops (the sysroot, the SDL port, the game port,
the compiler and graphics ports), and the lesson from the earlier bake
failure is *verify at one chokepoint instead of copying a check into every
caller.* It exists because the tool ate a function definition out of a port
patch and exited zero — a `patch` that reports success having dropped added
lines past a mis-counted hunk header.

Note what makes this different in kind from the two staleness checks: those
watch mtimes and can only warn. This one reads the artifact's own internal
arithmetic and refuses.

**A fourth guard warns about a stage the main chain never refreshes.** The
compiler-toolchain staging step is reachable only as its own explicit
target, never from `all`, so a rebuilt graphics binary does not reach the
staged tree on its own — and the pool then re-mints faithfully around the
*previous* binary with every ledger line green. That trap has been paid for
twice, and the second time the gate failed three times out of three on a
binary twenty-seven minutes older than the fix under test, *looking exactly
like a real defect in the change*. The warning compares mtimes deliberately:
a content check would mean stripping a ~145 MB binary on every pool bake.
See Caveats for what its own comment claims versus what it does.

**Host-side pool population reuses shipped Stratum tools, not new code.**
The "installer" is shell orchestration: `stratum-mkfs` creates the pool,
host `stratumd` is started on a temp socket, `stratum-fs write` writes each
corpus file through the audited 9P client, stratumd is stopped. No
Stratum-side code exists for it, so the bake exercises the same Twrite /
Tlcreate paths the guest does.

## Data structures

`build/` layout: `kernel/` and `kernel-undefined/` (parallel sanitizer
trees, so the production CMake cache is never clobbered), `sysroot/`,
`go/goroot/`, `clade/stage/`, `fixtures/{pool.img,system.key}` + their
`.baked-snapshot` twins, `ramfs.cpio`, `disk.img`.

## Concurrency

None internally. Two worktrees build concurrently without interference
because every path is repo-root-relative — the shared resource is the host,
not the tree.

## Invariants enforced

None of §28. It *produces* the artifacts several are checked against, and
one build-time property is load-bearing for I-12 (W^X — no registry note
yet; its surface is unswept): the ELF loader rejects W|X, so a segment
layout that would violate W^X fails at exec, not at build.

## Error paths

CMake / cargo / clang failures propagate (`set -euo pipefail`). The
significant *non*-error is the silent skip: several stages return 0 having
built nothing when an optional toolchain is absent — which is why the remote
builders assert on artifacts rather than exit status
([[sub-substrate-builders]]).

## Performance

The sysroot is ~1–2 min; the Go GOROOT and Clade stages dominate a cold
build. The cache checks exist to keep an incremental `all` in the tens of
seconds.

## Prosecution

- A new source directory feeding a cached artifact must be added to the
  corresponding `*_is_stale` watch list, or the artifact silently ages
  (compiler-rt is the worked example).
- A new bake payload must be verified at the chokepoint, not assumed from a
  flag (#101).
- Any new mutable fixture needs a `.baked-snapshot` twin, or the harnesses
  cannot restore it and it will contaminate (#85).
- The pool/key coupling must not be split; a target that re-bakes one alone
  reintroduces `STM_EBADTAG`.

## Seams

[[seam-87-disk-write-proof]] — `disk.img` has no build-maintained twin;
LS-CI mints one with `mkdisk.py` at need.

## Caveats

- ~~The header comment block is the most accurate documentation of the target
  chain and is actively maintained — prefer it to any prose elsewhere.~~
  **THAT ADVICE IS NOW WRONG, AND THIS DOSSIER GAVE IT.** The header is
  still the best account of *what each target it names does* — the caching
  footguns, the pool/key coupling, the summary contract are all there and
  all correct. But as a *list*, it is the least complete of the three: ten
  entries against the dispatcher's nineteen, so it is silent about nine
  working targets including every Clade toolchain stage.

  The failure is worth more than the correction. The claim was true when
  written and decayed without anything failing, because a target added to
  the dispatcher works perfectly whether or not the header mentions it —
  there is no build error, no test, and no user complaint, since the people
  adding targets already know they exist. The only reader who pays is the
  one who does not, and they cannot tell an omission from an absence. That
  is why the recommendation was the dangerous part: it routed exactly that
  reader to the list most likely to be short. Task #180.
- `THYLACINE_MKFS_PRESERVE=1` skips populate entirely, so new pool content
  needs a one-time `PRESERVE=0`. A runtime "file absent" for something you
  believe you baked is that, and it has produced at least one gate that
  reported PASS having verified nothing.

- **The stale-stage warning claims a property it achieves by maintenance,
  not by construction — and its own comment is the argument against
  itself.** The comment says it is *"checked for EVERY staged GL binary, not
  just the one that caught it: the trap is a property of the staging step,
  so a name-by-name check would go quiet again the moment a binary is added
  — which tyr-glquake then was."* The loop two lines below is a name-by-name
  check of four.

  It is **complete today** — the staged set and the watched set were diffed
  and match exactly, with nothing staged-but-unwatched — so the sharper
  finding ("a fifth will go quiet") would have been wrong, and checking is
  what stopped it being filed. What is true is weaker and still real: the
  set is maintained by hand in two places, and adding a binary takes two
  edits with nothing failing if only one is made. The failure mode is a
  warning that does not print, which is the quietest outcome in the file.

  One line from safe-by-default, because the staging step already computes
  the authoritative set. Task #181.

## Provenance

[[chg-2026-08-01-substrate-sweep]].

[[chg-2026-08-15-build-targets]] is the re-sweep: the target set nearly
doubled, the patch-hunk chokepoint and the stale-stage warning arrived, and
this dossier's own "prefer the header block" advice was falsified.
