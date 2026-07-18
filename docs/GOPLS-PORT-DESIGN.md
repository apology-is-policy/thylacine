# GOPLS-PORT-DESIGN — porting `gopls` to Thylacine as a `GOOS=thylacine` cross-build

**Status: FOCUSED DESIGN (Stage 8d, opened 2026-07-18). Prose-validated per the
spec-to-code suspension; no new kernel surface, no new §28 invariant, no spec.**

This is the Stage-8d focused design pass for `GO-IDE-DESIGN.md §8` — the
editing-intelligence half of the Go IDE. Where 8c (`DELVE-PORT-DESIGN.md`) ported
Delve and *did* need a new kernel surface (the debug-fs), gopls needs **none**:
it is a pure-Go LSP server whose entire OS surface is file I/O + invoking the
already-shipping on-device `go` toolchain (`go/packages`) + a transport (LSP over
stdio — no `/net`). It ports like the toolchain (`GO-PORT-PLAN`) and like Ambush,
only more cleanly. This document records the port shape, the two cross-build
shims the ground-truth build surfaced, the runtime-integration model + risks, and
the sub-chunk plan.

---

## 1. Scope — what 8d lands, and what it does not

**8d lands:**

- The **gopls fork** (`~/projects/gopls`, base = upstream gopls **v0.21.1**),
  cross-building `GOOS=thylacine GOARCH=arm64 CGO_ENABLED=0` GREEN — a static
  aarch64 ELF baked into the image (`build_gopls`, mirroring `build_ambush`).
- The **two vendored build shims** the cross-build needs (§4).
- The **on-device runtime proof**: gopls's engine (`go/packages` load → parse →
  type-check → query) runs on the device against the baked `/goroot` toolchain,
  demonstrated through gopls's **offline CLI subcommands** (`gopls check` /
  `gopls definition` — the gopls analog of `ambush exec … bt/print`), a boot-fatal
  `/gopls-probe` (§6).

**8d does NOT land** (later sub-stages, unchanged from the charter):

- The **Nora LSP client** + the plugin architecture — **8e** (gopls speaks LSP
  over stdio; 8e drives it).
- The **Kaua editing UI** (completion popups, diagnostics gutter, hover) — **8f**.
- **No kernel change.** gopls adds no syscall, no Dev, no invariant. The kernel is
  byte-unchanged across the entire 8d arc (the SMP gate is therefore a formality,
  run once for the record).

---

## 2. Why gopls ports even more cleanly than Delve (the three facts)

1. **No OS-specific backend.** Delve's native backend is ptrace/mach-shaped and
   needed ~4-5 build-tagged `proc_thylacine` files over the debug-fs. gopls has
   **no** OS backend: it reads files with `os`, walks the workspace, and shells
   out to `go`. The only GOOS-specific code in its dependency closure is two tiny
   file-fallback shims (§4) — *both in vendored deps, none in gopls itself.*
2. **The transport is stdio, not a socket.** LSP runs over stdin/stdout
   (`gopls serve` reads JSON-RPC framed messages on fd 0/1). Thylacine has pipes
   and stdio; **no `/net` is required** (unlike the real-socket `ambush dap`, which
   is deferred to 8e for exactly this reason). A future socket mode
   (`gopls -listen`) is a v-next nicety, not a v1.0 need.
3. **The toolchain is already on-device.** gopls's heavy lifting is delegating to
   `go list` (via `go/packages`) to resolve packages + read export data. That
   toolchain is the Stage-6 **by-default** `/goroot` — the exact dependency the
   Nora format-on-save + the on-device `go build` already exercise. gopls is a
   *client* of a capability we already proved.

The consequence: 8d is almost entirely **assembly + a runtime proof**, with the
compile half already de-risked (§4, §7). The genuinely new question is not "does
it build" but "does `go/packages` drive the on-device toolchain correctly under
gopls" — which §6 proves.

---

## 3. The runtime contract — how gopls reaches the OS

gopls's server loop (`internal/cmd` → `internal/server` → `internal/cache`):

