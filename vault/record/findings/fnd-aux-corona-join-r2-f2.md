---
id: fnd-aux-corona-join-r2-f2
type: fnd
round: adt-aux-corona-join-r2
severity: P3
status: fixed
title: "check-lock-helpers missed a client lock through a header member, parenthesised, or in a header"
surface: [sub-kernel-poll, sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-10-29-aux-corona-join
regression: "check-lock-helpers --self-test; each shape missed by the previous checker and flagged now; the self-test goes red with header scanning removed"
created: 2026-11-02
---
## Prosecution

Rule C named only `struct p9_client *` locals of the same file and read no header, so `&priv->client->lock`, `&(cl->lock)` and a header inline passed. Fixed in 009d20767: any `NAME->lock` in any spelling for a name the file declares that type, for a member a header declares that type (after `->` or `.`), or through a cast; headers are read. The names derive from the declarations. The reviewer's alternative, renaming the field to a unique token, would have changed production code after the SMP gate ran.
