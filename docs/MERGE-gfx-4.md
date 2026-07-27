# Merge handoff: `gfx-4` -> `main`

## Round 1: DONE (2026-07-27, by the main agent)

`gfx-4` merged into local `main` at **`15edb01e`**, plus a follow-up fix
`de451566` ("pouch 0030: restore the O_APPEND seek-to-END the gfx-4 merge
dropped"). The pouch series collision this doc warned about (§2 of the original)
was real and was resolved; the O_APPEND restore is exactly the residue that
section flagged.

The merge took `gfx-4` at **`11ebf755`**.

**Not yet pushed**: `origin/main` is still `b0bf63f2`. Local `main` is clean.

---

## Round 2: what is STILL outstanding

### A. Five `gfx-4` commits landed after the merge point

```
5af01124  sched test: spin-until in notify_idle_peer_smoke (the smp4 1-in-10 red)
f1e3dbef  docs: the aux roadmap records the two V-4a kernel prerequisites
406d75a9  VIVARIUM V-4a-0b: srv_peer_info.pid -- how the diorama resolves `self`
2e70f5ba  VIVARIUM V-4a-0: Proc.exe_path + /proc/<pid>/exe
b7df5b21  docs: the aux-track roadmap (three streams)
```

File set (15 files, `git diff --name-only 11ebf755..gfx-4`):

```
docs/AUX-ROADMAP.md              kernel/proc.c
docs/VIVARIUM.md                 kernel/syscall.c
docs/reference/32-devproc.md     kernel/test/test.c
kernel/devproc.c                 kernel/test/test_devproc.c
kernel/exec.c                    kernel/test/test_proc_identity.c
kernel/include/thylacine/proc.h  kernel/test/test_sched.c
kernel/include/thylacine/syscall.h   usr/joey/joey.c
                                 usr/lib/libthyla-rs/src/lib.rs
```

All pushed to both mirrors; gates on that tip were default 1199/1199, boot OK,
SMP 40/40 (0 corruption / 0 timing / 0 other), `ls-ci` PASS.

**One thing changed since round 1's analysis.** That analysis said V-1a's
`Proc.phenotype` "auto-merges" because it fit the tail pad and `struct Proc` stayed
352. V-4a-0 **grows `struct Proc` to 360** — no tail pad remained — and adds an
offset assert at 352. The size assert is the drift detector, so a bad merge fails
the build loudly rather than silently; but it is no longer a free auto-merge.

### B. Clade CL-4 never landed — the advised ordering was inverted

Round 1's §6 said: *land `clade-cl4-wip` -> `main` **first**, then `gfx-4`.* In the
event `gfx-4` went first and **CL-4 is still outstanding** — four commits:

```
7cfcabce  CL-4: make the clade gate actually boot-fatal + prove the /dev fstat fix
4d6f0680  Clade CL-4: clang++ compiles + links + runs on-device
c288dd75  Clade CL-4 WIP: clang++ runs on-device; 3 kernel bugs fixed
4b93a4dc  Clade CL-4 (device toolchain) WIP
```

This is an inconvenience, not a defect. The rationale for the ordering was that
CL-4 touches `kernel/elf.c` + `kernel/syscall.c`, so landing it first would keep
the later VIVARIUM work conflict-free. Landing it second instead means CL-4 now
merges against a `main` that already carries the gfx-4 kernel changes.

**Note the three-way overlap on `kernel/syscall.c`**: main (via gfx-4), CL-4, and
round 2's V-4a-0b all touch it. V-4a-0b's change there is small and localised — a
`pid` out-param threaded through `proc_peer_snapshot_by_stripes` and one extra
assignment in `sys_srv_peer_handler` — so it should sit well away from CL-4's ELF
and mmap-shape work, but it is worth diffing rather than assuming.

**Suggested order from here**: land CL-4 first (it is the older, larger, more
entangled branch), then round 2. That restores the original rationale.

---

## Gates for round 2

```
tools/build.sh all           # kernel + userspace both changed
tools/test.sh                # expect >= 1199 PASS, 0 FAIL, boot OK, 0 EXTINCTION
tools/ci-smp-gate.sh         # struct Proc grew -- SLUB sizing changed for every Proc
tools/test-interactive.sh ls-ci
```

Round 2 adds two in-guest probes that are boot-fatal on regression:
`joey: V-4a-0 /proc/<pid>/exe OK (/bin/ptyfs)` must appear, and the kernel tests
`devproc.read_exe` + `proc.identity_peer_snapshot_by_stripes` must pass.

## What NOT to do

- **Do not merge across a dirty worktree** (this bit round 1's planning; both trees
  are clean now).
- **Do not "fix" the `struct Proc` size assert by loosening it.** If it fires, the
  merge dropped or duplicated a field — that is the assert doing its job.
