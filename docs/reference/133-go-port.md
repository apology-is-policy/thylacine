# 133 — The Go port (GOOS=thylacine): capability map

**Status**: as-built through GO-PORT-PLAN Stage 5 (modules over the net, landed
2026-07-14). The fork lives at `~/projects/go-thylacine` (go1.25.3 base,
`GOFORK` in `tools/build.sh`); ~54 `*_thylacine*` files across runtime /
syscall / os / net / crypto / cmd-go. This page is the single map of **what
runs on-device, at which proof tier, and where the seams are**.

Proof tiers used below:

- **PROVEN** — exercised by a boot-fatal probe, a CI scenario, or a landed
  measured arc (the CHASE built `cmd/gofmt` on-device thousands of times).
- **EXPECTED** — the mechanism is fully present and its substrate is proven,
  but no dedicated probe drives it yet.
- **STUBBED** — deliberately returns `ENOSYS` (or is a no-op); listed so a
  consumer hitting it knows it is a port seam, not a bug.

## 1. Purpose

Thylacine runs the **real Go toolchain natively on-device**: the `go` driver,
compiler, linker, assembler and the full stdlib source live in the pool at
`/goroot` (Stage 4b bake), and `go build` compiles + links real programs inside
the guest (Stage 4c; the CHASE's `go build cmd/gofmt` bench). Stage 5 closes
the loop: the toolchain **pulls modules off `/net`** (proxy protocol over Go's
own net/http + TLS) and Go programs fetch from the real internet.

## 2. Runtime substrate (PROVEN)

| Mechanism | Realization | Proof |
|---|---|---|
| Process start | `rt0_thylacine_arm64.s` → `SYS_SPAWN_FULL_ARGV` ELF, REVENANT file-backed exec | every probe |
| Threads (M) | `SYS_THREAD_SPAWN` + TPIDR_EL0 g-pointer; multi-M GC | `go-goroutines` |
| Futex | `torpor` wait/wake (`SYS_TORPOR_WAIT/WAKE`) | every probe; the #343 churn fix |
| Clocks | vDSO page (`AT_VDSO_CLOCK`) for nanotime/walltime; `SYS_CLOCK_GETTIME` fallback | vDSO arc re-measure |
| Memory | `SYS_BURROW_ATTACH_LAZY` demand-zero + decommit (`mem_thylacine.go`); stock `heapAddrBits=48` | overcommit arc |
| Yield | `SYS_YIELD` (#33) | scheduler tests |
| Env | `goenvs` reads the per-Proc `/env` device; children inherit the KERNEL env (`env_clone_into`) — `os.Setenv` is process-local (plan9 model), `ProcAttr.Env` is deliberately dropped; **to change what children see, write `/env/KEY`** (os.WriteFile; devenv honors OTRUNC) | `go-env`, `go-get` |
| Hardware crypto | `AT_HWCAP` → `internal/cpu` (AES/PMULL/SHA2/...) | AEAD-lever bench |
| Entropy | `sysrand` → `SYS_GETRANDOM` (kernel ChaCha20 CSPRNG) | TLS handshakes |
| Signals | `os/signal` receiver loop wired (notes → signal channel) | `signal.Stop` livelock fix |
| os/exec | `SYS_SPAWN_FULL_ARGV` + pipes + `SYS_WAIT_PID`; `ProcAttr.Dir` honored; **`capMask = ^0` (inherit)** — pre-Stage-5 the zero-value record spawned every Go child **capability-naked** (parent & 0), invisible until the first entropy-needing child: cmd/go's module-fetch TLS hit the `CAP_CSPRNG_READ` gate on `SYS_GETRANDOM` and died `crypto/rand: ... operation not permitted` (a bare `-1` renders as EPERM) | `go-exec`; the toolchain itself; `go-get` (the fix's witness) |
| netpoll | **the plan9 blocking stub** — net I/O parks an M per op (`entersyscall`); no readiness integration | `go-net` |

## 3. The net package (plan9-shaped over `/net`)

`netFD{ctl,data,listen *os.File}` against netd's `/net`; names resolve via
`queryCS1` → `/net/cs` (numeric → ndb → DNS through slirp's host resolver).

| Surface | Tier | Note |
|---|---|---|
| TCP dial / listen / accept / Read / Write | **PROVEN** | `go-net` loopback round-trip (boot-wired); `go-web` external |
| DNS + cs resolution | **PROVEN** | via netd; IPv4 A-records only (netd v1.0) |
| Resolver ORDER (`net/conf.go`) | **PROVEN** (fixed at Stage 5) | GOOS=thylacine rides the plan9 gates: `goosPrefersCgo()` + the resolv.conf/nsswitch skip + the `mustUseGoResolver` guard all list thylacine, so hostname lookups take the cs-based system resolver. Pre-fix, the FIRST hostname lookup ever attempted from Go on-device (go-web) fell into the unix dnsclient, which defaults to `127.0.0.1:53` when `/etc/resolv.conf` is absent — dead air on the guest lo → `lookup: i/o timeout`. `go-net` never caught it (numeric 127.0.0.1 dial, no hostname). `GODEBUG=netdns=go` without a custom `Resolver.Dial` is refused, plan9-style. |
| `net/http` client, HTTPS end-to-end | **PROVEN** | `go-web`: DNS→TCP→TLS1.3→verified chain→HTTP (go5.exp) |
| UDP core (Dial/ReadFrom/WriteTo) | EXPECTED | `/net/udp` exists (netd); no Go probe yet |
| Deadlines (`SetDeadline` family) | **partial** | checked at op ENTRY; **do not abort an in-flight blocked call** (`fd_thylacine.go`). A responsive peer behaves; a wedged peer wedges the goroutine. Cancellation-by-close is the plan9-style seam. |
| UDP msg variants (`ReadMsgUDP` etc.) | STUBBED | `ENOSYS` |
| Raw IP (`iprawsock`) | STUBBED | `ENOSYS` (no raw-IP surface in netd) |
| Unix sockets (`net.UnixConn`) | STUBBED | `ENOSYS` (native srv-streams are not BSD sockets; pouch has the AF_UNIX shim) |
| TCP sockopts (keepalive etc.) | STUBBED | `ENOSYS` — smoltcp defaults apply |
| `net.Interfaces` | STUBBED/minimal | `/net/ipifc` exists; not surfaced |

## 3a. The T_WSTAT_SIZE lift (ftruncate — landed with Stage 5)

`go mod`'s module cache rewrites `.ziphash`/lock files in place via
`lockedfile`, which truncates — so Stage 5 needed a real `ftruncate`. The
kernel gained a fourth `SYS_WSTAT` axis, **`T_WSTAT_SIZE`** (== the 9P
`Tsetattr` SIZE bit; shrink discards, extend zero-fills, Stratum-side
`stm_fs_truncate`) delivered on the existing `x5=size` register:

- **Rights model** — SIZE is a **content** axis, unlike the #47
  kind-gate-only metadata axes: it requires `RIGHT_WRITE` on the fd (the
  POSIX write-opened-fd rule) and **skips** `perm_wstat_check` (the
  identity gate stays the sole authority for mode/uid/gid). A combined
  `SIZE|MODE` call is still adjudicated by the mode policy. The `s64`
  offset domain is bounded (`> INT64_MAX` → -1). A **CWALKONLY (O_PATH)
  handle is rejected** for SIZE (the #81 class): O_PATH is born `R|W` but
  perm_check-exempt at open, so its `RIGHT_WRITE` is hollow — a truncate
  through it would bypass the write-permission check. (The metadata axes
  are unaffected: `perm_wstat_check` is their real authority regardless of
  the handle's hollow rights.)
- **Cache coherence** — a size change invalidates the Larder attr **and**
  page caches for that qid (whole-file drop, the OTRUNC discipline).
- **Fork side** — `Ftruncate` = `SYS_WSTAT(T_WSTAT_SIZE)` behind the
  Larder-served fstat no-op fast path (cmd/go's `putIndexEntry` truncates
  every cache write to its own size — kept cheap); `Truncate(path)` opens
  `O_WRONLY` first (the write-open is the POSIX W-permission check).

Tests: `dev9p.wstat_size` (the per-axis gate truth table + the u64 wire
width) + `perm.wstat_rejects_unknown_valid_bit` (SIZE passes the
self-defend mask untainted).

**The Loom (async) twin** — the `T_WSTAT_SIZE` audit found the same O_PATH
truncate bypass on the `LOOM_OP_SETATTR` submit path (`kernel/loom.c`),
plus a broader gap: the async path ran **no** identity check on chmod/chown.
The async submit cannot evaluate the sync path's owner-only
`perm_wstat_check` without a blocking owner-stat, so the fix splits by
authority kind: **SIZE (authority = the fd's RIGHT_WRITE) stays** on a
non-CWALKONLY handle with the s64 bound; **MODE/UID/GID (authority =
identity) are rejected fail-closed** — v1.0 Loom SETATTR is truncate-only.
Async identity-setattr (a submit-stat or a completion-recheck design) is a
v1.x seam. Regression: `9p_client.loom_setattr_e2e` (chmod rejected + never
on the wire / truncate reaches the wire / O_PATH truncate rejected).

## 4. crypto / TLS (PROVEN by Stage 5)

- `crypto/tls` is pure Go; AES-GCM + SHA rides the AT_HWCAP hardware path.
- `crypto/x509` roots: `root_thylacine.go` reads, in order,
  `/etc/ssl/certs/ca-certificates.crt` (**the system bundle** — the host-baked
  Mozilla set, NET-DESIGN s9, shared with the native Rust TLS stack),
  `/lib/tls/ca-bundle.pem`, `/etc/ssl/cert.pem`; `SSL_CERT_FILE` overrides.
  Absent any bundle: empty non-nil pool → verification fails closed.
- Chain validity needs the wall clock: PL031 RTC anchor (LS-K).

## 5. The on-device toolchain

**Layout**: `/goroot` (bake: `THYLACINE_BAKE_GOROOT=1`, `build_go_goroot`) =
cross-built `bin/go` + `pkg/tool/thylacine_arm64/{compile,link,asm,...}` +
`pkg/include` + trimmed stdlib **source** (compiled on-device on demand) +
`go.env` + timezone db. `/go-cache` is a bake-time seed-warmed GOCACHE;
`/go4c` holds the boot-probe source.

**Env contract** (all via `/env`): `GOROOT=/goroot`, writable `GOCACHE` /
`GOPATH` / `GOTMPDIR` / `TMPDIR` / `HOME`, `GOENV=off`, `GOTELEMETRY=off`.
`go.env` pins **`GOTOOLCHAIN=local`** — a thylacine `go` can never exec a
foreign downloaded toolchain, so auto-switching is disabled at the root.
The boot probes run as SYSTEM with `GOPROXY=off` + `GO111MODULE=off` (hermetic;
joey's env is inherited by the login session, so module work must override —
the `go-get` driver does).

| Command | Tier | Proof |
|---|---|---|
| `go version`, `go env` | **PROVEN** | go4c boot probe (every boot) |
| `go build` (stdlib compile + link + run result) | **PROVEN** | go4c boot probe; the CHASE gofmt bench (S1/S3) |
| `go mod tidy` / module download (proxy protocol, sumdb verify, in-place truncate of `.ziphash`) | **PROVEN** | `go-get` driver via go5.exp |
| `go build` against a downloaded module + `go version -m` | **PROVEN** | `go-get` driver |
| `go run`, `go vet`, `go test`, `go fmt`, `go list` | EXPECTED | mechanisms (exec, pipes, tmp, cache) all proven; no dedicated probe |
| `go get` (upgrading go.mod in place) | EXPECTED | same substrate as tidy |
| Race detector (`-race`) | not available | needs TSan runtime — no thylacine port |
| cgo | not available | `CGO_ENABLED=0` everywhere by construction |

## 6. Stage 5 probes (this page's landing)

- **`/bin/go-web [URL]`** — net/http GET; prints status, byte count, proto,
  TLS version + verified-chain depth, the page `<title>`, and
  `go-web: STAGE 5 (net/http fetch) OK`. External-network dependent — never
  boot-wired.
- **`/bin/go-get [WORKDIR]`** — the module workflow driver: writes an embedded
  demo project (`thyla.dev/go5demo`, importing `github.com/google/go-cmp`),
  sets the module env by **writing `/env/KEY` files** (the G15 child-inherit
  mechanism), then `go mod tidy` (downloads from `https://proxy.golang.org`,
  sumdb-verified — no `,direct` fallback: no git on-device, fail loud),
  asserts `go.sum`, `go build`, runs the binary (asserts it uses the dep),
  `go version -m` (asserts the embedded module record). Prints per-step
  timings + the `go: downloading` lines.
- **`tools/interactive/go5.exp`** — the LS-CI scenario driving both as the
  logged-in user; **the only external-network-dependent scenario**: an
  in-guest `nslookup example.com` guard SKIPs (exit 0) on offline hosts.

## 7. Known caveats / footguns

1. **Deadlines do not abort in-flight ops** (section 3). `http.Client.Timeout`
   fires only between ops; a hung peer hangs the goroutine (the M is parked in
   the SVC). Fix path: the plan9 model (close-on-deadline via a timer) or a
   kernel cancellation surface — a Stage 8 (IDE/debugger arc) candidate, since
   gopls is long-running.
2. **`GOPROXY=off` + `GO111MODULE=off` leak from joey's env** into every login
   session (env inheritance). Module work overrides them per-session (the
   `go-get` driver does; an interactive user must too — there is no ut
   env-set builtin yet, the G15 seam).
3. **Writable dirs**: the pool bake is SYSTEM-owned + perm-enforced (A-3); a
   logged-in user points GOCACHE/GOPATH/GOTMPDIR at `$HOME`-side or `/tmp`
   paths (the driver defaults to its workdir argument).
4. **netpoll is blocking** — each in-flight net op holds an M (an OS thread).
   Fine for fetch-and-exit tools and modest servers; a high-fan-in server
   would want the Loom-side integration (a Stage 8+ candidate).
5. **IPv4-only resolution** (netd cs/DNS v1.0); no raw IP; no BSD Unix
   sockets in the native net package.
6. `GOFLAGS=-mod=vendor` anywhere in the env breaks module downloads —
   the driver clears `GOFLAGS`.
7. **`os.Executable` is ENOSYS** (no self-image path at v1.0; callers fall
   back to `os.Args[0]`). Visible as cmd/go's benign "failed to start
   telemetry sidecar" warning; a `/proc/<pid>/text`-style surface is the
   v1.x fix.
8. **Errno fidelity**: a kernel `-1` (the generic failure) renders as Go
   errno 1 = EPERM ("operation not permitted"), which can mislead — the
   getrandom cap-gate failure read as a permissions problem, which it
   technically was, but so would a bad VA. Per-touch errno upgrades are the
   ERRORS.md v1.x staged rollout.

## 8. Cross-references

- `docs/GO-PORT-PLAN.md` — the staged plan (Stages 1–6) + locked decisions;
  Stage 8 pointer to `docs/GO-IDE-DESIGN.md` (gopls + dlv + Ambush).
- `docs/CHASE.md` — the on-device `go build` performance record (S1 warm
  parity; S3 cold = the measured userspace-FS-server tax + section 9 avenues).
- `docs/reference/121-netd.md` (the `/net` server), `115-tls.md`-adjacent
  (the Rust TLS arc + the CA bundle bake), `128-devenv.md` (the `/env`
  device), `127-overcommit.md` (the Go heap model).
- Fork commits: `crypto/x509` system-bundle reconciliation +
  `go.env GOTOOLCHAIN=local` (Stage 5, 2026-07-14).
