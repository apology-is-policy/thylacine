---
id: fnd-b1c-r1-f2
type: fnd
title: "A reader that left was an error: grep -l's early stop failed its producer under ut's pipefail"
round: adt-b1c-r1
severity: P2
status: fixed
surface: [sub-libthyla-rs, sub-coreutils-filters, sub-coreutils-presenters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round1-close
regression: "coreutil-smoke's reader-leaves checks (grep, cut, cat, tee, uniq): status 0 and nothing on stderr; the status half sabotaged (grep's guard, OutSink::finish, cat's and tee's arms) RED by name in all five"
created: 2026-09-24
---
## Prosecution

`grep -l` stops reading at its first match, as GNU's does. Its producer's next
write then finds the reader gone: `cat` reported "broken pipe" and exited 1, so
`cat big | grep -l x` failed under ut's pipefail, which takes the rightmost
non-zero status. Every `OutSink` filter reported a gone reader as a write error
the same way. HT09.RND2-F2 had registered the policy as follow-up #54, blocked
because the kernel returned -1 rather than `-T_E_PIPE`; #100 has since made the
errno distinguishable, so nothing blocked it any more.

## Disposition

Fixed (#54). `OutSink` records whether its failed write found the reader gone,
and `finish(prog, status)` keeps the status and prints nothing for a gone reader
while reporting any other failure (`prog: write error`, exit 1). All nineteen
filters follow it: cat, cut, grep, head, hexdump, ls, ns, pelt, ps, qid, realm,
realpath, sort, stat, tail, tee, tr, uniq and wc (`yes` and `seq` already did).
ut's `?|` remains the idiom for a producer that genuinely fails.
