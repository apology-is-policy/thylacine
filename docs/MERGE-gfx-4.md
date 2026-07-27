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

## Round 2: DONE (2026-07-27, by the main agent)

`gfx-4` @ **`7b917e55`** merged into local `main` at **`db566f28`** ("Merge gfx-4
into main: VIVARIUM V-4a + the aux-resolved collisions"). The whole VIVARIUM V-4a
arc (V-4a-0 `Proc.exe_path`, V-4a-0b `srv_peer_info.pid`, V-4a the diorama) is in.

**Not yet pushed**: `origin/main` is still `da049000`. Local `main` is clean.

**Verified in main's tree** (checked, not assumed) — all four aux-side resolutions
carried through:

| Resolution | State in main |
|---|---|
| `sizeof(struct Proc) == 392` | ✅ |
| prowl-1 `name[]` @352 | ✅ |
| V-4a-0 `exe_path` @384 | ✅ |
| `PQS_SCHED=12` / `PQS_EXE=13` | ✅ |
| `usr/diorama` + `usr/diorama-probe` | ✅ present |

The two collisions and why they resolved the way they did are recorded in the
`struct Proc` field comments and at the `PQS_*` enum, so the reasoning travels
with the code rather than only with this file.

---

## The working pattern (both rounds)

1. Aux works on `gfx-4`, pushing to both mirrors.
2. Aux periodically merges `origin/main` **into** `gfx-4` to keep its base current
   — a merge, never a rebase: `gfx-4` is pushed, so a rebase would need a
   force-push, and its earlier history is already in `main` via prior merge
   commits.
3. Main periodically merges `gfx-4` **into** `main`.

Collisions caught by a `_Static_assert` (the `struct Proc` offset clash both
rounds) are the good failure mode — loud at build time rather than silent. Do not
loosen one to make a merge pass.

## What NOT to do

- **Do not merge across a dirty worktree** (this bit round 1's planning; both trees
  are clean now).
- **Do not "fix" the `struct Proc` size assert by loosening it.** If it fires, the
  merge dropped or duplicated a field — that is the assert doing its job.
