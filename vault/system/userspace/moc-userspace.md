---
id: moc-userspace
type: moc
title: "Userspace"
parent: home
created: 2026-07-31
updated: 2026-08-03
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

## Services

- [[sub-ptyfs]] — the pseudoterminal server: the pts pairs, the per-pts
  line discipline, and the teardown algebra. Holds the userspace half of
  [[inv-i20]].
- [[sub-tapestryd]] — the compositor: the weave lifecycle, the present
  engine, and the retire ordering. Holds the server half of
  [[inv-i40]].

## Cross-cutting

- **The three native 9P servers share a template, and the template's
  fixes did not all travel.** netd, [[sub-ptyfs]] and [[sub-tapestryd]]
  are the same shape — a Conn/fid table, a frame extractor, a qid scheme
  keyed on bit 40, deferred replies with a four-site cancel discipline,
  and a single-threaded loop whose top-of-loop delivery pass *is* the
  I-9 argument. tapestryd's header says so explicitly. But netd's
  `net-4d` F2 fix (reject `P9_NOFID` as a newfid; reject a newfid
  already in use) reached ptyfs and only half-reached tapestryd — see
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
