---
id: chg-2026-09-06-ninep-codec-absorb
type: chg
title: "docs/reference retirement: absorb 88-ninep (the userspace libthyla-rs 9P codec, NOT the kernel client) -- fold the codec invariants into sub-libthyla-rs, then stub (55 absorbed / 102 live)"
date: 2026-09-06
arc: arc-vault
commits: []
touched: [sub-libthyla-rs]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
First 9P-area file, and a verify-before-stub catch: 88-ninep's title and its
citations point at the kernel 9P client, but its SUBJECT is the userspace
`libthyla_rs::ninep` serving codec (`usr/lib/libthyla-rs/src/ninep.rs`) -- it
cites kernel/9p_client.c only as the wire reference. The owner is sub-libthyla-rs
(audit:light), not sub-kernel-ninep-client. The title-isn't-the-subject trap,
caught by reading the doc rather than trusting the grep of its citations.

sub-libthyla-rs is a BROAD runtime dossier and carried NO ninep content (1 hit,
the code: list entry). Folded a focused "The 9P serving codec" subsection: the
server-side-only shape (no session state/fid table/tag alloc -- those are the
server's; client side deferred), the codec invariants (pack/unpack identity, no
over-read/over-write, back-patched size, parse_twalk's double bound), and the
load-bearing distinction that the 9P SESSION invariants I-10/I-11 are
server-state enforced ABOVE the codec, NOT codec invariants. The constants /
message-type numbers / struct layouts stay code-authoritative (ninep.rs), per the
duplication-rot rule.

Stubbed with honest drift: the doc's Status still carries an unfilled
"commit *(pending)*" placeholder; it is largely an API+constant reference (code's
to pin). No code touched; no audit owed. sub-libthyla-rs already
updated:2026-09-06. view-absorption: 54 -> 55 absorbed, 102 live.
