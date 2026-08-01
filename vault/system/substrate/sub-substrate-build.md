---
id: sub-substrate-build
type: sub
parent: moc-substrate
title: "The build — targets, the ledger, and the two staleness checks"
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
updated: 2026-08-01
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

Sub-targets (`userspace`, `sysroot`, `pouch-progs`, `stratumd`, `pool`,
`ramfs`, `disk`) build one stage. `clean` is the only true from-scratch
reset.

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

- The header comment block is the most accurate documentation of the target
  chain in the tree and is actively maintained — prefer it to any prose
  elsewhere, including the absorbed reference docs.
- `THYLACINE_MKFS_PRESERVE=1` skips populate entirely, so new pool content
  needs a one-time `PRESERVE=0`. A runtime "file absent" for something you
  believe you baked is that, and it has produced at least one gate that
  reported PASS having verified nothing.

## Provenance

[[chg-2026-08-01-substrate-sweep]].
