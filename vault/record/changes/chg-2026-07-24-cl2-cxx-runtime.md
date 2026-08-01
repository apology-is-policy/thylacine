---
id: chg-2026-07-24-cl2-cxx-runtime
type: chg
title: "Clade CL-2: remove(3) by lstat-dispatch"
date: 2026-07-24
arc: arc-clade
commits: ["11f56b80"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The C++ runtime landing brought one pouch patch with it.
`std::filesystem::remove` uses `::remove`, and musl's `remove` relies on
the kernel answering `-EISDIR` so it can fall through to `rmdir` --
which Thylacine's `SYS_UNLINK` cannot, collapsing every failure to a flat
`-1` (the #102-class errno loss). 0027 dispatches on an `lstat` instead:
a directory goes to `rmdir`, anything else to `unlink`.