| gopls need | Mechanism | Thylacine substrate |
|---|---|---|
| Read/parse source | `os.ReadFile`, `go/parser` (pure Go) | the pool FS via 9P (Larder-cached) |
| Load package graph | `go/packages` → `go list -json -compiled -deps` | `exec.Command("go", …)` → `/goroot/bin/go` |
| Type-check | `go/types` + export data (pure Go) | in-process, no OS surface |
| Resolve `GOROOT`/env | `go env` via `gocommand` | the baked `/goroot`, `GOROOT`/`PATH` env |
| Watch on-disk changes | LSP `didChangeWatchedFiles` **from the client** | the client's (Nora, 8e) job; gopls does not fsnotify in the LSP path |
| Atomic file identity | `robustio.getFileID` | the 9P **qid.path** (§4) |
| Transport | LSP/JSON-RPC over stdio | pipes / stdio (no `/net`) |

**The single load-bearing runtime dependency is `go/packages` invoking `go`.**
gopls finds `go` on `PATH` (via `os/exec.LookPath` inside `gocommand`), so the
on-device environment must place `/goroot/bin` on `PATH` and set `GOROOT=/goroot`
(the driver otherwise infers GOROOT from its own binary location, but gopls sets
the env explicitly for `go list`). Writable `GOCACHE` / `GOPATH` / `GOMODCACHE` /
`TMPDIR` in the pool complete the picture — identical to what an on-device
`go build` already needs.

**File watching is not gopls's job in the LSP flow.** The editor client
(`workspace/didChangeWatchedFiles` + `textDocument/didOpen`/`didChange`) tells
gopls what changed; gopls re-runs `go/packages` on demand. So no fsnotify / kqueue
port is required — the one OS-integration surface that would have been hard on
thylacine is architecturally the *client's* responsibility (8e).

---

## 4. The cross-build shims (ground truth, 2026-07-18)

`GOOS=thylacine GOARCH=arm64 CGO_ENABLED=0 go build ./...` surfaced exactly **two**
gaps, both the same class: **thylacine is NOT in Go's `unix` build tag** (the fork
uses `_thylacine`-suffixed files rather than piggybacking on `unix`), so any
dependency whose platform files are `//go:build unix` with *no generic fallback*
leaves its symbols undefined for thylacine. Only two deps in gopls's closure do,
and both are fixed with a vendored build-tagged file — the Ambush precedent
(`vendor/.../isatty_thylacine.go`). Neither is in gopls itself.

1. **`golang.org/x/telemetry/internal/mmap`** — `mmap_unix.go` is `//go:build unix`
   and `mmap_other.go` is `(js && wasm) || wasip1 || plan9`, so neither defines
   `mmapFile`/`munmapFile` for thylacine. **Fix:** `mmap_thylacine.go` = the pure-Go
   `io.ReadAll` fallback the other non-mmap platforms use. Correct because thylacine
   has no userspace file mmap and on-device telemetry is offline (no upload, no
   cross-process counter sharing) — the counter file is just read, never mapped.
2. **`golang.org/x/tools/internal/robustio`** — `robustio_posix.go`
   (`//go:build !windows && !plan9`, a *negative* tag that *does* match thylacine)
   reads `stat.Dev`/`stat.Ino`, which the fork's reduced `syscall.Stat_t` lacks.
   **Fix:** exclude thylacine from the posix tag (`… && !thylacine`) and add
   `robustio_thylacine.go` whose `getFileID` uses **`stat.QidPath`** — the 9P
   qid.path, the inode-equivalent, a *real* per-file identity (unique within a
   served tree), not a path-string fallback. `fi.Sys()` returns
   `*syscall.Stat_t` on the fork (verified against `os/stat_thylacine.go`).

Both shims live in the fork's `vendor/` tree (offline reproducible build, the
Ambush model). Result: `go build ./...` GREEN across **every** package, `go vet`
clean, a 36 MB static aarch64 ELF with debug_info.

