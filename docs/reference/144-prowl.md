# 144 — prowl: the scheduler-aware process monitor [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-prowl-doc-absorb`).
The native `libthyla-rs` Kaua TUI — an on-device htop-equivalent that polls the
synthetic `/ctl` + `/proc` filesystems, derives per-process %CPU, renders a live
process list (flat or parent->child tree) under a per-CPU meter, and (as a
manager) kills / suspends / resumes the selected process. Kernel byte-unchanged —
pure userspace over telemetry that landed at prowl-1/3a/3b. Its content lives,
code-verified and current — and *ahead of this doc* — in:

- **the tool itself** — the three-layer split (the pure clock-free/terminal-free
  sampler, the back-buffer UI, the console-owning main loop), the integer
  tenths-of-a-percent htop math (cumulative `cpu_ns` diffed over the wall
  interval; 100% = one core; the counter-went-backwards/pid-reuse `saturating_sub`),
  the idle-inversion CPU meter with its clock-skew clamp, the cursor-tracks-a-PID
  (not a row) navigation that steps *display* order (the real tree-mode fix),
  prowl-4's cycle-safe + orphan-safe tree walk, the confirm-gated kill vs the
  reversible unconfirmed suspend/resume, and the deliberately-conflated
  denied-read / vanished-process "unavailable" detail pane:

      vault/system/userspace/tools/sub-prowl.md

- **the no-new-authority composition** — every control verb is a write to the
  target's `/proc/<pid>/ctl`, adjudicated by the kernel's I-26 two-axis gate
  (owner, or `CAP_HOSTOWNER`/`CAP_KILL`); the OQ-4 gate on `/proc/<pid>/sched`
  decides the detail pane; and the I-27 console posture (owns the screen on stdout,
  reads keys on stdin, never the line discipline — the shell raw-modes around it
  and restores even after a panic-abort) — all in sub-prowl's Invariants +
  Prosecution.

- **the CPR-round-trip console sizing** — there is no winsize syscall, so a Kaua
  TUI measures the console with a cursor-position-report round-trip at launch
  (80x24 fallback; a late reply re-fits). This is a *shared* Kaua mechanism the doc
  itself frames as "mirrors nora", home in the substrate:

      vault/system/userspace/shell-tui/sub-kaua.md

- **the telemetry read surfaces** it consumes — `/ctl/procs`, `/ctl/cpu`,
  `/ctl/sched`, `/proc/<pid>/sched` — are the devctl/devproc dossiers' (prowl reads
  them; it does not define them).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold onto a dossier that is AHEAD of the
  doc.** The doc attributes the ~30-process truncation to a kernel pagination seam
  (#62) and stops there; sub-prowl carries the sharper, verified finding the doc
  lacks: the kernel *computes* the truncation into an `overflow` field, **sets it at
  fifteen distinct points and then discards it** — fifteen writes, zero reads — so a
  truncated read is byte-indistinguishable from a complete one and no client care
  can recover the fact (task #158). It under-reports precisely under the parallel-
  build load it exists to observe. sub-prowl also carries the untested-pure-layer
  caveat (the rate math, the counter-reuse case, the 8-column parse's nine-token
  rejection, the tree walk — all pure, none run; the one-manifest-line fix two
  sibling crates already carry). Zero code change.
