# pouch-stratumd-build — the first cross-compiled daemon against pouch

Phase 6 sub-chunk 15 (`pouch-stratumd-build`). Cross-compiles **stratumd**
(Stratum's per-pool POSIX userspace daemon — ~860 KiB static `ET_EXEC`)
for `aarch64-thylacine` against the pouch sysroot. The chunk's purpose
is to **prove the pouch cross-toolchain end-to-end on a real-world,
multi-subsystem C codebase** — a step beyond sub-chunk 14's libsodium
(which is portable computation with no OS contact). Stratumd uses
pouch's full POSIX surface (pthread, sockets, file I/O, signals,
mlock-on-bearer-credentials, libsodium-via-sysroot) and a Thylacine-arm
in `peer_creds.c` that maps `SO_PEERCRED` onto `SYS_srv_peer`
underneath. Not audit-bearing per `POUCH-DESIGN.md §14` row 15 —
Stratum-side coordination; pouch's surface is unchanged.

## What landed

| Component | Path | Size |
|---|---|---|
| Cross-built daemon | `build/pouch/progs/stratumd` | 860536 bytes |
| Build step | `tools/build.sh::build_stratumd` | new in this chunk |
| Stratum-side Thylacine arm | `src/cmd/stratumd/peer_creds.c` (Stratum repo, branch `thylacine-pouch-arm` off `main`@`976cb6f`) | +13 lines |
| Stratum-side platform detection | `CMakeLists.txt` (same branch) | +28 lines |
| Coordination handoff | `docs/session-handoff-2026-05-24-thylacine-pouch-arm.md` (Stratum repo) | one-shot artifact for the Stratum agent |

## Cross-OS source story (POUCH-DESIGN.md §10)

Stratum's source stays one tree across Linux + macOS + Thylacine. Where
POSIX itself is insufficient or divergent, a program carries a thin
**per-OS boundary file** selected at build time. Stratumd already did
this — `peer_creds.c` has a `__linux__` arm (`SO_PEERCRED`) and a
BSD/macOS arm (`getpeereid`). Thylacine becomes a **third arm** gated
on `__thylacine__`. POUCH-DESIGN.md §10:

> The measure of a good pouch is how few per-OS arms a port needs.

Stratumd's measure: **one Thylacine arm** (the `peer_creds.c`
`__thylacine__` branch). The other 10 platform-conditional files in
Stratum's tree fall through cleanly on Thylacine without a third arm:

