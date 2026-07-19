# 137 — gopls (the Go LSP engine) on Thylacine

**Status**: as-built at Go-IDE Stage **8d** (arc COMPLETE 2026-07-18). A
`GOOS=thylacine` userspace **port** of gopls (the Go language server), the
editing-intelligence half of the Go IDE (NOVEL #13, `docs/GO-IDE-DESIGN.md`).
Design: `docs/GOPLS-PORT-DESIGN.md`. **No kernel surface** — gopls is pure Go
(file I/O + the on-device `go` toolchain via `go/packages` + LSP over stdio; no
`/net`). The kernel is byte-unchanged by 8d; the only Thylacine-side code is a
joey boot probe + a `t_chdir` libt wrapper.

## Purpose

gopls provides the editing intelligence a Go IDE needs — diagnostics,
definition/references, hover, completion, rename, format — over the standard
**LSP** protocol. On Thylacine it is a cross-compiled fork (`~/projects/gopls`,
base gopls v0.21.1 — the newest whose `go.mod` `go` directive is `1.25`,
matching the go-thylacine `go1.25.3`) baked into the pool at
`/goroot/bin/gopls` beside `go`/`gofmt`. Stage 8d proves the **engine** runs
on-device via gopls's offline CLI subcommands (`gopls check`, `gopls
definition`) — the LSP client (Nora) is 8e, the Kaua editing UI is 8f.

## The fork (`~/projects/gopls`, USER-TO-PUSH)

Three deltas over stock v0.21.1:

1. **Two vendored build-fallback shims** — thylacine is **not** in Go's `unix`
   build tag, so deps with only `//go:build unix` platform files and no generic
   fallback leave a gap. gopls's dependency closure has exactly two:
   - `vendor/golang.org/x/telemetry/internal/mmap/mmap_thylacine.go` — an
     `io.ReadAll` fallback for `mmapFile` (no mmap syscall in the port's needs).
   - `vendor/golang.org/x/tools/internal/robustio/robustio_thylacine.go` —
     `getFileID` via `os.Stat().Sys().(*syscall.Stat_t).QidPath` (the 9P qid is
     the inode-equivalent), plus excluding thylacine from `robustio_posix.go`'s
     tag.

2. **Telemetry disabled** (`main.go`) — `telemetry.Start(Config{ReportCrashes:
   false, Upload: false})`. See "Telemetry" below.

3. Nothing else — every other package cross-builds clean.

## The env-requirement chain (the load-bearing 8d-3 finding)

gopls runs correctly on Thylacine **only when its process env provides two
things**. Both are supplied to real users by `login` (which stamps the session
with `SHELL_CAPS = CAP_LOCK_PAGES | CAP_CSPRNG_READ` and seeds
`PATH=/bin:/goroot/bin`); a boot-probe / non-login spawn must supply them
explicitly.

1. **`CAP_CSPRNG_READ`** — gopls seeds a trace-ID generator at init
   (`golang.org/x/tools/internal/event/export.initGenerator` reads 8 bytes from
   `crypto/rand`). On Thylacine `crypto/rand` → `SYS_GETRANDOM`, which is
   `CAP_CSPRNG_READ`-gated (`kernel/syscall.c::sys_getrandom_handler`). Without
   the cap the syscall returns `-1`, which Go reads as `EPERM`, and
   `crypto/rand.Read` treats **any** error as a **fatal** (`crypto/rand: failed
   to read random data … operation not permitted`) — gopls aborts before doing
   any work. This is *not* a gopls or kernel bug; it is the capability model
   working as designed. Login grants the cap (crypto is a normal user need —
   TLS, ssh, …), so real users are unaffected.

2. **`PATH`** (containing the dir with `go`) — gopls resolves the `go` binary
   via `exec.LookPath($PATH)` to load a workspace **view** (`go/packages` shells
   out to `go list`). With no PATH, gopls reports **`no views`** (`failed to
   load view … go command required, not found: exec: "go": executable file not
   found in $PATH`). PATH is also the fallback `os.Executable()` uses (the Go
   fork routes thylacine to `executable_path.go`, the `os.Args[0]`+PATH
   resolver, not `/proc/self/exe`). Login seeds `PATH=/bin:/goroot/bin`.

A gopls invocation additionally needs its **cwd** to be (or be under) the module
directory — `gopls check`/`definition` derive the workspace folder from
`os.Getwd()` (`internal/cmd/cmd.go`). The interactive `go8d.exp` `cd gd`s first;
the boot probe `t_chdir`s into the module.

## Telemetry (disabled — the private-OS posture)

Stock gopls `main.go` calls `telemetry.Start(Config{ReportCrashes: true,
Upload: true})`. `Upload: true` periodically phones home to `telemetry.go.dev`
— undesirable on a private OS and network-dependent. `ReportCrashes: true`
**unconditionally** spawns a re-exec'd telemetry **sidecar** (a second gopls
process; needs `os.Executable`) plus a `crashmonitor.Parent` /
`debug.SetCrashOutput` crash-output pipe. The fork sets **both false**:
`parent()`'s `if reportCrashes || childShouldUpload { startChild(...) }` then
never fires — no sidecar, no upload, no crash-output pipe. This is the correct
Thylacine posture regardless of any other consideration, and it retires the
os.Executable-dependent sidecar that the leading teardown-segv hypothesis
pointed at (see below).

## The teardown segv (task #98) — disposition

Stage 8d-2 observed, in the interactive login session, a **non-blocking**
`snare:segv` at a high user VA (~`0x15aeb4400`) *after* a successful `gopls
check` printed its diagnostic. 8d-3 chased it:

- The gopls env-requirement chain above was root-caused to ground (kernel
  handler + the Go fork's `executable_path.go`).
- With the **full env** (cap + PATH + cwd), gopls fully runs in a
  transport-free boot probe — view loads, `check` reports the exact diagnostic,
  `definition` resolves — and **exits cleanly (status 0), with NO segv**, under
  telemetry **on** (boot7) *and* off (boot8). The teardown segv did **not**
  reproduce in a controlled full-env boot.
- Conclusion: the segv was interactive-env-specific / non-deterministic. The
  telemetry-disable fix removes the leading suspect (the sidecar/crashmonitor
  teardown machinery) as a belt-and-suspenders on top of its posture merit. If
  it recurs post-fix it is a fresh hunt.

## The Thylacine-side code

- **`usr/joey/joey.c`** — the gopls boot probe (inside the go4c block, gated on
  `/goroot/bin/gopls`). Writes a tiny module at `/tmp/gp` (`go.mod` + a
  `check.go` with a `Target` definition + a reference + an undefined ident,
  idempotently), sets `PATH=/bin:/goroot/bin` + drops go4c's `GO111MODULE=off`
  in `/env`, `t_chdir`s into the module, then spawns (with
  `CAP_CSPRNG_READ | CAP_LOCK_PAGES`, mirroring `SHELL_CAPS`):
  - `gopls check /tmp/gp/check.go` → `check.go:7:9-23: undefined: undefinedIdent`
  - `gopls definition /tmp/gp/check.go:5:9` → `check.go:3:6-12: defined here as
    func Target() int`

  Both exit 0 on success (a "no views" / crypto-rand fatal / missing-env failure
  exits non-zero); the probe is **boot-fatal** on either status != 0 and logs
  `joey: go8d OK …` on success. This is the **deterministic, transport-free**
  gopls gate.
- **`usr/lib/libt/include/thyla/syscall.h`** — a `t_chdir(path, len)` wrapper
  over `SYS_CHDIR` (69), previously unwrapped in libt.
- **`tools/interactive/go8d.exp`** — the LS-CI "gopls over a real login console"
  leg. Trimmed to the two transport-reliable assertions — gopls resolves via the
  login `$PATH` (`which gopls`) and execs (`gopls version`). The heavy engine
  check is NOT driven through the console: gopls's check output floods the macOS
  pty transport under ut's per-keystroke redraw storm (the **#66** class —
  guest-exonerated: the boot probe, `Thylacine boot OK`, and login all succeed
  when the console channel drops). The engine is the boot probe's job.

## Tests

- **Boot probe** (`joey: go8d OK`) — deterministic, every boot; boot-fatal.
  Proves `go/packages` → on-device `go list` → type-check → diagnostic, plus a
  `definition` query.
- **`tools/interactive/go8d.exp`** — resolves-via-login-PATH + execs, over a
  real PTY (best-effort; #66 host-transport tolerated by the harness's bounded
  retry).

## Known caveats

- **filecache "operation not permitted"** (task #99, non-fatal): on a fresh
  `HOME/.cache`, gopls's persistent-index filecache logs `create …-cas:
  operation not permitted` storing xref/index data. Root-caused (the earlier
  `O_EXCL` guess is refuted): filecache `Set` uses `os.OpenFile(O_WRONLY|
  O_CREATE, 0600)` — **no** `O_EXCL` — so Go's open-or-create does `SYS_OPEN`
  (returns proper `-T_E_NOENT`, falls through) then `syscall.Create` →
  `SYS_WALK_CREATE`, which returns a **bare `-1`** that Go reads as
  `Errno(1)`=EPERM (the go-fork `#102` legacy-errno wart), wrapped `Op:"create"`.
  The `-1` is one of two unclamped sites in `sys_walk_create_handler` —
  `perm_check(parent, W|X)` (`syscall.c:2886`, newly live since the dev9p
  `perm_enforced=true` A-3b flip; the `:2879` "deferred to A-3" comment is now
  stale) or `dev9p_create→NULL` (`dev9p.c:2989`/`:1299`, which collapses any
  Tlcreate errno to NULL, unlike `dev9p_stat_native` which propagates it). The
  differentiator from go-build's working creates: gopls reads-before-create and
  always reaches `SYS_WALK_CREATE` for a fresh file. gopls degrades gracefully
  (re-computes; check + definition still exit 0). Fix (owed, budgeted session):
  instrument to pin the site + underlying cause on a fresh-pool boot, fix it, and
  propagate the real errno through the create path. Audit-bearing (FS-mutation
  surface). Correctness is unaffected.
- **robustio `FileID` cross-dataset collision** (task #100, 8d-3 holotype F2 —
  was a P2 latent, now **RESOLVED**). **The proper devno ABI fix landed**:
  `t_stat` grew 80→88 with a `devno` field (Plan 9 Chan.dev / POSIX st_dev),
  filled from `spoor->devno` on both the SYS_FSTAT (`spoor_stat_native`) and
  SYS_STAT (pounce fused-query, `stalk.c`) paths; the Go fork's `syscall.Stat_t`
  grew in lockstep (as did libt `t_stat`, libthyla-rs `Metadata` + `dev()`, and
  the pouch patches 0010/0019/0021), and the shim now returns
  `FileID{device: uint64(stat.Dev), inode: stat.QidPath}`, matching
  robustio_plan9.go. The earlier FNV-of-path interim (gopls `bd329f6`) is
  superseded. Verified: 1164/1164 + boot OK + `go8d OK` + the pouch stat/fstat
  agreement (`fields == fstat`). A build-time lesson worth keeping: the
  per-mirror `_Static_assert(sizeof == 80)` checks only each mirror's *own* size,
  not that it matches the kernel — a stale mirror left at 80 passes its assert
  yet overflows at runtime (patches 0019 + 0021 were caught only by the boot
  segv + a `struct t_stat` grep, not the build). The original `device: 0`
  hazard, for the record: the shim returned `FileID{device: 0, inode:
  stat.QidPath}`, and qid.path is unique only *within one served tree*; a real
  login session mounts two independently-inode-numbered
  Stratum datasets (the system dataset — `/goroot`, `/bin` — and the per-user
  home). With `device: 0`, a cross-dataset qid.path collision + a same-second
  mtime (the `/goroot` bake writes thousands of files in one burst) makes
  gopls's memoized FS serve file A's bytes under file B's URI (fs_memoized.go
  alias branch) → silent wrong content. Fail-DANGEROUS, unlike the plan9
  precedent (which populates `device` from the server identity — the same
  per-server qid scope Thylacine has). The shim can't do better today: the
  80-byte `t_stat` surfaces no device identity (`Spoor.devno` exists — stalk-2 —
  but is unexported). Fix (ABI, user signoff): append `devno` to `t_stat`
  (80→88, the A-2a 72→80 precedent) and fold it into the shim, matching plan9.
  Narrow stochastic trigger (double collision) → P2, not P1; the boot probe's
  single-dataset config can never hit it.
- **The boot-probe gate is exit-status-based** (8d-3 holotype F5): `gopls check`
  exits 0 *with* diagnostics present, so `st_gp == 0` proves the engine loaded a
  view and ran to completion, not that a specific diagnostic was emitted. The
  diagnostic + the `definition` resolution DO appear in the boot log
  (human-verifiable), and the `definition` leg (resolving 5:9→3:6) requires the
  package to have been type-checked — so the engine is genuinely gated; only the
  diagnostics-*reporting* string is unasserted (a future leg could pipe the
  child's stdout and scan it).
- The offline CLI subcommands exercise the full engine but not the LSP transport
  — that is Stage 8e (the Nora client).

## Naming rationale

gopls keeps its upstream name (a standard tool driven over a standard protocol,
so knowledge and the Nora client stay portable) — the same discipline as the
`ambush` debugger keeping `dlv`'s CLI shape.
