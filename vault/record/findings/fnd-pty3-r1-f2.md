---
id: fnd-pty3-r1-f2
type: fnd
round: adt-pty3-r1
severity: P3
status: documented
title: "The one-read-fetches-the-whole-ctl-render property was load-bearing and unstated"
surface: [sub-pouch-tty]
threatens: []
fixed-by: chg-2026-07-18-pty3
created: 2026-08-01
---
## Prosecution

`pts_ctl_read` parses the ctl render from a SINGLE `read`. A partial read
would not fail — it would silently mis-parse flags and winsize into
wrong-but-successful `TCGETS` / `TIOCGWINSZ` answers, which is the
dangerous shape (a wrong termios that reports success).

The property held on four unstated facts: the render is ~54 bytes, ptyfs
serves it whole at offset 0 in one Rread, the negotiated msize is far
larger, and a ctl read never defers.

## Disposition

Named explicitly in a comment together with the condition under which it
must become a read-to-EOF loop. The value of the finding is not a code
change but the conversion of an inherited assumption into a stated
dependency.
