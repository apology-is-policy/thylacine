---
id: seam-845-untrusted-server
type: seam
title: "One-reply-per-tag trust envelope (untrusted-server tag generations)"
status: open
surface: [sub-kernel-ninep-client]
opened-by: fnd-845-r1-f1
tracker: "v1.x (the n_uname trust-stamp seam family)"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

The client trusts the server to send exactly one reply per tag. A
non-conformant/malicious server duplicating a reply onto a since-reused tag
mis-attributes it to the new op — for ANY op kind (a duplicate Rflush after
the flush tag's reuse prematurely frees the new flush's reserved oldtag; a
duplicate Rlerror parses cleanly for any outstanding kind). 9P carries no
per-tag generation, so this is wire-indistinguishable client-side; the
in-tree guard set is already maximal (`awaiting_flush` only ever on a
reserved normal op; bounds + active + type checks). #375 removed the CLIENT
as a duplicate source — with trusted stratumd/dev9p the in-tree
self-duplicate source set is now empty.

## What closes it

Wire-level tag generations — a v1.x ABI lift gated to the untrusted/remote
9P server path (no untrusted server exists at v1.0; every server is a
trusted local Proc).

## Risk while open

None at v1.0 (trust boundary holds by construction). Becomes load-bearing
the day a remote/untrusted 9P mount lands — that chunk must pick this seam
up in its design pass, not discover it in audit.
