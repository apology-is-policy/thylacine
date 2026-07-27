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

**The base is now current.** `gfx-4` merged `origin/main` (CL-4, `da049000`) at
`1273286d` — zero behind main — so round 2 is a small, one-directional merge of
this branch's own work rather than a two-way reconciliation.

### The commits

```
1273286d  Merge main (CL-4) into gfx-4     <- the base update; already contains main
7506fc23  VIVARIUM V-4a: the diorama       <- usr/diorama + usr/diorama-probe
f1e3dbef / 0c6ac776  docs (roadmap + this handoff)
5af01124  sched test: spin-until in notify_idle_peer_smoke
406d75a9  VIVARIUM V-4a-0b: srv_peer_info.pid
2e70f5ba  VIVARIUM V-4a-0: Proc.exe_path + /proc/<pid>/exe
b7df5b21  docs: the aux-track roadmap
```

### The two collisions are ALREADY RESOLVED here

Both surfaced while merging main *into* gfx-4, so main's side is preserved and
round 2 should carry these resolutions rather than re-litigate them:

- **`struct Proc` — prowl-1 and V-4a-0 both appended at offset 352.** Stacked
  rather than chosen between: prowl-1's `name[PROC_NAME_MAX=32]` keeps @352,
  V-4a-0's `exe_path` moves to @384. Size **384 → 392**. They are complementary
  — `name[]` is a bounded copy of the *basename* (what a listing wants, and it
  survives a Path-alloc failure); `exe_path` is the *full* path (what
  `readlink("/proc/self/exe")` means, and a basename cannot reconstruct it).
  Deriving one from the other is a recorded follow-up, deliberately not done
  inside a merge — collapsing them changes both prowl's and the diorama's
  contracts. **The size assert is what caught the collision**, which is the good
  failure mode.
- **qid subkind 12 — `PQS_EXE` vs `PQS_SCHED`.** `SCHED` keeps 12 (already
  landed); `EXE` moved to 13. Kernel-internal encoding, not ABI.

The feared three-way `kernel/syscall.c` overlap **did not happen** — it
auto-merged, as main predicted.

### Everything else was a union

`devproc.c` (includes / file table / the read whitelist, now admitting both EXE
and SCHED), `test_devproc.c`, `tools/build.sh` + `usr/Cargo.toml` (list
**contents** unioned, never the lines — the recorded trap), `32-devproc.md`.

## Gates for round 2

```
tools/build.sh all           # kernel + userspace both changed
tools/test.sh                # expect 1208 PASS, 0 FAIL, boot OK, 0 EXTINCTION
tools/ci-smp-gate.sh         # struct Proc grew again (384 -> 392)
tools/test-interactive.sh ls-ci
```

Measured on the merged base at `1273286d`: build 0 · **1208/1208** (main's 1207 +
`devproc.read_exe`) · boot OK · 0 EXTINCTION · SMP **40/40, 0 corruption / 0
timing / 0 other** · `ls-ci` PASS first attempt.

Round 2 adds three in-guest probes that are boot-fatal on regression — all three
already pass alongside main's own:
`joey: V-4a-0 /proc/<pid>/exe OK (/bin/ptyfs)`, `diorama: selftest PASS`, and
`diorama-probe: PASS`. Kernel tests `devproc.read_exe` +
`proc.identity_peer_snapshot_by_stripes` must pass.

## What NOT to do

- **Do not merge across a dirty worktree** (this bit round 1's planning; both trees
  are clean now).
- **Do not "fix" the `struct Proc` size assert by loosening it.** If it fires, the
  merge dropped or duplicated a field — that is the assert doing its job.
