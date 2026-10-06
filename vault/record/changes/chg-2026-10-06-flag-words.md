---
id: chg-2026-10-06-flag-words
type: chg
title: "The flag-word check: every bit-allocated word, not only proc_flags, derived from its header and self-tested"
date: 2026-10-06
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
Two branches each took bit 22 of `proc_flags` and both compiled, because each
flag's `_Static_assert` names only the flags its author knew. Main's
`tools/check-proc-flags.py` (1032ac49) derived that one word's set from
`proc.h`; `tools/check-flag-words.py` replaces it with a table of nine words --
`proc_flags`, the spawn permission word and the four one-bit spawn words, the
walk-create mode word, the 9P attach flags and the mount flags
([[sub-substrate-build]]). A member owns the bits its own literals contribute,
so a mask built from members overlaps them and a literal mask owns its bits; a
member that uses another other than under `|` cannot be classified and fails.
Before it reports a pass, it mutates each word's header in memory five ways and
stops the build if any mutation goes unreported by the rule it targets, the
idiom of `tools/check-spawn-args-mirrors.py`. Main agreed on yip 0172 that aux
takes it.
