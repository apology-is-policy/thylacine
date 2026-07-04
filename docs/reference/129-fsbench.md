# 129 — fsbench: the in-guest FS throughput bench

As-built reference for `/bin/fsbench` (`usr/fsbench`) — the native multi-mode
filesystem throughput bench, the storage analog of `netperf` (network) and
`cpubench` (scheduler). It measures **end-to-end in-guest FS throughput** through
the real Thylacine stack, the number a real workload (the on-device `go build`)
actually experiences.

---

## 129.1 Purpose

The Stratum Stabilization arc measured the FS *core* host-side (the ctest
benches `bench_concurrent_write` / `bench_commit` / `bench_create_many` /
`bench_crypto`). But those skip everything between a userspace `write()` and the
Stratum core: the syscall, the dev9p client, the SrvConn rings, the 9P framing,
the scheduler. `fsbench` measures the **whole** path:

```
SYS_READ/WRITE/FSYNC -> dev9p client -> SrvConn -> stratumd (FS server)
                     -> bdev_thylacine -> virtio-blk -> the Stratum pool
```

It is pure userspace (a native `libthyla-rs` no_std binary): a buggy bench
corrupts only its own scratch files (the kernel + stratumd validate every op).
It cleans up its scratch after every mode (the pool persists across boots).

---

## 129.2 Modes

Each mode maps onto a Stabilization area, exercising its work end-to-end:

| Mode | What it measures | Area |
|---|---|---|
| `seqwrite` | sequential write of a large file + fsync — MiB/s | A (coalesce/extent write) |
| `seqread` | read it back, cold — MiB/s | B (multi-extent read + decrypt) |
| `reread` | read it again, warm — MiB/s + speedup vs cold | **E (the #343 dcache, through the real stack)** |
| `rewrite` | overwrite interior blocks (seek+write) — MiB/s | A (the RMW / ar-header-patch path) |
| `create` | create+write+close N small files — files/s | S (the Pleiades object-file storm) |
| `fsync` | write+fsync per small file — files/s + us/durable-file | G (the per-file commit / bdev FLUSH) |
| `sweep` | `seqwrite` at 4/64/256/1024 KiB chunks — MiB/s per size | D (record-size sensitivity) |

Invocation (mirrors `cpubench`):

- `fsbench` — the bounded boot/CI probe (seqwrite/seqread/reread + create +
  fsync, small files), prints data, exits 0 (data is the value — no flaky
  perf-threshold gate, per the no-host-load discipline). A joey boot probe runs
  it every boot (the `joey: fsbench PROBE OK` line; gated on a clean run).
- `fsbench all` — the comprehensive run (every mode incl. `rewrite` + `sweep`).
- `fsbench <mode> [size_mib] [chunk_kib]` — one mode, custom params, from the
  shell.

The scratch dir is `/tmp` (mkdir'd on the pool root; fsbench is spawned by joey
as `PRINCIPAL_SYSTEM`).

---

## 129.3 Representative numbers (Apple M2 / HVF, cpus=4, the boot probe)

```
seqwrite: 8 MiB in 373 ms -> 21.41 MiB/s  (write+fsync, 256 KiB chunks)
seqread:  8 MiB in 336 ms -> 23.74 MiB/s  (cold -- decrypt path)
reread:   8 MiB in  81 ms -> 97.58 MiB/s  (warm -- #343 dcache; 4.1x vs cold)
create:   200 files x 4096 B -> 1936 files/s, 7.56 MiB/s
fsync:    50 files x 4096 B  -> 45 files/s (21776 us/durable-file)
```

Two results stand out:

- **The #343 dcache delivers ~4.1× on a re-read, end-to-end in-guest** (98 vs 24
  MiB/s). Area E's decrypted-extent cache (validated host-side at 8–10× on the
  raw decrypt→memcpy) holds through the full 9P/syscall stack: stratumd serves
  the second read decrypt-free, and the saved decrypt is large enough to show
  even behind the per-RPC overhead.
- **The write/read ceiling (~21–24 MiB/s) is syscall/9P-bound, NOT FS-core
  bound.** `SYS_RW_MAX = 4096`, so an 8 MiB write is **2048 separate
  `SYS_WRITE` syscalls, each a full 9P `Twrite` round-trip** (~182 µs apiece).
  The Stratum core does ~57–200 MB/s host-side; the ~10× gap is the per-4-KiB
  syscall + 9P RPC, even though the 9P msize already permits 8 MiB frames. **A
  `SYS_RW_MAX` bump (larger syscall chunks → far fewer RPCs) is the single
  biggest in-guest FS-throughput lever** — the concurrent-FS arc / RW-11 #62
  domain. fsbench is the instrument that quantifies that gap and will track the
  lift when it lands.
