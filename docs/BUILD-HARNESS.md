# The Thylacine Build Harness — a developer's manual

**Audience.** Anyone building Thylacine from source, customizing what a boot
image contains, or adding their own files and binaries to a running system. This
is the hands-on companion to `docs/TOOLING.md` (which covers the QEMU launcher,
the 9P host share, snapshots, and the agentic loop) and `CLAUDE.md`'s "Build +
test commands" (the canonical command list). Where those describe *what* the
tools are, this describes *how to drive the build* and *how to get your own
content into the guest*.

Everything below is driven by one script, **`tools/build.sh`**, with a thin
`Makefile` of aliases over it. The real build systems underneath are CMake (the
kernel) and Cargo (the Rust userspace); `build.sh` orchestrates them plus the
Stratum host tools that assemble the on-disk pool.

---

## 1. What a build produces

A bootable Thylacine is **two artifacts**, and QEMU is handed both:

| Artifact | Path | What it is | Persistence |
|---|---|---|---|
| **The kernel** | `build/kernel/thylacine.bin` (+ `.elf`) | The ELF, flattened to a raw binary, passed as QEMU `-kernel`. | n/a |
| **The ramfs** | `build/ramfs.cpio` | An initramfs cpio: `joey` (init), the userspace binaries, boot fixtures, and **`/system.key`**. Passed as `-initrd`. Lives in RAM. | **Non-persistent** (rebuilt every boot from the cpio) |
| **The pool** | `build/fixtures/pool.img` (+ `system.key`) | A pre-formatted, pre-populated **Stratum** filesystem image, mounted read-write over 9P. Passed as a `virtio-blk` drive. | **Persistent** (survives reboots; this is the real disk) |
| **The disk** | `build/disk.img` | A small secondary scratch `virtio-blk` drive (default 16 MiB). | Persistent |

**The load-bearing invariant: the ramfs and the pool are cryptographically
paired.** Re-baking the pool mints a *fresh random* `system.key`; the ramfs
bakes a copy of that key at `/system.key`, and the initramfs feeds it to
`stratumd` at boot. If you rebuild one without the other, the guest mounts a
pool whose AEAD metadata key no longer matches the ramfs key and stratumd fails
the first B-tree node tag verify with `STM_EBADTAG` (`stratumd: run failed
(rc=-201)` at mount). **This is why every pool re-bake also re-bakes the ramfs**
(see the `pool` target below) — and why you must never ship one without the
other. If you see `STM_EBADTAG`, you have a stale pairing.

---

## 2. Prerequisites

The kernel and native userspace need an LLVM/Clang cross toolchain plus the Rust
`aarch64-unknown-none` target; the pool needs the Stratum source tree; QEMU runs
it. Optional toolchains unlock optional bake chunks (Section 6).

| Need | For | Default location / how to get it |
|---|---|---|
| LLVM + Clang | kernel + pouch sysroot | `/opt/homebrew/opt/llvm` (override `LLVM_PREFIX`) |
| lld | linking pouch programs | `/opt/homebrew/opt/lld` (override `LLD_PREFIX`) |
| CMake | kernel + C userspace | on `PATH` |
| Rust + Cargo | native userspace | `rustup target add aarch64-unknown-none` |
| QEMU (`qemu-system-aarch64`) | running | on `PATH` |
| Stratum source | the pool | a sibling tree; `build.sh` finds it and builds `stratum-mkfs`/`stratumd`/`stratum-fs` on demand |
| **Go fork** (`$GOFORK`) | `/goroot` chunk | `~/projects/go-thylacine` (override `GOFORK`); absent → the Go chunk skips cleanly |
| **JDK + tla2tools.jar** | TLA+ specs (`make specs`) | see `CLAUDE.md` "TLA+ setup" |

An absent *optional* input never fails the build — the corresponding chunk is
skipped and the build summary says so.

---

## 3. Quick start

```bash
tools/build.sh all      # kernel + sysroot + userspace + pool + ramfs + disk
tools/test.sh           # boot it under QEMU, assert the boot banner
```

or with the Makefile aliases:

```bash
make all && make test
make run                # launch an interactive dev VM (UART on the terminal)
```

**Want a complete image with everything baked in that boots fast?** Use the
all-in-one script — every bake chunk turned on, `--production` so there are no
boot tests and the image goes straight to the login getty:

```bash
tools/build-everything.sh          # or: make everything
SKIP_CLADE=1 make everything       # skip the one slow chunk (the LLVM device toolchain)
```

It turns on every chunk toggle, stages the ones `build.sh all` doesn't stage
itself (`/clade`, `/storm`, `/quake`), and reports up front which optional chunks
have their inputs present (an absent input skips that chunk with a warning rather
than failing the build). See Section 5 for what each chunk is, and Section 8's
speed note: this is the *slow build* that yields the *fast boot*.

