# 144 -- prowl: the scheduler-aware process monitor (the tool)

**Status:** prowl-2 (the tool MVP) as-built. The kernel telemetry it reads is
prowl-1 (`docs/reference/15-scheduler.md` on-CPU accounting + `32-devproc.md` +
`33-devctl.md`); the design + phasing is `docs/PROWL-DESIGN.md`.

`prowl` is a native `libthyla-rs` Kaua TUI -- an htop-equivalent that runs
on-device. It polls the kernel's synthetic `/ctl` + `/proc` filesystems, derives
per-process %CPU, renders a live process list under an aggregate CPU meter, and
(as a manager) terminates the selected process. It is the tool the HVF-idle-256%
hunt (`docs/DEBUGGING-PLAYBOOK.md 6.17`) would have used to name the culprit on
sight instead of git-bisecting.

## Purpose

Thylacine tracks rich per-process telemetry (since prowl-1: cumulative on-CPU
time + process names) but exposes it only as text in `/ctl/procs` +
`/proc/<pid>/status`. `prowl` turns that into a live, sortable, interactive view
+ a kill surface -- the "what is burning my cores?" answer.

The identity fit is exact: the filesystem is the API (Plan 9's `/proc` model),
and a Kaua TUI is the shell-as-UI presentation. A buggy `prowl` corrupts only its
own screen; it validates nothing on the kernel's behalf (the kernel gates every
read and the kill).

## Invocation

```
prowl                 # live monitor, sorted by %CPU
prowl -s pid          # initial sort: cpu (default) | pid | mem | name
```

