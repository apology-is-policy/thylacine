---
id: chg-2026-09-06-dev-fdclass-console-helpers
type: chg
title: "sub-kernel-dev de-stale: dev.h gains the devdev_fd_devclass / spoor_is_console decls + the extern devcons (the header side of the devdev fd-class + console-identity work)"
date: 2026-09-06
arc: arc-vault
commits: ["d102f449"]
touched:
  - sub-kernel-dev
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-dev]] (updated 2026-08-16) -- the Dev vtable + bestiary header --
had `dev.h` change under H-1a `7cd1ab94` + viv-C2 `1890283c` (both 2026-09-01,
settled). The changes are the HEADER side of the devdev work folded into
[[sub-kernel-devdev]] this run, so this is a small currency note, not a
re-documentation:

- `extern struct Dev devcons;` joined the extern-declared Dev set (the console
  door, dc='c').
- `int devdev_fd_devclass(const struct Spoor *)` and `bool spoor_is_console(struct
  Spoor *)` are now declared in the shared header -- their semantics (the
  SYS_FD_DEVCLASS effective-class query and the unforgeable-Dev-pointer console
  identity) live in the devdev dossier; they sit here because callers outside
  devdev use them.

Folded one Caveats bullet tying them to the dossier's existing theme -- a Dev's
identity is its `dc` and its pointer, neither a bit a 9P server can set. Not the
vtable or the bestiary; those are unchanged. `updated:` -> 2026-09-06. Stale
backlog 27 -> 26.
