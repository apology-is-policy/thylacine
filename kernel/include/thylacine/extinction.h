// Kernel extinction — single-line print with TOOLING.md §10 ABI prefix,
// then halt forever. Named for the thylacine's own fate: when the kernel
// dies, that boot's lineage is extinct. The line is unrecoverable; only
// a fresh boot continues the species.
//
// The string "EXTINCTION: " is the agentic-loop's catastrophic-failure
// signal. It must be the first 12 bytes on the line emitted by
// extinction(). Per TOOLING.md §10:
//
//   "Any output matching /^EXTINCTION:/ on the UART stream triggers the
//    agent to: record the message, restore the last good snapshot, and
//    report to the human before retrying."
//
// Don't change the prefix without coordinated updates to
// tools/run-vm.sh, tools/test.sh, tools/agent-protocol.md, CLAUDE.md,
// and TOOLING.md.

#ifndef THYLACINE_EXTINCTION_H
#define THYLACINE_EXTINCTION_H

#include <stdint.h>

// Print "EXTINCTION: <msg>\n" to UART and halt forever. Never returns.
void extinction(const char *msg) __attribute__((noreturn));

// Print "EXTINCTION: <msg> 0x<addr>\n" and halt. Convenience for fault-
// handler callers that want to include a faulting address.
void extinction_with_addr(const char *msg, uintptr_t addr)
    __attribute__((noreturn));

// Convenience macro for assert-style checks. The expression that fails
// shows up in the message verbatim so a developer can grep the source.
#define ASSERT_OR_DIE(expr, msg) do { \
    if (!(expr)) extinction(msg ": " #expr); \
} while (0)

#endif // THYLACINE_EXTINCTION_H
