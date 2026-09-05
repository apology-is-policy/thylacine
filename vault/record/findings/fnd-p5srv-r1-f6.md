---
id: fnd-p5srv-r1-f6
type: fnd
title: "The post/connect handlers skipped sys_validate_user_buf before their per-byte copy loops"
round: adt-p5srv-r1
severity: P2
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-05-19-srv-birth
created: 2026-07-31
---
## Prosecution

`sys_post_service_handler` and `sys_srv_connect_handler` copied the
user-supplied name (and path) per-byte via `uaccess_load_u8` without
first calling `sys_validate_user_buf` — correctness-equivalent (the
per-byte fault fixup catches a bad VA) but divergent from the discipline
every other user-buffer handler follows, and a divergence is where the
next refactor introduces the real bug.

## Disposition

Fixed: both handlers pre-validate the range before the copy loops.
Historical: both syscalls were later RETIRED at
[[chg-2026-06-03-stalk3c-retire]]; the discipline itself carries on in
the surviving `sys_srv_peer_handler` (validate-before-store).
