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

// For exception.c's el1_sync_runaway -- the one OTHER emitter of the ABI line.
// Claims the console, or confirms this CPU already owns it (the runaway is
// reached from a chain that may have claimed it at depth 1). Returns 0 iff a
// PEER holds it: the caller must then park WITHOUT emitting (it is counted as a
// suppressed peer). Not for any other caller.
int extinction_console_claim_or_own(void);

// The pure exactly-one-winner core, exported so a test can exercise it on its
// OWN word. Deliberately not reachable against the live console word: a test
// that claimed THAT would disable extinction reporting for the rest of the boot.
int extinction_claim_word(uint32_t *word);

// The console gate every direct UART byte passes (uart_putc). Enter returns 0
// when a PEER owns the console: the caller drops the byte and does not call exit.
// Otherwise the byte is in flight until extinction_tx_exit, which takes enter's
// token: HELD when IRQs were on and the thread's preempt count is held across
// the byte, BYTE when IRQs were masked and no thread state was touched.
#define EXTINCTION_TX_BYTE 1
#define EXTINCTION_TX_HELD 2
int  extinction_tx_enter(void);
void extinction_tx_exit(int token);

// The gate's pure cores, exported so a test can drive them on its own words (the
// live ones cannot be claimed without silencing the rest of the boot). gate:
// raise `busy` by one, then admit iff `word` is unclaimed or `owner`; on refusal
// `busy` drops back. exit: drop it by one (a depth, so a nested byte's exit
// leaves the outer byte marked). wait: wait out every busy[c], c != self, under
// a bound; 1 iff every flag reached zero in time.
int  extinction_tx_gate_core(volatile uint8_t *busy, const uint32_t *word, uint8_t owner);
void extinction_tx_exit_core(volatile uint8_t *busy);
int  extinction_tx_wait_core(const volatile uint8_t *busy, unsigned n, unsigned self);

// Convenience macro for assert-style checks. The expression that fails
// shows up in the message verbatim so a developer can grep the source.
#define ASSERT_OR_DIE(expr, msg) do { \
    if (!(expr)) extinction(msg ": " #expr); \
} while (0)

#endif // THYLACINE_EXTINCTION_H
