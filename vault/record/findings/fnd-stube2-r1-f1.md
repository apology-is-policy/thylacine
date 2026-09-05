---
id: fnd-stube2-r1-f1
type: fnd
title: "Kernel-stack info leak: the max-length walk name was never NUL-terminated"
round: adt-stube2-r1
severity: P0
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-05-21-p5-chroot
regression: "sys_walk_open.max_length_name_nul_terminated (a 64-byte name + a responder asserting the wire wname[0]_len is EXACTLY 64)"
created: 2026-08-01
---
## Prosecution

`sys_walk_open_handler` staged the component name into
`char name_scratch[SYS_WALK_OPEN_NAME_MAX]` — sized exactly to the
maximum — and wrote the terminator CONDITIONALLY:
`if (name_len_raw < SYS_WALK_OPEN_NAME_MAX) name_scratch[name_len_raw] = '\0';`

At exactly the maximum length the condition is false and no NUL is
written. `dev9p_walk` then discovers the length by scanning
(`while (s[l] != '\0') l++;`), so it runs off the end of the scratch
into adjacent kernel stack until it happens on a zero byte — saved
registers, return addresses, the KASLR slide. The discovered length and
the bytes are packed into the `Twalk` `wname[0]` and shipped over a
transport the caller controls.

Reachable from any EL0 Proc that can name a 64-byte component. No
corruption needed, no race — just the boundary value.

## Disposition

Fixed: the scratch widened to `SYS_WALK_OPEN_NAME_MAX + 1` and the NUL
written UNCONDITIONALLY. The regression asserts on the WIRE — a
responder that checks the outgoing `wname[0]` length is exactly 64 —
because a regressed handler's over-scan shows up as a larger length, and
asserting on the returned value alone would not have seen it.

Root cause was [[fnd-stube2-r1-f6]], the comment that had described the
terminator as optional.
