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
// Don't change the prefix without updating everything that mirrors it.
// The authority for that set is the vault's abi-boot-banner note, reached
// via `quaestor owner` -- not a list transcribed here. The list that used
// to sit on this line named tools/agent-protocol.md, planned in Phase 1
// and never written, and tools/run-vm.sh, which matches neither literal
// because it only launches QEMU and never reads boot output.

#ifndef THYLACINE_EXTINCTION_H
#define THYLACINE_EXTINCTION_H

#include <stdint.h>

// Print "EXTINCTION: <msg>\n" to UART and halt forever. Never returns.
void extinction(const char *msg) __attribute__((noreturn));

// Print "EXTINCTION: <msg> 0x<addr>\n" and halt. Convenience for fault-
// handler callers that want to include a faulting address.
void extinction_with_addr(const char *msg, uintptr_t addr)
    __attribute__((noreturn));

// Has the extinction console been claimed? FALSE on a clean boot, and it must
// STAY false: a spurious claim would silence every later extinction's banner --
// the same fail-open the claim exists to close, arriving from the other side.
// Guarded by the `extinction.console_unclaimed_on_clean_boot` test.
int extinction_console_claimed(void);

// The pure exactly-one-winner core, exported so a test can exercise it on its
// OWN word. Deliberately not reachable against the live console word: a test
// that claimed THAT would disable extinction reporting for the rest of the boot.
int extinction_claim_word(uint32_t *word);

// Convenience macro for assert-style checks. The expression that fails
// shows up in the message verbatim so a developer can grep the source.
#define ASSERT_OR_DIE(expr, msg) do { \
    if (!(expr)) extinction(msg ": " #expr); \
} while (0)

#endif // THYLACINE_EXTINCTION_H
