---
id: sub-substrate-remote-host
type: sub
title: "warp-host.sh -- the remote GL/KVM host driver: sync, the paired artifacts, and the restore twins"
parent: moc-substrate
code:
  - tools/warp-host.sh
audit: none
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/GPU-HOST-SETUP.md", "docs/GPU-DESIGN.md"]
created: 2026-09-21
updated: 2026-09-25
---
## Purpose

`tools/warp-host.sh` drives a REMOTE Linux host from the build host (the Mac):
it pushes the committed tree plus the boot artifacts built here, then runs one
verb's boot(s) there over ssh and greps its own positive evidence out of the
log. Two hosts exist: `thyla-gl` (a Parallels VM: TCG + lavapipe) and
`thyla-pi` (a Pi 400: KVM + a real V3D), selected by `WARP_HOST`. It never
builds remotely. This dossier covers `sync` in depth -- the verb every other
verb, and every hand-run remote gate, stands on -- and only lists the rest.

## Contract

- `sync` makes the remote `~/projects/thylacine` equal to `git archive HEAD`
  plus `build/kernel/thylacine.{bin,elf}`, `build/ramfs.cpio`,
  `build/disk.img` and the pool. UNCOMMITTED work does not travel, except the
  fixed list of harness files `sync_all` scp's on top (run-vm.sh, lib.exp, the
  warp `.exp` set). A scenario edited but not committed runs at HEAD's version.
- The pool and the ramfs are a cryptographic PAIR: the ramfs bakes
  `bin/system.key`, the pool is sealed under it. A mismatch is `stratumd: run
  failed (rc=-201)` (STM_EBADTAG) and then `EXTINCTION: joey: /joey exited
  non-zero` -- the designed refusal, reading exactly like a guest defect.
- Every verb is fail-closed: no positive evidence line, non-zero exit.

## Mechanism

**The pool travels in 64 MiB chunks.** One 2.5-5 GiB stream does not survive
the Cloudflare tunnel (three attempts died at 600-870 MB -- a deterministic
sustained-transfer limit, so a retry budget buys nothing). Each chunk is its
own short connection, gzipped, landed with `dd seek=` into a pre-sized sparse
`.part`; chunks whose md5 already matches are skipped; the `.part` is renamed
over the live pool only after EVERY chunk hash-matches, so a failed sync leaves
the previous pool intact.

**The pristine pool is what ships (2026-09-21).** `build/fixtures/pool.img` is
what the last local run booted and may have written to; `pool.img.baked-
snapshot` is what `build.sh` minted beside the key. When the snapshot exists
and the key twins cohere (`cmp system.key system.key.baked-snapshot`, the same
test LS-CI applies), the snapshot is the source.

**The restore twins travel with the pool (2026-09-21).** After the rename the
sync copies the remote `pool.img` to `pool.img.baked-snapshot`
(`cp --sparse=always`), ships `build/fixtures/system.key`, copies it to
`system.key.baked-snapshot`, and md5-checks both remote key files against the
local key. WHY: `tools/test-interactive.sh` boots every attempt from
`pool.img.baked-snapshot`, and validates that twin only against
`system.key.baked-snapshot` -- twin against twin, never against the ramfs that
will boot. A host that once baked locally keeps a COHERENT pair of stale
twins, so a sync that shipped pool + ramfs and not the twins booted the new
ramfs on the old pool, on every attempt. Astra lost a Pi run to it on 09-18
(recorded as "her fixture error"); main lost one on 09-21 with a sync that had
md5-verified kernel, ramfs and pool. The key is already on the wire -- the
ramfs bakes it -- so shipping it adds no exposure.

## Data structures

None of its own. The artifact set: `build/kernel/thylacine.{bin,elf}`,
`build/ramfs.cpio`, `build/disk.img`, `build/fixtures/pool.img` (+
`.baked-snapshot`), `build/fixtures/system.key` (+ `.baked-snapshot`).

## Concurrency

Single-flight by convention: one 2 GiB guest at a time on the Pi (4 GiB RAM).
Hold the yip `pi` lease around a sync + run. A sync while a remote guest is
running replaces the pool under it.

## Invariants enforced

None of section 28's. It is the carrier for the I-45 / I-40 silicon witnesses
(`prove`, `tri`, `quake`, `composed`, `readback`) and, since 2026-09-21, for
the graphical trusted path on a real GPU scanout.

## Error paths

- `SYNC-FAILED: chunk N unsendable after 3 attempts` -- the `.part` is kept for
  resume, the previous pool is intact.
- `SYNC-FAILED: assembled pool does not hash-match` -- same.
- `SYNC-FAILED: the remote key twins do not match the local system.key`.
- A verb whose evidence line is missing exits non-zero and says which.

## Performance

A full pool is ~9 minutes over the tunnel; an unchanged pool is a hash pass.
The Pi boots the gauntlet in ~210 s under KVM; `ls-graphical-sak` ran 161 s
there (2026-09-21, virgl=1 ctxinit=1, first attempt).

## Prosecution

Not an audit surface. The 09-21 failure is the one to keep in mind: the sync
VERIFIED everything it shipped, and what broke was a file it did not know the
consumer read.

## Seams

- An ssh session through the tunnel can be closed by the remote end mid-run
  ("Connection ... closed by remote host"), which kills a foreground gate. Run
  long remote gates detached (`nohup setsid ... > log 2>&1 < /dev/null &`) and
  poll the log.
- The remote `BOOT spawn accel=hvf` step line is the scenario's REQUEST;
  `run-vm.sh` falls back to the host's native accelerator (kvm on the Pi). The
  qemu command line, not the step line, says what booted.

## Caveats

- Running a graphical gate accelerated on the Pi is not a verb yet; the
  invocation is `THYLACINE_GPU_DEV=virtio-gpu-gl-pci THYLACINE_DISPLAY=egl-
  headless THYLACINE_EGL_VNC_SOCKET=/tmp/sak-vnc.sock LS_CI_JOBS=1
  tools/test-interactive.sh ls-graphical-sak` from the remote repo (QEMU 10's
  QMP screendump refuses a GL texture scanout; the private VNC socket is how
  the scenario captures pixels).
- The verb list in the file header is the register of verbs; this dossier does
  not restate it.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
