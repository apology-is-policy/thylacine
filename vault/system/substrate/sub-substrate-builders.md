---
id: sub-substrate-builders
type: sub
parent: moc-substrate
title: "The remote builders — disposable spot, permanent keep"
code:
  - tools/clade-gcp-build.sh
  - tools/clade-keep-build.sh
  - tools/clade-stage1.sh
audit: none
guarded-by: []
validated-by: [prose]
locks: []
abis: []
design: ["docs/LLVM-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Build what the dev host cannot. The Clade device toolchain (a cross-LLVM
fork plus Mesa/llvmpipe) needs ~45 GiB of intermediate tree and hours of
compute; the macOS dev box is also where both sanitizer runtimes are broken.
Two tools, because the right shape depends on what you want back.

## Contract

- **`clade-gcp-build.sh`** — creates a disposable spot VM, builds, fetches,
  and DELETES it. Correct when the artifact you want is the multicall
  binary.
- **`clade-keep-build.sh`** — drives `thyla-keep`, a machine that is STOPPED,
  never deleted, whose `/build` disk survives. Correct when the artifact you
  want is the BUILD TREE. Verbs: `start` / `sync` / `stage1` / `stage2` /
  `stage3` / `all` / `log` / `inventory` / `fetch` / `stop` / `status`.

## Mechanism

**Which shape, and why there are two.** CL-7 links Mesa against the
cross-LLVM's 207 static libraries. Re-creating that tree per iteration is
hours per attempt, so the permanent builder turns a 24-minute cold build
into an incremental ninja. The disposable tool stays because for a
self-contained artifact, create-build-fetch-destroy is cheaper and leaves
nothing to forget about.

**STOP, NEVER DELETE — enforced structurally, not by discipline.** There is
deliberately no `down`/`delete` verb on the keep tool, and the instance name
sits OUTSIDE `clade-gcp-build.sh`'s `clade-builder-*` prefix so that tool's
teardown **provably cannot reach it**. Stopped, the machine costs disk only;
running, it also costs compute — so `stop` when idle. A safety property
expressed as a naming invariant rather than a rule someone has to remember.

**EVERY STAGE ASSERTS ON ARTIFACTS, NOT EXIT STATUS.** `build.sh` skips
silently and returns 0 when it cannot find the fork toolchain, so `set -euo
pipefail` cannot catch a run that built nothing — and a green-but-empty
stage 2 is exactly what motivated the assertions. This is the same rule the
gates learned as "verify the artifact, not the intent" (#101), arrived at
independently on the remote side, and it generalizes past the build: an
**archive cannot fail** — a 210 MB `libOSMesa.a` once built perfectly while
missing every GL entry point, and only running the executable said so.

**No config is duplicated.** Stage 1's recipe is `clade-stage1.sh`, shared
verbatim with the disposable tool — one original, two callers. Stage 2 runs
the real `build.sh` targets with the working copy overlaid. Stage 3 is the
only recipe original to the keep tool, and it is a target list, not a
configuration.

**Quota has dimensions you cannot see.** Only an attempted create reveals
them, and a disk can be too full to run the mechanism that would un-fill it.
Cost discipline is the standing rule: propose-then-execute for mutating
`gcloud` operations, batch queued payloads onto one machine, and tear a
disposable down immediately.

## Data structures

Remote: `/build` on the keep machine's persistent disk (the LLVM tree, the
sysroot, the staged toolchain). Local: `build/clade/stage/bin` — whose
presence is what makes `ci-smp-gate.sh` default `THYLACINE_BAKE_CLADE=1`.

## Concurrency

One remote build at a time per machine. `all` runs detached with `log` to
follow — a remote `nohup ... &` is the correct shape there, and it needs
`< /dev/null` or it inherits a dying stdin.

## Invariants enforced

None. This is host/cloud tooling.

## Error paths

Per-stage artifact assertions are the real error path; a stage that
"succeeds" without producing its artifact is the failure mode being guarded.
`inventory` exists to answer "what is actually built up there" independently
of any run's claimed success.

## Performance

Cold LLVM ≈ 24 min on the keep machine (32 vCPU) versus hours locally; the
dev box builds at `-j2` when it participates at all. Disposable spot
`e2-standard-4` ≈ $0.04/h, a boot-disk-only run ≈ $0.02–0.05.

## Prosecution

- A new stage must assert on its artifact. Exit status is not evidence here,
  by construction.
- Nothing may give the keep tool a delete verb, and the keep instance's name
  must stay outside the disposable tool's teardown prefix.
- A recipe that drifts between the two tools reintroduces the divergence the
  shared `clade-stage1.sh` exists to prevent.

## Seams

None open.

## Caveats

- Sanitizer runs (ASan / LeakSan / TSan) go to a disposable Linux VM because
  BOTH are broken on the macOS host — TSan SIGSEGVs inside
  `__tsan::InitializePlatform` before any user code; ASan hangs producing no
  output. The kernel is unaffected (no libc, no sanitizer runtime): its
  witnesses are the UBSan build and the multi-boot gate.
- A stage verified only on the machine that generated it is not verified;
  fork patch series are regenerated per arc for that reason.

## Provenance

[[chg-2026-08-01-substrate-sweep]].
