---
id: moc-userspace
type: moc
title: "Userspace"
parent: home
created: 2026-07-31
updated: 2026-08-04
---
The native + ported userspace tree: the boot chain (joey, corvus, login,
warden), the services (netd, stratumd-facing proxies, ptyfs, tapestryd),
the shell/TUI stack (ut, kaua, nora, aurora), the runtime libraries
(libthyla-rs, libdriver, netdev, tls), and the ports plane (pouch + the
Go fork + clade). Orientation only — the facts live in the `sub-*`
dossiers.

## Children

- [[moc-userspace-netd]] — the network daemon (the first userspace area;
  other area MOCs land with the per-subsystem sweep).
- [[moc-userspace-runtime]] — the libraries a native program links rather
  than the programs themselves. Not a privilege boundary, which is what
  makes its notes read differently: they describe discipline a library
  applied on its own initiative, and the failure mode of a plane where
  a wrong claim costs nothing until someone reads it.
- [[moc-userspace-shell-tui]] — what a person actually touches. Spans the
  widest range of risk in the tree, from a parser that touches nothing to
  a raw-mode handoff the user cannot type their way out of.
- [[moc-userspace-boot-chain]] — the Procs that run before there is a
  session, whose common shape is that each is spawned holding an authority
  it exists to give away. The one plane where the kernel's
  conferred-within-conferrer rule is vacuous by construction, because being
  early means holding everything — so its notes are about arithmetic nobody
  re-derives rather than about gates.
- [[moc-userspace-hardware]] — the programs that reach a device. None is a
  service and none has a user: each is spawned by something above it, does
  one bounded job, and reports through a channel that party chose. Splits
  into the drivers that take their own authority and the two that are
  granted it, which makes it the tree's clearest before-and-after on
  [[inv-i34]].

## Services

- [[sub-ptyfs]] — the pseudoterminal server: the pts pairs, the per-pts
  line discipline, and the teardown algebra. Holds the userspace half of
  [[inv-i20]].
- [[sub-tapestryd]] — the compositor: the weave lifecycle, the present
  engine, and the retire ordering. Holds the server half of
  [[inv-i40]].
- [[sub-corvus]] — the key agent: authentication, key custody, elevation
  and recovery in one daemon, bounded by [[inv-i23]] to the storage
  capability it is handed. The oldest of the five servers and the one the
  others' shared codec was lifted out of.
- [[sub-diorama]] — the synthetic Linux world: a read-only tree presenting
  native state in the shapes an unmodified Linux binary expects. Holds
  [[inv-i43]], whose whole content is that it must reformat and never
  become an authority — the newest server and the only one whose design
  property is an absence of privilege.

## Cross-cutting

- **Five native 9P servers share a template, and the template came from
  the oldest of them.** netd, [[sub-ptyfs]], [[sub-tapestryd]],
  [[sub-corvus]] and [[sub-diorama]] are the same shape — a Conn/fid
  table, a frame extractor, a qid scheme, and a single-threaded loop
  whose top-of-loop pass *is* the I-9 argument. The shared 9P codec the
  others link was **lifted out of corvus's private module**, which
  inverts the usual reading of a shared template: a fix that reached the
  library reached the descendants, and a fix that reached only a
  descendant never came back to the ancestor. (corvus and diorama both
  differ in one respect: they answer inside the dispatch rather than
  deferring a reply, so neither has a held-tag cancel discipline to get
  wrong.)
- **A second fix travelled one hop and stopped, and the two ends are now
  both swept.** A full connection table plus a pending connection keeps
  the listener perpetually readable, so an accept loop that merely skips
  the accept spins at full CPU. [[sub-diorama]] drops the listener from
  its poll set while full and cites the audit finding that taught it;
  [[sub-corvus]] does not, and its comment calls the situation a benign
  deferral (task #149). Same defect, same family, one sibling fixed —
  which is the third instance of this shape in the userspace sweep.
- **The template's fixes did not all travel.** netd's `net-4d` F2 fix
  (reject `P9_NOFID` as a newfid; reject a newfid already in use)
  reached ptyfs and only half-reached tapestryd — see
  [[chg-2026-08-02-server-sweeps]]. The same two lines are load-bearing
  in ptyfs, where they are link 3 of `HupAtMostOnce`'s structural
  argument, and absent in its descendant.
- **A deferred reply is a held tag, and every path out must cancel it.**
  All three servers park a reply and deliver it from the loop top; all
  three must drop it at conn death, clunk, `Tversion` and `Tflush`. A
  missed site strands a tag, which is netd's `net-4d` F1 class.

- The native/ported split (ARCHITECTURE.md section 3.5): authored-within
  → libthyla-rs, ported → pouch. Every service here is native unless its
  dossier says otherwise; the ported half's translation layer is
  [[moc-pouch-seam]], which lives on the boundary plane because both
  halves must agree on it.
- Kernel boundaries these Procs stand on: [[moc-kernel-srv]] (post/
  connect), [[moc-kernel-ninep]] (the mounts that reach them).