**Read the build summary.** Every `build.sh` run ends with a
`==> [build.sh] SUMMARY for target '<t>'` block that lists exactly what was
**BUILT / REUSED / REGENERATED / PRESERVED** this invocation. Caches are content-
addressed and aggressively reused, so "REUSED" is normal; when in doubt about
what actually changed, read that block rather than re-deriving it.

---

## 4. `tools/build.sh` — targets and flags

### 4.1 Targets

```
tools/build.sh <target> [flags]
```

| Target | Builds |
|---|---|
| `all` | the whole chain: kernel, sysroot, userspace, pool, ramfs, disk (default if no target given) |
| `kernel` | the kernel ELF + flat binary. **Note: this runs the full `all` chain** — it re-bakes the pool and ramfs too, because a kernel is not bootable without them. There is no "kernel only" target. |
| `sysroot` | the pouch musl sysroot (cross libc for ported programs) |
| `userspace` | the Rust (`aarch64-thylacine`) + C userspace binaries |
| `ramfs` | re-assemble `build/ramfs.cpio` only (fast; picks up rebuilt userspace + the current `system.key`) |
| `pool` | regenerate `pool.img` + `system.key`, **then re-bake the ramfs** (keeps the pairing sound) |
| `disk` | the secondary scratch `disk.img` (size from `THYLACINE_DISK_SIZE`) |
| `go-probes` | rebuild the Go boot probes + re-bake the ramfs (no full kernel rebuild) |
| `stratumd` / `pouch-progs` / `sdl2` / `tyrquake` / `gnumake` / `libcxx` | individual ported components |
| `clade` / `stage-clade` / `stage-storm` | build/stage the on-device LLVM toolchain payload (Section 6) |
| `clean` | remove `build/` |

The `Makefile` exposes the common ones as `make <target>` plus `make production`
(= `all --production`).

### 4.2 Flags

| Flag | Effect |
|---|---|
| `--release` | Release build (`-O2`, no assertions). Default is `Debug` (assertions on). |
| `--sanitize=ubsan` | UBSan kernel, in a separate build dir (`build/kernel-undefined`). |
| `--hardening-full` | Enable the full P1-H hardening flag set. |
| `--kaslr` | Enable kernel-base KASLR. |
| `--production` | The lean **V1.0 boot shape**: drops the in-kernel test suite (`KERNEL_TESTS=OFF`) *and* joey's boot-test probe ladder (`THYLA_BOOT_PROBES=OFF`), so the image boots straight to the login getty. The default (dev/CI) shape keeps both. |
| `--verbose` | Verbose CMake/Cargo output. |

Example:

```bash
tools/build.sh all --release --production   # a lean, optimized ship image
tools/build.sh kernel --sanitize=ubsan      # a UBSan kernel for the SMP gate
```

---

## 5. Bake chunks — what goes *into* the image

A "chunk" is an optional payload the harness bakes into the boot image. Each is
toggled by an environment variable at build time and, when enabled, either
lands in the **ramfs** (RAM, every boot) or the **pool** (disk, persistent).
Most large chunks land in the pool. The build summary reports each chunk's
disposition.

### 5.1 The chunk toggles

| Env var | Default | What it bakes | Enables |
|---|---|---|---|
| `THYLACINE_BAKE_GOROOT` | `1` (on) | The trimmed Go `GOROOT` → **`/goroot`** (needs `$GOFORK` staged; ~hundreds of MB) | Running the Go toolchain on-device; the `go` driver, `gopls`, the Go boot probes |
| `THYLACINE_BAKE_CLADE` | `0` (off) | The device LLVM toolchain → **`/clade`** (`clang++`, `ld.lld`, headers, sysroot; ~280 MiB). Requires a prior `stage-clade`. | On-device C/C++ compilation (the CL-4 clang++ gate) |
| (rides `BAKE_CLADE`) | — | The build-storm sources → **`/storm`** | The CL-5 on-device build-storm |
| `THYLACINE_CHASE_W2` | `0` (off) | A marker file `/chase-w2` | joey's heavy on-device `cmd/compile` bench steps (CHASE W2) |
| `THYLACINE_ALPINE_TARBALL` | (unset) | An Alpine minirootfs → the `alpine*` viv bundles under `/vivarium` | Running stock Alpine Linux binaries under the VIVARIUM phenotype |
| `THYLACINE_BUSYBOX_STATIC_APK` | (unset) | A `busybox-static` apk into the Alpine bundles | busybox applets in the phenotype (every stock Alpine ELF is dynamic-PIE; the static busybox is the bootstrap) |

Chunks that are **always** baked when their inputs are present (no toggle):
`/quake` (Quake shareware, if `build_tyrquake` staged it), `/vivarium` (the viv
container bundles), `/var/lib/corvus/*` (the host-minted system identity), and
`/lib/ndb/local` (the network database). Absent inputs skip silently.