**Porting-risk note (recorded for future Go-program ports):** the "thylacine ∉
`unix`" property means each new ported Go program should be cross-build-swept for
`//go:build unix`-only deps. The generic-fallback deps (`os`-only, or with an
`_other.go` that includes thylacine) are free; the ones that gap are always a
one-file vendored shim. The alternative — adding thylacine to the fork's `unix`
set — is **rejected**: it would pull a large body of `unix`-tagged stdlib into the
thylacine build that assumes syscalls the kernel does not have, destabilizing the
toolchain the whole arc depends on. Per-dep shims keep the fork stable.

---

## 5. What 8d deliberately does not touch

- **No kernel surface.** No syscall, Dev, or ABI. The debug-fs (I-39) is Ambush's;
  gopls consumes only the filesystem + `os/exec` + stdio, all pre-existing.
- **No new §28 invariant, no spec.** gopls is a userspace port; its correctness is
  execution-verified (the on-device E2E) + the arc-close robustness holotype over
  the port shims + the probe, not a formal model.
- **No fork toolchain change.** The two shims are vendored-dep files; the go
  toolchain fork is byte-unchanged (contrast: some ports needed fork syscall
  additions). This keeps 8d orthogonal to the toolchain arc.

---

## 6. The on-device E2E — the gopls engine over the baked toolchain

The 8c analog of `ambush exec … break/continue/bt/print` is gopls's **offline CLI
subcommands**: they run the *full* engine (`go/packages` load → type-check →
query) on a file and print a result, needing **no LSP client**. This isolates the
one real runtime risk (`go/packages` driving `/goroot`) into a deterministic,
boot-fatal probe.

`/gopls-probe` (native Rust, mirroring `/ambush-probe`):

1. Materialize a tiny module in a writable pool dir: `go.mod` (`module p` / `go
   1.25`) + a `.go` file with a known symbol and a deliberate reference (e.g. a
   `const Sentinel = <known>` and a func that returns it).
2. Set the env `go/packages` needs: `GOROOT=/goroot`, `PATH=/goroot/bin:…`,
   writable `GOCACHE`/`GOPATH`/`GOMODCACHE`/`TMPDIR`, `GOFLAGS=-mod=mod`,
   `GOTOOLCHAIN=local` (never fetch a toolchain), `GOPROXY=off` (offline).
3. Drive two subcommands, asserting output:
   - `gopls check <file.go>` on a file with a **deliberate diagnostic** (an
     unused variable or a type error) → gopls prints the expected diagnostic
     (proves load + type-check).
   - `gopls definition <file.go>:<line>:<col>` on the symbol → gopls prints the
     definition's file:line (proves the query path + position mapping).
4. Boot-fatal: joey spawns + reaps + asserts exit 0 (the `/ambush-probe` pattern).

This proves the substantive claim — **the gopls engine works on-device** —
without the 8e transport. Expect iteration: `go/packages`'s exact env
requirements, `go list` behavior on the pool FS, and gopls's workspace-detection
heuristics are where the runtime surprises live (the "ground truth over theory"
discipline: instrument, do not theorize past two contradictions).

**Deferred to 8e** (needs the Nora side): a full LSP round-trip
(`initialize`/`didOpen`/`completion`/`hover`/`definition` over stdio). The
in-process-`net.Pipe` trick that proved the DAP round-trip (8c-4b) is available
as a fallback if an LSP-transport proof is wanted before Nora lands, but the CLI
subcommands are the more direct engine proof and the LSP transport is Nora's
contract to exercise.

---

## 7. Build + transport mechanics

- **`build_gopls`** in `tools/build.sh` mirrors `build_ambush`: a `GOPLSFORK` env
  (default `~/projects/gopls`), `GOOS=thylacine GOARCH=arm64 CGO_ENABLED=0
  go build -mod=vendor -ldflags="-s -w" -o $go_out/gopls ./` — self-contained
  (vendored), skips gracefully if the fork is absent (so a fresh checkout still
  boots, `/gopls-probe` SKIPs).
