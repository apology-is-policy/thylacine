---
id: dec-2026-09-28-remote-label-carrier-r2
type: dec
title: "A remote mount's label rides the 9P session (restated; the Fork's ns claim corrected)"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-haul, sub-kernel-territory, sub-kernel-devsrv, sub-kernel-ninep-attach, sub-coreutils-presenters]
created: 2026-09-28
supersedes: dec-2026-09-28-remote-label-carrier
---
## Fork

This note restates [[dec-2026-09-28-remote-label-carrier]]. The decision is
unchanged. That note's Fork said the operator's `ns` called the Haul mount's
source `disk`; the operator reported `la` alone, and the claim was a reading of
`ns.rs` (`#9` -> `disk`) on the wrong premise that a 9P session root has no
name. `dev9p_attach_client` names every session root `/` at birth, so an
unlabelled Haul mount read source `/` and REALM `fs`. The first boot of the LR-1
tests showed it (`dev9p.remote_format_ns` rendered `mount /m /`).

The operator listed a Haul mount's parent with `la` and saw REALM `fs`, the
same as every directory beside it. The first vote (2026-09-24) gave a mount
point a REALM of its own, `remote` for a network mount and `mount` for a local
one: Haul declares its mount remote and the kernel carries the declaration.
That vote signed off a change to the mount syscall to carry it.

Designing it (LR-1) showed that a mount-syscall flag is right in one of the
three documented flows alone. Where the declaration rides is a syscall
interface change, so it went back to the operator.

## Research

- **The three flows** (`docs/HAUL-DESIGN.md` 4.8, `docs/manual/14-remote-files.md`).
  `haul HOST!PORT PATH` attaches and mounts in Haul. `haul --post NAME
  HOST!PORT`, then the shell's `mount /srv/NAME PATH`, is the manual's primary
  example: the SHELL attaches (`SYS_ATTACH_9P_SRV`) and mounts, and never
  learns what is behind the service. A directory inside a remote mount, mounted
  elsewhere, is a Spoor of the same session.
- **Heritage.** Plan 9 needs no declaration: `import` and `srv` mount the
  network connection itself, and `ns` names each mount by its channel, so a
  network mount reads as a `/net` connection file or a `/srv` entry named after
  its dial string. Thylacine's kernel has no TCP, and Haul's pipes hide the
  connection.
- **Precedent in the tree.** The identity cape already travels from the program
  that created the session: `SYS_ATTACH_9P_CAPE` on the private attach,
  `DMSRVCAPE` on the post, inherited by every attach over the service.

## Options

1. **The 9P session.** Bit `0x4` in `SYS_ATTACH_9P`'s flags (the private
   attach) and `DMSRVREMOTE`, bit 22, on a `/srv` post. The kernel stamps the
   session once, before its root publishes; `/proc/<pid>/ns` renders ` remote`
   on each member entry whose source belongs to it, never on a union's covered
   entry. Labels all three flows.
2. **A `SYS_MOUNT` flag,** the change the first vote signed off. Labels the
   private form only; the posted-then-mounted flow and a remote subtree read
   `mount`.
3. **Park the label** and land the `ns` fixes alone.

## The call

Option 1 (operator, 2026-09-28). `SYS_ATTACH_9P_SRV` refuses the bit: over a
service the poster declares. The declaration is part of a service's identity on
a tombstone rebind, and either service mode admits it. It is display only:
`territory_format_ns` is its one kernel reader. The `ns` changes that ship with
it (the caller's namespace by default, a FLAGS column, `9p` for a `#9` source)
are the implementer's, not part of either vote.

## Rationale

The declaration belongs where the fact is known. Only the program that created
the session knows where its bytes go, and a label passed at mount time would be
wrong in the manual's primary flow, whose mounter is the shell.
