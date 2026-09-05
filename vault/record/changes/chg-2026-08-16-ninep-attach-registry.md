---
id: chg-2026-08-16-ninep-attach-registry
type: chg
title: "Membership as the liveness proof, and a fixed class that came back as data"
date: 2026-08-16
arc: arc-vault
commits: ["35b8847b"]
touched: [sub-kernel-ninep-attach]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The reply-loss investigation needed to see which 9P sessions existed and what
their ring counters said, and there was no way to ask. So the attach layer grew
a registry — and it is worth recording for its lifetime argument rather than for
the feature.

Swept immediately after [[sub-kernel-devctl]], which renders this registry, and
the pair turned out to be one story.

## Membership is the liveness proof

Every session links itself into one global list at construction and unlinks at
the **top** of its last-unref teardown. The walker holds the registry lock across
its **entire** walk.

Those two facts together mean the walk needs no reference count. A session the
walker can reach has not begun tearing down; a session that has begun tearing
down is already unreachable. **List membership itself is the proof**, so the
usual apparatus — take a reference, walk, drop it, handle the case where the
object died anyway — is simply absent.

The reason this is worth writing down is that **it is a pairing, and neither half
is safe alone**. A walker that released the lock mid-walk could resume into a
freed entry. An unlink placed after any teardown step — after the root clunk, say
— would leave a window where the walker reaches a half-destroyed session and
snapshots it. Each half looks like an ordinary implementation choice in
isolation, and only together do they constitute the argument.

That is the third structure in this sweep whose correctness lives in a
*relationship* rather than in either site: the two ring owner pointers bound at
opposite ends of setup, the two class bits whose test order depends on a
constructor elsewhere, and now this. All three are invisible to a reader of
either half.

Lock order is registry then client, and the link and unlink take **only** the
registry lock — so there is no path that could invert it.

## The registry sees production only

Test loopback clients never register, because they do not pass through this
layer.

That is correct for the instrument's purpose and worth stating rather than
leaving implicit: **a defect visible only in this registry's output is a defect
no test can currently observe.** An instrument scoped to production is exactly
right for diagnosing production and exactly blind in the suite, and knowing which
of those you are relying on matters when a reading looks wrong.

## The class came back as data

Session labels are sanitized: truncated, non-printable bytes replaced, and an
empty result replaced by a placeholder.

That last clause exists because the consumer treats **bytes written** as its
overflow signal, so a zero-length field is indistinguishable from a full buffer
and aborts the entire listing.

**The history is the finding.** That collision was found and fixed once, as a
literal empty string in a conditional expression. It came back here **as data** —
a label that happens to be empty at runtime rather than a constant written into
the source. And the input that produces it is ordinary: the connecting-service
path usually has an empty attach name, which is precisely why that path now
stamps the peer's process id instead.

**Fixing the instance did not fix the class**, and the second instance was
unreachable from the first one's shape. A search for the fixed defect — an empty
string literal routed through the formatter — finds nothing here, because there
is no literal. The producing expression is a field copy.

The defence is now at both ends, which is the correct response to a class that
has demonstrated it can re-enter from a direction the first fix did not face. The
consumer guard covers what it receives; the producer guard covers what it can
generate. Neither alone would have held.

Worth pairing with the width rule from [[chg-2026-08-16-hwcap-widths]] and the
prosecution line rewritten in [[chg-2026-08-16-exception-descent-guard]]: three
times in one sweep, a defence written over the **instance** read as covering the
**class**. Here it is the first time the class actually came back to prove it.
