---
id: dec-2026-09-29-inline-media-one-principal
type: dec
title: "Inline media: a pane's place token routes, it does not isolate -- one principal's panes are one authority domain"
date: 2026-09-29
status: standing
decided-by: research-collapsed
affects: [sub-halcyond, sub-view]
created: 2026-09-29
---
## Fork

The Fable-diversity round on inline media (FABLE-1, F2) found that the session
channel's per-pane token, which `halcyond` writes into the pane's `/env` before
the spawn, is readable by any Proc of the session's principal through
`/proc/<pid>/environ`: pane programs are unsealed, and devproc's owner axis
admits the same principal. So a program in pane B can place an image into
pane A. The reserved I-47 row said "no cross-pane placement", and the
AUDIT-TRIGGERS row called the token "unnameable across panes". I-47 could not
be flipped to ENFORCED while it claimed that. Two ways out: say what the code
enforces, or make the token unreadable.

## Research

- **Plan 9.** rio serves every window's files (`cons`, `consctl`, `window`,
  `text`) under `/dev/wsys/<n>`, open to every process of the session's
  namespace: `echo hi > /dev/wsys/3/cons` writes into another window. The user
  is the protection domain; the per-window files are routing, not a boundary.
- **The tree, 2026-09-29.** `kernel/devproc.c:1353`: the owner axis is the
  same principal, and `/proc/<pid>/environ` is 0400 owner-or-`CAP_HOSTOWNER`.
  `usr/ptyfs/src/server.rs:171-181`: every pts slave and ctl file is 0666 and
  SYSTEM-owned, so any Proc that can name a live pts can read or inject into
  it. That posture is recorded there as v1.0's (per-pts owner and 0600 is
  AUX-ROADMAP #13), and it is wider than a principal. I-39: a debugger of the
  same principal whose caps cover the target's can stop it and drive its
  registers.
- **SOTA.** A terminal's image protocols (sixel, kitty, iTerm) carry pixels in
  the pane's own byte stream, so whoever can write the pty can place an image.
  Wayland isolates clients from each other, but not from the same user's
  processes, which can ptrace them or read their environment.

## Options

1. **Say what the code enforces.** The peer gate is the authority axis: only a
   peer of the session's principal is admitted, fail-closed. The token is
   routing: a request is never misrouted and never lands in a pane that has
   gone. One principal's panes are one authority domain. Rewrite ARCH I-47 (a)
   and the AUDIT-TRIGGERS row to say so. No code change.
2. **Pass the place channel as an inherited fd** and take the token out of
   `/env`. A program in pane B could no longer read pane A's token. But it can
   still write pane A's pts, which is more than placing an image, and a
   debugger of the same principal can drive pane A's shell to use the fd. The
   isolation it seems to buy does not exist while those hold.

## The call

Option 1. The research collapsed the fork: option 2 protects nothing that the
pts posture and the debug axis leave open, and the heritage answer is the one
the code already implements. The implementer (Opus 5.5, aux) made the call
under the operator's "your guts" grant and names it in the run's summary for
the operator's veto; the chunk's audit round (Fable) was asked to rule on it.

## Rationale

A token held in the environment is a name, not a capability, and it was never
the weakest link: any Proc that can name a pts can inject text into that pane.
Stating the domain truthfully keeps the invariant row honest, which is what an
ENFORCED row is for. Revisit if either premise changes. If ptyfs gains per-pts
ownership and pane programs gain a boundary inside the principal (a sealed
pane, or an environ read that also asks the debug axis's cover rule), then the
token in `/env` becomes the weakest link, and option 2's inherited fd becomes
the right carrier.
