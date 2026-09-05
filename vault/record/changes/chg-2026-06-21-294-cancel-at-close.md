---
id: chg-2026-06-21-294-cancel-at-close
type: chg
title: "#294: readiness-op cancel-at-close (the permanent netd-slot-leak fix)"
date: 2026-06-21
arc: arc-net
commits: ["bb720981", "4de9e4a6"]
touched: [sub-kernel-ninep-dev9p-poll, sub-kernel-ninep-session, sub-kernel-ninep-dev9p]
established: []
closed: [fnd-294-self-1, fnd-294-self-2, fnd-294-r1-f1]
opened: []
mirrors-checked: []
depth: rich
created: 2026-07-31
---
## What

The readiness op stops pinning the `ready` Spoor (the leak root: the pin
deferred `dev9p_close` -- and the slot-freeing Tclunk -- behind a kthread
GC that an SMP race could skip forever). It pins the refcounted poll-state
+ the SESSION instead; `dev9p_close` runs at the user's fd-close, grabs
the op from the registry (whoever unlinks owns it), cancels via Tflush,
frees, then delivers the Tclunk deterministically. Model-first
(`bb720981`: net_poll_teardown.tla -- Fix=TRUE clean + liveness,
Fix=FALSE reproduces the leak) because the bug was a Heisenbug (in-guest
prints shifted the GC window and hid it) -- the model, not a boot, is the
reliable design witness. A SECOND load-bearing fix sat BELOW the model's
abstraction and was caught by the kernel TEST: `any_outstanding_on_fid`
counted `awaiting_flush` entries, so the cancel's own Tflush made the
immediate Tclunk REFUSE -- the slot would have leaked anyway
([[fnd-294-self-2]], fixed in [[sub-kernel-ninep-session]]).

## Why

The pre-#294 net-6b design's "borrow-guard defers dev9p_close" mechanism
was itself the leak; bounded (netd MAX_SLOTS=16) but permanent per leaked
slot on the poll-timeout/abandon path.

## Alternatives rejected

Keeping the Spoor pin + making the GC reliable (the GC's window IS the
race; determinism at fd-close is the correct ownership); per-op deadlines
on the readiness Tread (the #841 desync class).

## Verification

`dev9p.poll_cancel_at_close` + net_poll_teardown.tla gate + the formal
round ([[adt-294-r1]], 0/0/0/3 NOT dirty, the prosecutor independently
confirming both self-found fixes) + SMP gate 0 corruption across 45 boots.
