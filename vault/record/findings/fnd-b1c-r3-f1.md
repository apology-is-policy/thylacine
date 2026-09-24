---
id: fnd-b1c-r3-f1
type: fnd
title: "The smoke's run_tool reaped before it read, with no bound: an output past a pipe's 4 KiB, or a tool that never ends, hung the boot"
round: adt-b1c-r3
severity: P2
status: fixed
surface: [sub-coreutils-filters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round3-close
regression: "coreutil-smoke 'the capture carries more than a pipe holds' (the boot hangs there on the reap-first capture) and 'the capture ends a tool that never does' (RED by name when a tool cut off at the bound is reaped for its own code rather than reported as cut off: yes, its pipes dropped, exits zero; the kill-before-drop order alone no check can see, since the cut-off path reports None either way)"
created: 2026-09-24
---
## Prosecution

The smoke's `run_tool` wrote a tool's whole input, waited for the tool with no
bound, and only then read its stdout. An output had to fit the 4096-byte pipe,
or the tool blocked writing it and the wait never returned; joey waits for the
smoke with no bound, so the boot hung where a check should have failed, with no
FAILED line. Round 2's `grep -o every match of a line` expected 3000 bytes, a
margin of 1.37x: a regression that styled the plain arm, numbered each match or
printed the line instead would have hung the boot. The long inputs (18 and 24
KB) had the same shape on the other side: a tool that neither read its input nor
exited blocked the write before the wait was reached. Reaping first had not been
needed since #68 closes a child's handles at its exit. The order predates B-1c;
round 2 added the thin-margin check.

## Disposition

Fixed. Every check runs through one capture, `converse`. The tool's stdin is made
non-blocking, and one poll over stdin's write side and stdout's and stderr's
read sides feeds the input as the pipe takes it and reads both outputs as they
come, keeping 64 KiB of stdout and 300 bytes of stderr, all within the check's
bound. A tool still running at the bound is killed while its pipes are open, so
it is reported as cut off, never as whatever losing its reader made it do; one
that finished is reaped within what is left of the bound. The re-run that
reported a failed check's stderr is gone, since the one run keeps it, and so are
the `ps` checks' pipe-budget gates. Two checks hold the capture to this: `cat`
carries 18.9 KB in and out, on which the reap-first capture deadlocks, and `yes`
is cut off at one second with its lines kept.
