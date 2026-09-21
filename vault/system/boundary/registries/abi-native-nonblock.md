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
created: 2026-09-18
updated: 2026-09-18
---
## Contract

Native 123 SET_NONBLOCK takes fd and on (0 or 1). It sets or clears CNONBLOCK
on the owned Spoor's shared open-file description, including aliases created by
dup. Success is 0; invalid fd range or mode returns EINVAL, and a missing/non-Spoor
handle returns EBADF. It grants no new handle rights. Individual Devs implement
nonblocking behavior; this call does not promise that arbitrary device methods
are asynchronous. [[sub-kernel-devsrv]] honors it for raw /srv transport endpoints.

This append raises the native dispatch ceiling to 123. The existing Linux
phenotype translation handles its own numbered calls before native dispatch.
