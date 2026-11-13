---
id: chg-2026-11-05-cooked-flush-refused
type: chg
title: "ptyfs: a cooked line that does not fit is refused whole"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-ptyfs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
A newline whose line and terminator did not both fit in `m2s` pushed what fit, lost the tail and then the newline, and reset the line, so the head ran on into the next line after the echo had shown them apart. The console stopped doing this in `#129`; ptyfs's text still called it the console's posture. A flush now goes whole or not at all: nothing moves, nothing echoes, the line stays assembled, and the write returns short, so the writer re-offers the newline. `kaua-term` and `ptyhost` already wait out a count of 0, as raw mode needs, for 200 ms, and then drop the rest of that buffer: the loss is the writer's, and it stays visible, since the line on the screen is the line the reader gets. `^C` is not held up. `drop_flush` had nothing left to count and is gone. Battery step 12 pins it, and `pty-mut.py`'s rows turn it red both ways on a host.

Running the battery on a host for the first time found two steps left stale by the previous change: the read that took two lines, and the control byte's echo. Either would have failed the boot.

`pty.tla`'s `CookData` already requires ring room, one char at a time, so the model is unchanged: the code moved to the model. A mode write still drops, having no writer to refuse, under `drop_modeflush`.