Run it from `ut` as a bare command (`/bin` resolution, #58). `ut` recognizes
`prowl` as a full-screen TUI (`console::is_raw_command`) and performs the
raw-mode dance before spawning it (below).

### Keys

| Key            | Action                                                      |
|----------------|-------------------------------------------------------------|
| Up / Down      | Move the cursor (tracks the process, not the row index)     |
| PageUp / PageDown | Move the cursor by 10                                    |
| Home / End     | Jump to the first / last process                            |
| `s`            | Cycle the sort key (cpu -> pid -> mem -> name)              |
| `r` / Space    | Refresh now (re-sample immediately)                         |
| `k`            | Kill the selected process (opens a `y`/`n` confirm)         |
| `q` / Esc / Ctrl-C | Quit                                                    |

The `k` kill is confirm-gated (`k` then `y`) so a stray keystroke cannot
terminate a process. `Ctrl-C` arrives as a raw key (ut sets `-isig` for a raw
command) and quits like `q`.

## The data it reads (prowl-1 substrate)

`prowl` does no new syscalls -- it opens + reads two text files.

- **`/ctl/procs`** (`kernel/devctl.c` `format_procs`) -- the all-pids process
  table, one header line then 7 whitespace-separated columns per process:

  ```
  PID    NAME    STATE    THREADS    PAGES    CHILDREN    CPU_NS
  1      kproc   ALIVE    3          16       1           4210000
  ...
  ```

  `prowl` parses pid / name / state / threads / pages / cpu_ns (children is
  parsed for column alignment but not surfaced at prowl-2). Visibility is the
  Plan 9 all-pids posture (#57a) -- coarse metadata for every process.

- **`/ctl/sched`** (`kernel/devctl.c` `format_sched`) -- the `cpus: N` line gives
  the online CPU count (`smp_cpu_count()`), which normalizes the aggregate meter.
  Absent -> falls back to 1.

Reads use the cpubench idiom: `File::open` + a bounded (4 KiB) read-to-EOF. The
kernel caps `/ctl/procs` at `DEVCTL_READ_BUF` (2048 bytes, ~30 processes); a
busier system's tail is truncated kernel-side -- a pagination seam tracked to the
#62 perf backlog, not `prowl`'s.

## %CPU derivation (the htop method)

The kernel exposes **cumulative** `cpu_ns` (monotonic on-CPU nanoseconds, summed
over a process's threads). `prowl` derives instantaneous %CPU by diffing two
polls:

```
cpu_pct = (cpu_ns_now - cpu_ns_prev) / (wall_ns_now - wall_ns_prev)
```

100% == one core fully busy; a process spanning two cores reads 200%. The wall
delta is `time::Instant` (the #343 vDSO monotonic clock -- no syscall). The math
is integer, in tenths of a percent (`delta_ns * 1000 / elapsed_ns`); no float.

- A process unseen last poll, or whose `cpu_ns` went backward (pid reuse), reads
  0% this frame and corrects on the next (`saturating_sub`).
- The **first** frame has no delta -> every process reads 0% (and is pid-sorted);
  the first ~1.5 s tick shows real rates.

The refresh cadence is ~1.5 s (the htop default). Keys wake the loop early for
responsive navigation without resampling, unless a full interval has elapsed --
so %CPU stays live even while navigating. The event loop is
`PollSource::poll(PollTimeout::Millis(1500))`: an empty return is the tick.

## The view (`ui.rs`)

Three regions, top to bottom (`Layout::vertical`):

- **Header (2 rows):** `prowl   procs: N   cpus: N`, then an aggregate CPU meter
  `CPU [████░░░░] 47.3%  total 189.2%` -- utilization = the sum of every
  process's %CPU divided by the core count (0..100%); total = the raw sum (can
  exceed 100% across cores). The bar is drawn by hand (kaua has no gauge widget)
  with `█`/`░` cells.
- **Body:** the process table (kaua's `Table` widget) -- columns pid / name /
  %cpu / mem(pages) / threads / state, sorted, with the selected row highlighted
  (reverse video) and scroll-to-keep-selection-visible.
- **Footer (1 row):** the key hints + the active sort; a pending kill replaces it
  with the confirm prompt.

The cursor tracks a **PID**, not a row index, so it stays on a process as %CPU
re-sorts the list (the htop cursor-follows behaviour). A process that exits snaps
the cursor to the top.

## The kill authority (I-26; no new authority)

`k` -> `y` writes `"kill"` to `/proc/<pid>/ctl` (`kernel/devproc.c`
`devproc_write`). The kernel enforces the I-26 two-axis gate at the write site:
the caller must **own** the target (same `principal_id`, the `0600` ctl w-bit) OR
hold `CAP_HOSTOWNER` / `CAP_KILL`. `prowl` confers **no authority of its own** --
a denied kill returns `-1` (surfaced as "kill <pid> denied"), so an ordinary
user's `prowl` terminates only its own processes. This composes I-26 + I-22
(fs-admin `CAP_DAC_OVERRIDE` is deliberately not a kill axis).

## Console discipline (I-27; the nora contract)

`prowl` owns the SCREEN on fd 1 (`kaua::Terminal`) and reads keys on fd 0
(`kaua::PollSource`); it **never** touches the line discipline (consctl). `ut`
sets raw termios (`-icanon -echo -isig -icrnl -onlcr`) via its private consctl fd
**before** the spawn (the T-4 dance -- `prowl` is in `ut`'s `is_raw_command`
set), hands `prowl` fd 0/1/2 (Inherit), and on `prowl`'s exit OR crash re-cooks
the console + re-emits the screen-restore escapes (the `panic=abort` backstop,
since a `no_std` `Drop` does not run on a crash). `prowl` is never
console-attached -> I-27 is untouched. On the clean path `Terminal::Drop` +
an explicit `Terminal::leave()` both restore the alt-screen (idempotent).

Size: there is no winsize syscall, so `prowl` measures the console via a CPR
round-trip at launch (`kaua::query::terminal_size`), falling back to 80x24 when
the terminal does not answer (a dumb terminal / the non-interactive harness). A
late CPR reply arrives as `Event::Resize` and re-fits the view (the only
live-resize path over UART; mirrors nora / `bug_nora_hvf_cpr_handshake`).

## Files

| File                 | Role                                                    |
|----------------------|---------------------------------------------------------|
| `usr/prowl/src/main.rs` | Entry (`rs_main`), the App state, the event loop, key dispatch, the kill + the `/ctl` reads. |
| `usr/prowl/src/sample.rs` | The pure telemetry layer: `/ctl/procs` + `/ctl/sched` parsing, the cross-poll %CPU sampler, the sort keys. Terminal- and clock-free. |
| `usr/prowl/src/ui.rs`  | The Kaua rendering (the CPU meter, the process table, the footer). |
| `usr/prowl/Cargo.toml` | A device-only native crate: `kaua` (default features -> the console backend) + `libthyla-rs`. |

Build wiring: workspace member (`usr/Cargo.toml`), ramfs bake
(`tools/build.sh` `usr_rs_bins` -> `/bin/prowl` via joey's `/bin` bind, #58),
and the raw-command allow-list (`usr/utopia/libutopia/src/eval/console.rs`
`is_raw_command`).

## Tests + validation

- The pure `sample` layer is obviously-correct by inspection (a 7-token
  whitespace parse + integer %CPU math); the design's prowl-5 is the focused
  audit.
- **`tools/interactive/prowl.exp`** (an LS-CI scenario) is the runnable coverage
  no host test can reach: it boots, logs in, launches `prowl`, asserts the render
  (the `cpus:` banner + the `MEM(pg)` table header + the live process `kproc`),
  exercises the sort + cursor keys, quits with `q`, and proves `ut` restored a
  usable console (a post-quit command runs at a clean prompt). This exercises the
  whole fd-bound path -- the raw-mode dance, the screen acquisition, the `/ctl`
  read + parse + render, and the restore.

## Status + known caveats

- **This is prowl-2, the MVP.** The aggregate CPU meter, the process list, %CPU,
  sort, and kill are live. Kernel byte-unchanged -- pure userspace.
- **Per-CPU meters** (one bar per core) need the per-CPU `/ctl/cpu` leaf -> that,
  plus the **scheduler-introspection view** (`/proc/<pid>/sched`: band /
  parks-per-s / wake-source / starved) + a detail pane + band/parks columns, is
  **prowl-3** (the "better than htop" differentiator).
- **stop/cont + the tree view** are **prowl-4**.
- The `/ctl/procs` ~30-process cap (kernel `DEVCTL_READ_BUF`) truncates a busy
  system's tail; the sampler's previous-poll lookup is a linear scan (O(n^2) over
  the process count) -- both fine at the current process scale, tracked for the
  #62 perf pass.
- No host unit tests: `prowl` is a `no_std` binary crate (no lib half), so the
  `sample` layer is validated in-guest + at prowl-5, not via `cargo test`.
