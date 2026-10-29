# Controller lifecycle self-review

Author review only, not an independent audit.

R1 (open during review): foreground epoch alone does not describe every terminal
nomination change. Kernel ACK can replace a nominated subject at the same epoch.
The adapter's retirement input must also carry that subject (zero for no valid
nomination), and refuse retention when it differs. Add a same-epoch changed-subject
schedule before closing this finding. The new table is not yet activated.

R1 repair: `terminal_state` now requires epoch and subject; zero subject retires
unacknowledged/dead nominations. Named schedule and dropped-subject mutant added.

R2 harness: first mutation run correctly failed `wrong-peer` at the test's
custom `field 2` assertion, but the harness incorrectly required the word
`assertion` in its message. Accept this exact named witness for that mutant;
compilation failure still cannot count. Preserve the failed harness log.

Final review: R1 and R2 closed by the revised 37-test actual-source run and all
eight intended named mutant failures. Final canonical host suite: 487/487;
aarch64-unknown-none library check passes. No locks, borrowed I/O or allocation
in this table. Pending publication owns only values. Retire removes the entry
before its synchronous cancellation callback; a replacement cannot register
during this single-owner call. Global generation/request histories survive
retirement. Failed reports cannot mutate labels or advance sequence state.

Remaining integration obligations are explicit in the controller design and
status: live peer samples, ordered terminal/focus notifications, HIA request
sequencing and timeout/drain, application replies and their cancellation, total
resource ledger. The core is not instantiated in production yet. No visual,
full-system, SMP, sanitizer or independent audit claim.