- **Bake**: `gopls` joins the `go_bins` ramfs list (the binary is ~36 MB; strip
  keeps it lean). It lands in the default image alongside `ambush` + the `go`
  toolchain.
- **The `/goroot` dependency is already satisfied** — Stage 6 bakes it by default
  (`build_go_goroot` / `THYLACINE_BAKE_GOROOT`). gopls needs no new bake; it reads
  the same tree the on-device `go build` does.
- **Transport**: stdio for the LSP server (`gopls serve`, driven by Nora at 8e);
  the CLI subcommands (8d-2 E2E) read a file path + print to stdout.

---

## 8. Sub-chunk plan

- **8d-0** — this design pass (scripture; the cross-build ground truth + the
  runtime model + the plan). **DONE this commit.**
- **8d-1** — the fork (stock v0.21.1 → vendor + the two shims → cross-build GREEN)
  + `build_gopls` + the `go_bins` bake. The compile half is de-risked (§4). The
  fork commits are `8e1192d` (stock) + `34e2bfc` (vendor + shims), USER-TO-PUSH.
- **8d-2** — the on-device runtime E2E (`/gopls-probe`: `gopls check` +
  `gopls definition` over `/goroot`, boot-fatal). The substantive validation;
  expect boot iterations on the `go/packages` env + workspace detection.
- **8d-3** — the arc close: a focused robustness holotype over the port (the two
  shims + the probe + the env wiring — a lighter, no-kernel-surface review, the
  8c-4c client-side-audit shape) + docs (`docs/reference/NN-gopls.md`) + the SMP
  gate (kernel byte-unchanged → a formality) + the `GO-IDE-DESIGN §8` as-built
  flip.

---

## 9. Signoff items

None requiring a user vote. gopls is a userspace port with no kernel surface, no
ABI, no invariant, and no scripture-altering decision. The fork base (v0.21.1) is
forced by the toolchain (the newest gopls whose go.mod `go` directive is `1.25`,
matching go-thylacine go1.25.3; v0.22+ require go 1.26.0). The naming is settled —
gopls keeps its upstream name (it is *Nora's Go plugin's* language server, per
`GO-IDE-DESIGN §10`; only the debugger is renamed Ambush).

---

## 10. Ground-truth appendix (2026-07-18)

- Fork base: `golang.org/x/tools/gopls@v0.21.1` (go.mod `go 1.25`; v0.22.0+ require
  `go 1.26.0`, incompatible with the go-thylacine go1.25.3 host).
- `~/projects/gopls`: `8e1192d` (stock v0.21.1) → `34e2bfc` (`go mod vendor` +
  the two thylacine shims). 26 MB vendor tree, 1236 vendored `.go` files.
- Cross-build: `GOOS=thylacine GOARCH=arm64 CGO_ENABLED=0 go build -mod=vendor
  ./...` GREEN across every package; `go vet ./` clean; `/tmp/gopls-thyla` = a
  36 MB static aarch64 ELF (`ELF 64-bit LSB executable, ARM aarch64, statically
  linked, with debug_info`).
- The two shims: `vendor/golang.org/x/telemetry/internal/mmap/mmap_thylacine.go`
  (io.ReadAll fallback) + `vendor/golang.org/x/tools/internal/robustio/
  robustio_thylacine.go` (getFileID via `stat.QidPath`) + the one-token tag
  exclusion in `robustio_posix.go`.

---

## 11. References

- `docs/GO-IDE-DESIGN.md §8` (the staged plan; 8d = this arc).
- `docs/DELVE-PORT-DESIGN.md` (the sibling port; the methodology template — the
  fork/vendor/shim/build.sh pattern + the boot-fatal probe).
- `docs/GO-PORT-PLAN.md` (the toolchain arc; the `/goroot` bake gopls consumes).
- `docs/KAUA.md` (the TUI substrate the 8f gopls UI builds on).
- `docs/reference/132-larder.md` (the guest FS cache gopls's file reads ride).