- **fsync ≈ 45 durable files/s (~22 ms/durable-file)** — each is a full pool
  commit + bdev `VIRTIO_BLK_T_FLUSH` (Area G: the 2-phase commit's fixed
  per-commit overhead; the same metadata-re-COW root tracked to the Bε arc).

Absolute MiB/s is host-CPU + accel bound; the *ratios* (the dcache 4.1×, the
sweep curve) are the portable signal.

---

## 129.4 Implementation notes

- Timing: `libthyla_rs::time::Instant` (CNTVCT-backed). MiB/s via u128 integer
  math (no float). Durability: `t_fsync(fd, 0)` — the real `Tsync` → commit →
  bdev FLUSH path.
- Data buffers are filled with a varied (non-zero, xorshift) pattern so an
  all-zero special-case cannot flatter a write; HOT extents do not dedup, so the
  content is otherwise irrelevant.
- Every mode is fail-soft: an I/O error (e.g. ENOSPC on a tight pool) prints a
  `FAIL` line and the bench still exits 0 — the boot probe never breaks the boot.
- `reread` reuses the *same* file `seqwrite` wrote, so the second read is a
  genuine dcache hit on the live extent (the `(paddr,gen)` HOT key).

## 129.6 FS-correctness diagnostics (the go-build `not package main` hunt)

Beyond throughput, `fsbench` carries three **correctness** probes built to
prosecute the on-device `go build` blocker (`link: <pkg>/_pkg_.a: not package
main`, #342). They run in the boot probe and as shell modes; each `PASS`/`FAIL`
isolates one FS hypothesis for that failure. All three currently **PASS**, which
is the load-bearing finding: **the FS is ruled out as the cause** — the bug is
in the on-device Go toolchain (compile logic / link / build-driver), not the
storage stack.

| Mode | Reproduces | Hypothesis it tests |
|---|---|---|
| `coherence` / `coh` | write → close → reopen a *fresh* fid → fstat-size + full read + byte-compare (no-fsync and fsync) | a truncated/unsynced tail seen by a fresh reader |
| (in the boot probe) `arpatch` | write contiguous → reopen `O_RDWR` → seek interior → overwrite → reopen → verify size + patch + tail intact | an interior overwrite (`updateBuildID`-style) dropping the tail (Area A partial-overlap) |
| `arwrite` / `arw` | go `cmd/compile finishArchiveEntry` **byte-for-byte**: `!<arch>\n` + two entries, each *placeholder-header → body → **seek-back-overwrite-header** → **seek-forward-past-data** → write the next entry* | the seek-back-then-seek-**forward**-then-write-more archive shape — the one part `arpatch` omits |

`arwrite` is the sharpest: it mirrors exactly what the Go compiler does when it
emits an archive (`startArchiveEntry` writes a placeholder 60-byte `ar` header,
the body is written, then `finishArchiveEntry` seeks *back* to stamp the real
size into the header and seeks *forward* past the body to write the `_go_.o`
entry — `src/cmd/compile/internal/gc/obj.go`, via `bio.Writer.MustSeek` →
`os.File.Seek` → `SYS_LSEEK`). Its verifier asserts the exact linker success
condition (`ldpkg` scans header lines for one equal to `main`): both size fields
round-trip, both bodies are intact, and the `\nmain\n` line survives. It PASSes
(`sz0=86/86 sz1=1777/1777 body0=ok body1=ok main_line=true`), proving Thylacine's
lseek/write emulation handles the compiler's archive-write pattern correctly —
so a fix for `not package main` will be **go-fork-side, not kernel/FS-side**.

These are correctness assertions, not perf, so they have no host-timing
sensitivity; a `FAIL` is a real FS bug and a hard signal.

## 129.5 Status

Implemented: `usr/fsbench` (the tool), the build.sh bake, the joey boot probe.
Runs every boot (`joey: fsbench PROBE OK`) and from the shell, including the
`coherence` / `arpatch` / `arwrite` FS-correctness diagnostics (129.6). Owed
v1.x: a multi-threaded concurrent-writer mode (the in-guest twin of the host
`bench_concurrent_write`, blocked on a multi-connection FS path — the
concurrent-FS arc); the `SYS_RW_MAX`-bump re-measure when that lever lands.
