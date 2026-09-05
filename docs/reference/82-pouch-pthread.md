# 82 — pouch pthread [ABSORBED INTO THE VAULT]

Absorbed at the pouch sweep (`chg-2026-08-01-pouch-sweep`). Its content
now lives, code-verified and current, in:

    vault/system/boundary/pouch-seam/sub-pouch-thread.md

(the spawn + clear-child-tid handoff, the two independent tid
guarantees, the 1-hour torpor clamp, the requeue removal, the stack
story, and `nanosleep` on the same primitive.)

**What this file got right, and what it did not carry.** Unusually for
this sweep, its body was CURRENT — the #111 child-side fix and the #112
kernel publish are both described, correctly, as two independent
guarantees rather than one superseding the other.

What it did not carry is `0022-pouch-nanosleep.patch` (G-7a), which puts
`nanosleep` / `usleep` / `clock_nanosleep` on the same torpor primitive
and belongs to the same surface — it landed two months later and was
documented only in its own commit. Before it, every `SDL_Delay` and frame
pacer busy-returned instantly on the `ENOSYS` sentinel.

Binding design (unchanged): `docs/POUCH-DESIGN.md` §7.
