---
id: lock-env
type: lock
title: "Env.lock — the per-Proc environment guard"
kind: spin
orders-before: [lock-kmem-cache]
guards: "one Env's entries[] table, its next_id counter, and the name/value storage they point into"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

A per-`Env` spinlock guarding the `/env` variable table. It exists because peer
Threads of one Proc **share** the Env — a Proc's environment is one object with
many concurrent mutators — so unlike most per-Proc structures there is no
single-writer story to fall back on.

Every access takes it: get, set, unset, list, and the flat-block render that
`/proc/<pid>/environ` serves. There is no lock-free fast path, deliberately;
the table is small and the operations are cold.

A genuine **leaf**. The only lock reached beneath it is the slab allocator's, and
nothing held under it takes anything else — in particular nothing under it
reaches for [[lock-proc-table]], which is what makes the inbound
`proc-table -> env` edge acyclic. That edge is real and load-bearing: the
`environ` render resolves its target Proc inside a process-table walk and takes
this lock nested, relying on the outer lock for the target's *lifetime* (the Env
is freed from the Proc teardown) and on this one for the table's *stability*.

The entry identifiers are monotonic and never reused, which is what lets a
resolved handle fail cleanly rather than silently resolving to a different
variable if one is removed between a walk and a read — a lock alone would not
give that, since the two operations are separate calls.

Cloned rather than shared across a fork: a child gets an independent copy under
the parent's lock, so the two diverge from that instant.
