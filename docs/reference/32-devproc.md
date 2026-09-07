# 32 — devproc (/proc) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-devproc-absorb`). Its
content now lives, code-verified and current, in the dossier:

    vault/system/kernel/introspection/sub-kernel-devproc.md

(the qid target encoding and the pid-0 disambiguation, the four-place
registration with the one silently-failing whitelist, the debug-attach slot's
bare-pointer soundness and its atomic `CDEBUGOWNER` release, the two-stop-owners
park and the fully-stopped conjunction, the park-predicate stale-registration
race and its narrows/converges/composes fix, the SPSR write guard, the kstack
raw/symbolic capability split, and the rendered-field contracts — `cpu_ns`
cumulative/monotonic, the unforgeable `name`, `exe`/`cwd` bare-bytes. I-26, I-39,
I-16, I-22, I-1.)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- It is a **P4-C-plus-telemetry** account: it carries `status`/`ns`/`cwd`/`exe`
  and the read partition well, but underweights the **Go-IDE debug control
  surface** that is now the bulk of the Dev — the stop/step/hardware-breakpoint
  verbs, the fully-stopped conjunction and the park-predicate stale-registration
  race (a one-in-eighty transient that a single `-1` return value turned fatal,
  fixed by a third conjunct that only narrows), the SPSR-never-written register
  guard, and the settled-thread `kstack` raw/symbolic capability split. The
  dossier carries these with their prosecution chains.
- Its secondary-file mentions (`proc.c`, `spoor.c`, `territory.c`, `env.c`) are
  cross-references; each is owned by its own dossier (`sub-kernel-proc`,
  `sub-kernel-spoor`, `sub-kernel-territory`, `sub-kernel-content`), so nothing
  is orphaned by this single-redirect stub.
- The qid `(pid << 32) | subkind` field split and the `PQS_*` subkind values live
  in `kernel/devproc.c` — the source of truth — which the dossier points at
  rather than duplicating.
