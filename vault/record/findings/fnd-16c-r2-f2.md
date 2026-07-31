---
id: fnd-16c-r2-f2
type: fnd
title: "install_transport-failure path leaked the adapter struct"
round: adt-16c-r2
severity: P2
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

On the defensive install-fail path, `p9_attached_unref` dropped the
srvconn ref via close, but `attached_destroy_inner`'s adapter block was
skipped (a->adapter never set) -- the kmalloc'd adapter leaked.

## Disposition

Fixed: explicit destroy + kfree after the unref on that path. The same
shape survives in today's `srvconn_attach_dev9p_root` install-fail arm.