### 5.2 Pool-fixture controls

| Env var | Default | Effect |
|---|---|---|
| `THYLACINE_MKFS_PRESERVE` | `0` | `1` = **skip the populate step** and keep the existing `pool.img` as-is. **Trap:** a preserved pool keeps the *old* content and the *old* key — safe only if you did **not** rebuild the ramfs. Use it to iterate on kernel/ramfs without paying the ~minute pool populate; set it back to `0` (and re-bake) the moment a pool payload changes, or joey's fixtures go stale. |
| `THYLACINE_MKFS_SEED` | (random) | Pin the mkfs RNG seed for a reproducible `pool.img`. The seed used is logged either way. |
| `THYLACINE_DISK_SIZE` | `16M` | Size of the secondary `disk.img`. |
| `GOFORK` | `~/projects/go-thylacine` | Path to the Go-port toolchain fork (for `/goroot`). |
| `CLADE_JOBS` | (nproc) | Parallelism for the `/clade` toolchain build. |

### 5.3 Examples

```bash
# A minimal image: no Go toolchain on disk (faster, smaller pool).
THYLACINE_BAKE_GOROOT=0 tools/build.sh all

# An image with the on-device C/C++ compiler at /clade.
tools/build.sh stage-clade          # build + stage the toolchain payload
THYLACINE_BAKE_CLADE=1 tools/build.sh pool   # bake it into the pool + re-pair ramfs

# A reproducible pool (same bytes every run).
THYLACINE_MKFS_SEED=0xC0FFEE tools/build.sh pool

# Iterate on the kernel WITHOUT re-populating the pool each time.
# (Only safe while no pool payload and no ramfs input changed.)
THYLACINE_MKFS_PRESERVE=1 tools/build.sh all

# Run stock Alpine binaries under the phenotype.
THYLACINE_ALPINE_TARBALL=~/dl/alpine-minirootfs-3.21.0-aarch64.tar.gz \
THYLACINE_BUSYBOX_STATIC_APK=build/cache/busybox-static-*.apk \
tools/build.sh all
```

---

## 6. Including your own files and binaries in the guest

There are two destinations, and which you want depends on **persistence** and
**when the file is needed**.

### 6.1 The RAMFS — RAM-resident, available from the first instant of boot

The ramfs is `build/ramfs.cpio`, assembled by `build_ramfs()` in
`tools/build.sh` from a staging dir `build/ramfs-src/`. At boot the cpio root
binds at `/` (its binaries appear at `/bin`), *before* the pool is mounted, so
this is where init-critical content lives. Everything here is **rebuilt every
boot from the cpio** — non-persistent.

**Binaries land automatically from the userspace build.** `build_ramfs` copies
from hardcoded lists:

- Native (libthyla-rs, `no_std`) Rust binaries from
  `build/usr-rs/aarch64-unknown-none/release/<name>` — the shell (`ut`), the
  coreutils, `corvus`, the drivers, etc.
- Pouch / C binaries from `build/usr/<name>/<name>`.
- Go probes built with `$GOFORK`.
- Ported daemons (`stratumd`, …).

To add a **new native binary** to the ramfs: build it into the userspace
workspace so it lands in `build/usr-rs/.../release/`, then add its name to the
relevant `usr_rs_bins` / `usr_bins` array in `build_ramfs`. Re-bake with
`tools/build.sh ramfs` (fast — no kernel rebuild).

To add a **data file / script**: drop it into the `ramfs-src` assembly in
`build_ramfs` (the function already does this for `welcome`, `version`,
`net-demo.ut`, the ambush init scripts, etc.), then `tools/build.sh ramfs`. A
`#!/bin/ut` script baked `0755` is runnable by name once `/bin` is on the path.

`/system.key` is baked here automatically from `build/fixtures/system.key` —
never hand-edit it (see the pairing invariant, Section 1).

### 6.2 The POOL — persistent, on the real Stratum disk

The pool is the persistent filesystem. Content is written **at host build
time**, before QEMU ever starts, by `populate_stratum_pool()` in
`tools/build.sh`. The mechanism: the harness starts a **transient host
`stratumd`** on `pool.img` over a private unix socket, then drives the 9P CLI
client **`stratum-fs`** to write the boot corpus, then tears `stratumd` down —
producing a pre-populated `pool.img`.

The `stratum-fs` verbs the populate uses:

```bash
stratum-fs -s <sock> write <pool-path>          # write a file, content from stdin
stratum-fs -s <sock> put   <local-path> <pool>  # recursively copy a file OR dir in
stratum-fs -s <sock> mkdir <pool-path>          # single level only (no -p)
stratum-fs -s <sock> sync                        # flush to the image
stratum-fs -s <sock> read  <pool-path>           # read back (used for verify)
```

