---
id: fnd-b1d-v-r2-f1
type: fnd
title: "Every /srv/<name> node carried the registry root's qid.path, so a mount at one was keyed at the root and at every other service"
round: adt-b1d-v-r2
severity: P2
status: fixed
surface: [sub-kernel-devsrv, sub-kernel-territory]
threatens: [inv-i3]
fixed-by: chg-2026-09-25-b1d-v-emount
regression: "devsrv.service_keys_distinct"
created: 2026-09-25
---
## Prosecution

The mount key is `(dc, devno, qid.path)`. `devsrv_attach_registry` gives a
registry root `qid.path` 0, and `devsrv_walk` gave every service node
`qid.path` 0 as well; a node is a clone of the root, so it also shares the
root's `dc` and `devno`. Any EL0 Proc could MREPL a readable file over
`/srv/<name>`: file over file passes B-1d-v's check, and the entry was keyed
exactly where the registry root is. The next resolution through `/srv`
crossed into the registry root and then hopped through that entry into the
file, so `/srv` opened the file and every `/srv/<x>` answered `ENOTDIR`, after
a `SYS_MOUNT` that returned 0. After a chroot to `/srv`, an `MBEFORE` of a
directory at `/` started a union at the same key, which put two members at
every service node, a point that is not a directory. The damage stays in the
caller's own namespace (I-1 holds) and the use-time gates refuse the bad
paths, but the mount key stopped naming one point -- the identity I-3's cycle
check and the resolver both rest on -- so two unprivileged calls defeated the
guarantee B-1d-v documents.
The alias dates from stalk-3a; Plan 9's devsrv gives each `srvcreate` its own
path.

## Disposition

Fixed: a per-registry counter stamps each reservation's `qid_path`, never 0,
and `devsrv_walk` reads the name, the LIVE state and the path in one hold of
the registry lock, since a tombstoned slot can be recycled under another name.
A new post of a name gets a new path, so a mount at the old post does not
carry over. `devsrv.service_keys_distinct` checks the identities, that an
MREPL over `/srv/a` shows at `a` alone, and that a union at the registry root
leaves `/srv/b` its service node.