| File | Why the existing arm chain works on Thylacine |
|---|---|
| `src/9p/server.c:56` | `_Static_assert` block confirming the wire constants equal `<errno.h>` on Linux; the comment says non-Linux uses the canonical table directly. |
| `src/cmd/stratumd/serve.c:49` | Just an extra `#include <sys/socket.h>` for `SO_PEERCRED` on Linux; redundant elsewhere. |
| `src/block/posix.c` (`BLKDISCARD`, `FALLOC_FL_PUNCH_HOLE`) | Gated `__linux__ && FEATURE_MACRO`; on Thylacine the function falls through to `return STM_ENOTSUPPORTED`. |
| `src/fs/fs.c:1000` (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`) | Same — feature macro gate; falls through. |
| `src/pool/pool.c:274` (same pthread tuning) | Same. |
| `src/corvus_client/corvus_client.c:375` (`MADV_DONTDUMP`) | Same. |
| `src/janus/daemon.c` | Janus is not used on Thylacine (corvus replaces it; CORVUS-DESIGN.md §1). |
| `src/host_fs/host_fs.c`, `src/cmd/stratum-host-fs/run.c` | Host-fs auxiliary tool; not built into the stratumd binary. |
| `src/cmd/stratum-slate/run.c` | Slate auxiliary tool; not built. |

This is exactly the "thin per-OS arm" pattern POUCH-DESIGN.md §10
envisions — the Thylacine-specific surface is *one* function in *one*
file in a million-line tree.

## Why the Thylacine arm uses `getsockopt(SO_PEERCRED)` not `t_srv_peer`

POUCH-DESIGN.md §10 line 269 names the Thylacine arm `t_srv_peer` — a
direct libt call exposing the full `struct srv_peer_info` (stripes,
caps, console bit, alive). At v1.0 the simpler implementation is to
reuse the Linux arm body — `getsockopt(fd, SOL_SOCKET, SO_PEERCRED,
&uc, &len)` — because pouch's `0006-pouch-sockets.patch` already
marshals `SO_PEERCRED` onto `SYS_srv_peer` underneath, returning a
`struct ucred` with `pid = peer stripes`, `uid = 0`, `gid = 0` (Thylacine
has no uid model at v1.0). Stratum's `stm_peer_creds` contract is
**uid + gid**; the lossiness is tolerable.

Forward-looking: when Thylacine grows a uid model and the lossy ucred
marshal becomes load-bearing, switching to a `t_srv_peer` arm exposing
the full kernel-stamped record is a small follow-up. Deferred to v1.x.

## Toolchain (`cmake/Toolchain-aarch64-pouch.cmake`)

Existed before sub-chunk 15 (no CMake-using consumer until now). This
sub-chunk updated three things:

| Change | Why |
|---|---|
| Added `-D__thylacine__=1` | The C-visible peer of the `aarch64-thylacine` triple. Stratum's `peer_creds.c` + `CMakeLists.txt` both gate on this macro. |
| Added `-D_GNU_SOURCE=1` | Pouch is musl-based; `struct ucred`, `getrandom`, `fallocate`, etc. live behind `_GNU_SOURCE`. Setting it at the toolchain level keeps each consumer (Stratum, future ports) from re-stating it. |
| `-nostdinc` → `-nostdlibinc` | Mirrors `build_compiler_rt`'s reasoning: `-nostdlibinc` strips host **system** headers (libc) while keeping clang's **own resource headers** (`stdint.h`, `stdarg.h`, `arm_neon.h`, `unwind.h`). Without this, `third_party/xxHash_src/xxhash.h` (which `#include <arm_neon.h>`) fails to compile. |
| Removed `-fstack-clash-protection` | clang on `aarch64-thylacine` emits `argument unused during compilation` warnings for this flag (the link phase doesn't consume it). Stack-clash protection is a stack-allocation-side check; on aarch64 with the kernel's per-thread fixed-size user stacks (no growable stacks at v1.0), the protection has limited applicability anyway. |
| New `CMAKE_C_LINK_EXECUTABLE` | Routes the link step through `tools/pouch-ld`, which drives `ld.lld` directly with the static/W^X/non-PIE link line + auto-supplies the CRT + libc.a + libclang_rt.builtins.a. clang as a link driver mis-selects the host Darwin toolchain for unknown OS triples (see `tools/pouch-ld` header). |

## Stratum-side coordination (`thylacine-pouch-arm` branch)

The Stratum-side changes live on branch `thylacine-pouch-arm` in
`~/projects/stratum/v2` (off `main`@`976cb6f`, the R172 close tip).
**The Stratum agent is paused during this work**; the branch is the
hand-off artifact, alongside `docs/session-handoff-2026-05-24-thylacine-pouch-arm.md`
which describes the integration. The Stratum agent merges the branch
forward when convenient (no time pressure — Stratum's own Phase 9.8
crown-jewel arc dominates their roadmap).

Two files changed in the Stratum tree:

**`v2/CMakeLists.txt`**:
1. `STM_PLATFORM_THYLACINE` detection (when `CMAKE_C_COMPILER_TARGET STREQUAL "aarch64-thylacine"`).
2. Synthesize `Threads::Threads` + `PkgConfig::LIBSODIUM` IMPORTED targets on Thylacine — host `find_package(Threads)` and `pkg_check_modules(libsodium)` can't see the cross-sysroot (pouch's musl bundles pthread in libc; libsodium ships pre-built without a `.pc` file).
3. Skip `_FORTIFY_SOURCE=2` on Thylacine (musl doesn't implement it).

**`v2/src/cmd/stratumd/peer_creds.c`**:
- Extend the `__linux__` arm guard to `__linux__ || __thylacine__`. Same body — `getsockopt(SO_PEERCRED)` is routed to `SYS_srv_peer` by pouch.

## `build_stratumd` configure flags

| Flag | Why |
|---|---|
| `-DCMAKE_TOOLCHAIN_FILE=<...>/Toolchain-aarch64-pouch.cmake` | The pouch cross-toolchain (defines `__thylacine__`, `_GNU_SOURCE`, drives pouch-ld). |
| `-DSTM_ENABLE_PQ=OFF` | liboqs (ML-KEM-768) not cross-compiled for pouch at v1.0. PQ wrap deferred to a future libt-side port. |
| `-DSTM_ENABLE_IOURING=OFF` | io_uring is Linux-only; explicit-off avoids the `pkg_check_modules(liburing)` probe. |
| `-DSTM_ENABLE_LIBAIO=OFF` | libaio is Linux-only. |
| `-DSTM_BUILD_TESTS=OFF` | Tests need a host harness that doesn't exist on Thylacine. |
| `-DSTM_BUILD_FUZZERS=OFF` | libFuzzer not cross-compiled. |
| `-DSTM_WERROR=OFF` | Pouch's clang flags + musl headers surface warnings the upstream Stratum tree hasn't hit; cross-compile is best-effort on warning hygiene at v1.0. |
| `-DSTRATUM_BUILD_TESTING_HOOKS=OFF` | Production build: the test-only API hooks (`stm_snapshot_create_for_test`) literally don't exist in the linked archive. |

## v1.0 limitations and deferred items

| Limitation | Deferred to |
|---|---|
| PQ hybrid wrap disabled (liboqs not cross-built) | v1.x — needs liboqs cross-compile + sysroot install, paralleling libsodium's path |
| `__APPLE__`-only auxiliary tools (host-fs, slate) not built | Out of scope — they're macOS/Linux-only debug tools |
| Janus daemon not built | Out of scope — Thylacine uses corvus, not janus |
| Stratumd not yet spawned by joey | Sub-chunk 16 (`pouch-stratumd-boot`): joey spawns stratumd; ramfs pivot to `/sysroot`; boot wedges if stratumd dies. |
| Server-accepted `getsockopt(SO_PEERCRED)` returns `uid=0`, `gid=0` | v1.x — when Thylacine grows a uid model, peer_creds switches to a `t_srv_peer` arm exposing live caps + stripes. |
| `_FORTIFY_SOURCE=2` not applied to the Thylacine build | musl doesn't implement it; permanently unavailable. |

## Audit posture

**NOT audit-bearing** per `POUCH-DESIGN.md §14` row 15.

The chunk:
- Adds no new kernel surface.
- Adds no new pouch musl arms (the existing `SO_PEERCRED` route from sub-chunk 12 is reused).
- Touches one toolchain file (`cmake/Toolchain-aarch64-pouch.cmake`) and adds one build helper (`tools/build.sh::build_stratumd`).
- Touches two Stratum-side files (one `__thylacine__` arm + one platform-detection branch + IMPORTED-target stubs).

The audit-bearing follow-up is **sub-chunk 16** (`pouch-stratumd-boot`),
which integrates stratumd into the boot path — joey spawns it, ramfs
pivots to `/sysroot`, boot wedges if stratumd dies.

## How to rebuild

```sh
tools/build.sh sysroot     # builds libc + compiler-rt + libsodium
tools/build.sh stratumd    # cross-builds stratumd against the sysroot
```

The output binary lives at `build/pouch/progs/stratumd`. Verify:

```sh
$ /opt/homebrew/opt/llvm/bin/llvm-readelf -h build/pouch/progs/stratumd | head -8
$ /opt/homebrew/opt/llvm/bin/llvm-nm --defined-only build/pouch/progs/stratumd | grep stm_peer_creds
0000000000225720 T stm_peer_creds
```

## Cross-references

- `docs/POUCH-DESIGN.md §10` — the cross-OS source story (Thylacine arm pattern).
- `docs/POUCH-DESIGN.md §14` row 15 — sub-chunk 15 scope.
- `docs/reference/78-pouch.md` — why pouch-ld is separate from clang on macOS.
- `docs/reference/84-pouch-libsodium.md` — sub-chunk 14's libsodium build (the model for sub-chunk 15's CMake-based build).
- Stratum tree: `v2/CMakeLists.txt` (Thylacine platform detection), `v2/src/cmd/stratumd/peer_creds.c` (the Thylacine arm), `v2/docs/session-handoff-2026-05-24-thylacine-pouch-arm.md` (the coordination artifact).
