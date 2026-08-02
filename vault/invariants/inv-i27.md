---
id: inv-i27
type: inv
title: "I-27 — the trusted path: an unspoofable attention key, and three console roles that never substitute"
number: I-27
guards: [sub-kernel-cons, sub-kernel-devdev]
validated-by: [prose, gate-interactive, gate-smp]
strength: prose
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A user must be able to reach an authority they can **prove is the real one** —
so that a passphrase typed at an elevation prompt cannot be captured by
whatever program happens to be running.

Three clauses carry it.

- **The attention key is unforgeable.** The secure-attention signal is a
  *line condition* on the serial link, not a byte sequence, so no EL0 program
  and no injected input stream can synthesize it. It is recognized
  unconditionally — no mode flag, no capability, no configuration disables it.
- **On the attention key, the console's elevation authority is revoked from
  whoever held it and re-granted to the trusted login authority alone.** The
  path fails *safe*: with no trusted authority alive, the authority is revoked
  and granted to nobody, so elevation becomes unredeemable rather than
  reachable by the wrong Proc.
- **Every door onto the console gates identically.** The console is one
  single-reader resource with two front doors — a syscall and a namespace path
  — and both enforce the same attachment check. Adding a walkable path adds no
  ungated door.

## The three roles

The invariant's working content is that **console authority is three separate
roles, and holding one never confers another.** Each is a distinct kernel
concept with its own gate:

| Role | Conveys | Held by |
|---|---|---|
| **attached** | the right to redeem elevation, and to open the console by name | the boot authority before it relinquishes; the trusted login authority after an attention key |
| **owner** | being the target of the interrupt and window-change notes | the session shell |
| **renderer** | the output drain and the input feed — nothing else | the bound compositor |

The separations are not incidental — each was established by a specific
failure, and collapsing any pair reintroduces it:

- **attached ≠ owner.** The attention key attaches the trusted authority but
  deliberately does *not* make it the owner. When it did, a later interrupt
  posted to a login authority that manages no notes armed the terminate latch
  and killed the trusted path until reboot.
- **owner ≠ terminate-anything.** The attention key posts no note at all. It
  once reused the interrupt note as a courtesy "you lost the console"; once
  that note became a real terminate-if-uncaught signal, that courtesy
  terminated init during bringup. A dedicated revocation note is a recorded
  seam; until then, losing the attach bit *is* the observable effect.
- **renderer ≠ attached.** The renderer holds the console's entire output
  stream and injects its input, but may not read console *input* through the
  data leaf and may not fire the attention key. It is the display, not the
  authority.

## Enforcement

`kernel/cons.c` owns the recognizer and the input path; `kernel/devdev.c`
owns the namespace gates; `kernel/proc.c` owns the role pointers and the
transition.

**The recognizer is stateless** — one flag set on a line-condition entry, no
multi-byte state machine to starve or partially spoof — and its work is
*deferred*, because the transition takes the process-table lock and cannot run
in interrupt context. The console manager kthread performs it.

**The transition is a single critical section** under the process-table lock,
so the owner and trusted pointers cannot be reaped mid-transition; it never
dereferences a role pointer it has not just liveness-checked. It is idempotent
under a flood: once the trusted authority is attached and no owner remains,
a repeat is a no-op.

**The injected-input path hardwires the line condition false.** That is the
renderer clause made structural rather than checked: there is no value a feed
byte can take that reaches the recognizer.

**A batched attention key supersedes a coalesced interrupt.** Two pending
deferred actions lose their arrival order, and delivering the interrupt to the
*pre*-transition owner is exactly the outcome removed above, re-synthesized
through coalescing. So the transition wins the batch.

## Medium independence

The invariant is stated over an *attention key* and a *trusted sink*, not over
a serial port. On serial-bearing media the trusted path is live today. On a
display-only board it is reserved: the attention key becomes a kernel-scanned
key combination from a trusted-tier keyboard, and the trusted sink becomes the
kernel painting the framebuffer directly with the renderer suspended.

The reason the graphical path *needs* that, rather than reusing the renderer,
is the renderer's position: on a graphical backend it decodes the keyboard, so
it is already in the input path and has every keystroke by construction. That
is precisely why it is untrusted for elevation, and why the trusted path stays
on serial until the kernel owns the keyboard.

## Validation

Prose plus the console test family, which drives the transition directly:
revoke-and-regrant, the fail-safe with no trusted authority alive, idempotence
under a flood, the path through the real manager kthread, that the transition
terminates nobody, and that it attaches correctly from the relinquished state
the boot chain leaves behind. The gates are the interactive harness, which
drives a real login over a real console, and the multi-boot SMP gate.

**blind-to:** the attention key itself is never exercised end to end. The
harness cannot inject a serial line condition — one UART, no side channel, and
the boot-banner contract forbids restructuring the serial setup — so the
recognizer is proven by synthetic drive plus one interactive escape sequence,
never by the automated suite. The unforgeability argument is therefore
*structural* (a line condition is not data) rather than tested, which is the
strongest available form but is not a test.

There is no model. What a model would add is the interleaving of a transition
against a concurrent owner exit — and that is the process-table lock's
territory, shared with [[inv-i24]]. The console's one *modelled* obligation is
the deferred wake relay, which is [[spec-cons-poll]] and belongs to [[inv-i9]].
