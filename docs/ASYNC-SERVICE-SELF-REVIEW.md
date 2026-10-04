# Async service self-review

Single-agent review, as directed by the operator. This is not an independent audit.

## AS-0: reserved encodings and lifecycle model

Scripture4722f34e8 and numeric reservationd2362ec11 precede the consumers.
The kernel, native C and Rust records pin every field offset and total size.
`tools/check-loom-service-abi.py` compiles actual mirrors and compares their
encoded bytes with independently constructed vectors: 22 constants, five
records, signed fd/error fields and nontrivial 64-bit incarnations. The C
layouts also compile for ARM64. Mutating kernel opcode, native C scope kind
or Rust setup flag fails the expected mirror comparison in all three cases.
The full consuming kernel header compiles for ARM64; static assertions confirm
PRIVATE_SERVICE remains outside LOOM_SETUP_VALID and LOOM_OP_COUNT stays20.
There is no runtime dispatch or authorization change in this checkpoint.

`specs/check-loom-service.py` explores5,828 states, depth28. Seven mutants
violate their named properties: premature free, success committed after abort,
double terminal, stale incarnation, CQ overflow, credit refund while a peer
retains its endpoint, and retirement waiting for peer progress. Peer progress
has no fairness premise. Local execution and CQ draining are weakly fair;
closing the ring discards delivery obligations only after local borrows end.

Completion commit and CQ delivery are separate. A success committed before
abort can be delivered later without reviving the scope. The design now says
this explicitly; it does not permit deciding new success after the abort latch.

Evidence: work/oct4-async-service/{abi.log,as0-verification.json,model-gate/}.
The first full-header fixture missed arch/arm64 on its include path; the saved
failure is a compile-command defect, corrected before its successful check.
The earlier model precedence and Vault mirror-count fixture errors are retained
in abi-reservation-fixture-note.md. None counts as an intended mutant failure.

Limits: the abstract model does not prove actual C locks, byte framing, DAC,
usercopy, Proc/AddrSpace ownership or runtime retirement. Those are AS-1/2 gates.
This checkpoint adds declarations and verification only: no new boot, SMP,
sanitizer, graphical, Pi or minimum-display qualification is claimed.
