---
id: chg-2026-11-09-trusted-audit-close
type: chg
title: "The trusted-path batch audit close: renewal behind a live engine, serial verdicts wait, the episode tail (votes 70-73)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-corvus
  - sub-kernel-cons
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
One round (Opus 5.5 reviewing Opus 5.5, same family; 0 P0, 2 P1, 1 P2, 9 P3) over votes 70, 71, 72 (both halves) and 73 on corona/trusted, merged with the implementer's own pass, which reached F4 first.

F1: the vote-71 renewal waited for a retirement, so one client chain longer than five seconds, or a host loop held by a readback, still failed every chord with no trusted screen: the case [[dec-2026-11-05-sak-waits-on-backlog]] exists to close, whose stated outcome is the trusted screen or a dead engine. Lictor now renews while the engine is alive and the control queue holds the takeover up (`Gpu::control_waiting`); a chain that never retires kills the engine at `FENCE_ABANDON_MS`, inside the kernel's 35 s. A cursor queue stalled alone renews nothing: no client reaches it. Witness: `ls-graphical-sak-dead` leg 1, rebuilt (a stall withheld 8 s with no retirement must end in the trusted scanout).

F2: vote 70 never reached the serial verdict, which skipped `console_wait_dismiss` for generation 0, so the rest of what the operator typed went to the shell. Every verdict now waits, on both media and whether or not its frame was confirmed, and the serial one says `press Enter`. Ten serial scenarios press Enter after a verdict; `im3-lex-curiata` arm 1 types a letter at one and needs no END for 3 s.

F3: warp-prove's C6-RB DEADLINE now judges vote 72a's bound (no bystander present past 900 ms, and one refused, once the readback stood past it). F4: the seat's return keeps phase-0 input from the compositor until a pass of the input thread begun after it has been taken ([[chg-2026-11-09-input-thread]]); leg H's lever stops the thread through the held restore. F5: the side queue's mover stops after a take that echoes, so the pre-SAK echo past BEGIN is `CONS_ECHO_MAX` again. F6: a Ctrl-C read with ISIG clear stays data (LITERAL); the rest of F6, bytes read under the termios of their move, is Linux n_tty's flip buffer under throttle, kept and documented. F7, the echo of a moved byte overtaken by the next byte's, is cosmetic and documented. F8: the backlog scenario lists failures and EXTINCTION first and skips on a 2D host by its own guard. F9, F10: the tests now see a second interrupt and a renewal that jumps to the limit. F11: stale prose. F12: test mode only, a lever key dropped at a full queue no longer stops the input thread for good.
