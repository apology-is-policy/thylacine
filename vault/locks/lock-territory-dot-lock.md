---
id: lock-territory-dot-lock
type: lock
title: "Territory.dot_lock — the cwd-string leaf"
kind: spin
orders-before: []
guards: "one Territory's dot_path pointer (the cwd string; NULL == \"/\")"
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

A per-Territory LEAF spinlock guarding exactly one field. Threads of a
Proc share `dot_path` (POSIX per-process cwd), so every read copies the
string out under the hold and never retains the pointer past its
critical section — which is what lets `territory_setdot` swap in the
new string under the lock and `kfree` the old one OUTSIDE it. Two
concurrent `setdot`s capture distinct olds, so neither double-frees nor
leaks.

`cwd_lexical_resolve` runs UNDER the lock in `territory_resolve_cwd`.
That is safe and deliberate: it is bounded CPU with no allocation and no
block, so holding across it costs less than copying the cwd out first.

`kmalloc` under the lock is legal (`territory_clone` duplicates the
parent's cwd that way): SLUB is non-sleeping and knows nothing of
Territory, so `dot_lock -> slub c->lock` has no reverse edge. The lock
is taken only from process/syscall context — chdir, getcwd, the
`SYS_OPEN` relative join, clone — never from an IRQ handler, which is
why a plain `spin_lock` with no IRQ mask is correct.

Deliberately NOT [[lock-territory-ns-lock]]: `dot_path` predates
`ns_lock` and is on a hotter path, so it keeps a leaf of its own. The
one thing to watch is scope creep — this lock guards `dot_path` and
nothing else, and a second field placed under it would be the
"lock added for one field of a shared struct" shape.
