---
id: abi-native-nonblock
type: abi
kind: registry
stability: append-only
title: "Native shared open-file nonblocking mode"
pinned-by:
  - "VIV_NATIVE_CEILING == SYS__NATIVE_TOP - 1"
mirrors:
  - kernel/include/thylacine/syscall.h
  - kernel/syscall.c
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/lib.rs
  - kernel/include/thylacine/vivarium.h
  - usr/lib/pouch/patches/0056-pouch-dup-fcntl-status.patch
created: 2026-09-18
updated: 2026-10-15
---
## Contract

Native 123 SET_NONBLOCK takes fd and on (0 or 1). It sets or clears CNONBLOCK
on the owned Spoor's shared open-file description, including aliases created by
dup. Success is 0; invalid fd range or mode returns EINVAL, and a missing
handle returns EBADF. A live handle that is not a Spoor answers 0 and changes
nothing (`handle_set_nonblock`); Pouch's F_SETFL relies on that, since it
sets the flag on whatever native descriptor it is handed. It grants no new
handle rights. Individual Devs implement
nonblocking behavior; this call does not promise that arbitrary device methods
are asynchronous. [[sub-kernel-devsrv]] honors it for raw /srv transport endpoints.

Pouch reaches it as `__NR_thyla_set_nonblock` 123 (patch 0056: F_SETFL and
`pipe2(O_NONBLOCK)`).

This append raises the native dispatch ceiling to 123. The existing Linux
phenotype translation handles its own numbered calls before native dispatch.
