---
id: chg-2026-07-24-getcwd-oversized
type: chg
title: "getcwd accepts an oversized buffer (the PATH_MAX reject)"
date: 2026-07-24
arc: arc-clade
commits: ["ad9e4a72"]
touched:
  - sub-kernel-territory
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
`sys_getcwd_handler` rejected any `buf_len > SYS_OPEN_PATH_MAX + 1` with
`-1`. That is the near-universal `getcwd(buf, PATH_MAX)` idiom — GNU
make, clang, git, configure scripts — so the reject fired on the common
case and surfaced as `make: getcwd: I/O error` under the CL-1c on-device
`make` gate.

The fix inverts the order: compute the cwd FIRST into a kernel scratch,
then require only that `len + 1` fits the caller's buffer, and copy
EXACTLY that many bytes — never the whole buffer. That is also what
POSIX actually specifies ("writes at most the pathname plus NUL"), and it
incidentally removes the range check a huge `buf_len` could have
overflowed.

Two lessons. The bound was placed on the wrong quantity: an ARGUMENT was
validated against a limit that belongs to the RESULT, and the two are
unrelated — a caller passing a generous buffer is doing nothing wrong.
And the finder is the point: no hand-written probe passes `PATH_MAX`,
because a probe author picks a buffer that fits. It took a real
toolchain, which does what real programs do, to drive the idiom at all.