Worked shapes, taken verbatim from the populate routine:

```bash
# a single file from stdin
echo "$content" | stratum-fs -s "$sock" write /thylacine-version

# a whole directory tree in one recursive put (a per-file loop is infeasible)
stratum-fs -s "$sock" put "$goroot_stage" /goroot
stratum-fs -s "$sock" sync

# a nested path — mkdir is single-level, so create parents top-down
for d in /var /var/lib /var/lib/corvus; do stratum-fs -s "$sock" mkdir "$d"; done
stratum-fs -s "$sock" write /var/lib/corvus/system-wrap < "$minted"
```

**To add your own persistent content**, add a `write`/`put` block to
`populate_stratum_pool()` alongside the existing ones (guard it behind a chunk
toggle if it is large or optional — follow the `THYLACINE_BAKE_GOROOT` /
`THYLACINE_BAKE_CLADE` pattern), then re-bake:

```bash
tools/build.sh pool     # re-populates + re-pairs the ramfs
```

Read it back inside the guest at the path you wrote (e.g. `cat /goroot/VERSION`).
For anything non-trivial, do a `read`-back `cmp` after the write, as the routine
does for the corvus wraps and the ndb file — it catches a mangled write at build
time rather than at a boot-time tag failure.

**Note on path depth:** the *build-time* `stratum-fs put` writes arbitrarily
deep trees fine (that is how `/goroot`'s ~3600 files land). Constraints on how
*the guest* resolves deep paths are a guest-syscall matter, not a populate one;
the harness's own sentinel sits at the root (`/thylacine-version`) for exactly
that reason.

### 6.3 Choosing ramfs vs pool

| Put it in the… | when the content is… |
|---|---|
| **ramfs** | needed before/at mount (init, drivers), tiny, or must be present on every fresh boot regardless of disk state |
| **pool** | large, persistent, user data, a toolchain, container bundles — anything that belongs on "the disk" |

---

## 7. The build → run → test loop

```bash
tools/build.sh all            # (re)build
tools/test.sh                 # boot + assert the banner (HVF on a capable host)
tools/run-vm.sh               # interactive dev VM (see docs/TOOLING.md §3)
```

Useful `test.sh` / `run-vm.sh` environment knobs:

| Env var | Effect |
|---|---|
| `THYLACINE_ACCEL=tcg` | Force full emulation (TCG) instead of HVF — the deterministic compat run. |
| `THYLACINE_CPU=cortex-a72` | Boot on an ARMv8.0-only core (the portability floor; `make test-a72`). |

Heavier gates (all in `CLAUDE.md`'s command list and the `Makefile`):

- `make smp-gate` — the SMP soundness matrix (single boots lie; multi-boot smp4/smp8 × default/UBSan).
- `make test-interactive` — the LS-CI expect/PTY interactive suite (login + assert rendered output).
- `make test-fault` — the deliberate-fault harness (proves the hardening protections actually fire).
- `make verify-kaslr` — the KASLR slide-varies witness (I-16).
- `make specs` — the TLA+ spec suite.

---

## 8. Gotchas (the ones that cost time)

- **`build.sh kernel` is `build.sh all`.** It re-bakes the pool from the ambient
  environment. If you had a chunk toggle set in a previous shell and not now,
  the pool changes under you. Set toggles explicitly per invocation.
- **The pool re-bake mints a fresh key.** Never reverse-sync or ship a `pool.img`
  without its paired `ramfs.cpio` (Section 1). A mismatch is `STM_EBADTAG`.
- **`THYLACINE_MKFS_PRESERVE=1` skips populate.** A preserved pool can be stale
  (missing a bundle a boot gate needs). If a boot fixture looks wrong, re-bake
  once with `PRESERVE=0`.
- **`disk` ≠ `pool`.** `disk.img` is a small scratch drive; `pool.img` is the
  real filesystem. `THYLACINE_DISK_SIZE` sizes the former only.
- **Verify by content, not by "it built".** The summary ledger and a `read`-back
  are the truth; a green exit with a cached step can leave old content in place.
- **Optional chunks skip silently.** If `/goroot` or `/clade` "isn't there" in
  the guest, check the summary — the input (`$GOFORK`, a staged clade) was
  probably absent, or the toggle was off.

---

## 9. Reference

- `docs/TOOLING.md` — the QEMU launcher, the 9P host share (hot-reload), snapshots, the agentic loop, the boot-banner ABI.
- `CLAUDE.md` "Build + test commands" — the canonical, always-current command list (the gates, their env vars, and why each exists).
- `tools/build.sh` — the source of truth; `build_ramfs()`, `build_stratum_pool_fixture()`, and `populate_stratum_pool()` are the three functions this document describes.
- `docs/DEBUGGING-PLAYBOOK.md` — the `STM_EBADTAG` / stale-key-pairing case study, among others.
