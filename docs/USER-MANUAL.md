# Thylacine OS — User Manual

This is the **user-facing** reference for Thylacine OS. Audience: people who use Thylacine — operators, developers writing programs against Thylacine syscalls, system administrators, container users, end users of Halcyon.

This is distinct from `docs/REFERENCE.md` (the technical reference, audience: developers of Thylacine itself). Both are first-class; both are maintained continuously; both are binding for every PR.

---

## How to read this

The manual is split by topic. Each topic gets its own page in `docs/manual/NN-<topic>.md`. Pages follow a consistent template:

- **Overview** — what this surface is for; when to use it.
- **Getting started** — minimal example to do the most common thing.
- **Reference** — every operation with arguments, return values, errors, examples.
- **Patterns** — common compositions; idiomatic usage.
- **Differences from Linux** — gotchas for users coming from Linux. What Thylacine does the same; what it does differently; what it deliberately doesn't do.
- **Troubleshooting** — common errors and how to resolve them.
- **See also** — pointers to related topics + relevant ARCHITECTURE / TOOLING sections.

The bar: a user landing on a topic page should be able to learn how to do the thing without leaving the page. A developer porting a Linux program should be able to find every relevant compat note in one place.

---

## Snapshot

- **Thylacine version**: pre-v0.1 (Phase 0 complete; Phase 1 not yet started).
- **Phases shipped**: 0 (the Phase 0 design phase; no user-visible binary yet).
- **First user-visible release**: **v0.5 — Utopia** (Phase 5 exit). The textual POSIX environment.
- **v1.0-rc.1**: at Phase 7 exit. Hardened textual + compat OS. Shippable as v1.0 if Halcyon slips.
- **v1.0**: at Phase 8 exit. Halcyon + final release.

---

## Contents

| Page | Topic | Audience | Available from |
|---|---|---|---|
| [00-overview.md](manual/00-overview.md) | Bird's-eye view of using Thylacine | All users | Phase 0 (this scaffold) |
| 01-getting-started.md (planned) | Install, boot, first login | All users | Phase 5 (Utopia) |
| 02-shells.md (planned) | rc, bash, pipelines, redirection, job control | Developers, admins | Phase 5 |
| 03-territories.md (planned) | bind / mount / unmount; per-process territories; containers as territories | Developers, admins | Phase 5-6 |
| 04-coreutils.md (planned) | uutils-coreutils + Plan 9 userland; differences from GNU | Developers | Phase 5 |
| 05-syscalls.md (planned) | Native + Linux-compat syscall surface; man-page-quality reference | Developers | Phase 5 |
| 06-posix-programming.md (planned) | poll, futex, pthread, signals; what works, what's deferred | Developers | Phase 5 |
| 07-stratum-admin.md (planned) | Pools, datasets, snapshots, send/recv, encryption, /ctl/, janus | Admins | Phase 4-5 |
| 08-linux-binary-compat.md (planned) | What runs (musl-static, OCI containers); what's best-effort | Developers, users | Phase 6 |
| 09-containers.md (planned) | thylacine-run; OCI images; territory construction | Users, admins | Phase 6 |
| 10-network.md (planned) | /net/ administration; sockets API; smoltcp behavior | Admins, developers | Phase 6 |
| 11-troubleshooting.md (planned) | Boot failures, recovery shell, common panics, /ctl/log/ | All users | Phase 5+ |
| 12-halcyon.md (planned) | Halcyon usage; scroll buffer; image display; video; customization | End users | Phase 8 |
| 13-keyboard-shortcuts.md (planned) | Halcyon keybindings, terminal-mode keybindings | End users | Phase 8 |

(Pages appear as their underlying surfaces ship. Phase 0 scaffold provides the index + overview only.)

---

## Quick-start summary (will be filled in as Phase 5 ships)

(Stub — Phase 5 deliverable. The "5 minutes to Utopia" walkthrough.)

---

## Document maintenance

When a chunk lands that affects user-visible behavior, the author updates the relevant `docs/manual/NN-*.md` section. Internal refactors that don't change user-visible behavior don't require manual updates; user-visible changes always do.

Per CLAUDE.md "Reference documentation discipline": the technical reference (`docs/REFERENCE.md`) and the user manual (this document + `docs/manual/`) are both binding for every PR. Missing user-manual updates on user-visible changes are reverted along with their code.

Update cadence:
- **New syscall** → manual entry in `05-syscalls.md` with man-page-quality detail.
- **New admin surface** → manual entry in the relevant page (e.g., `07-stratum-admin.md` for `/ctl/.../scrub`).
- **New error case** → entry in `11-troubleshooting.md`.
- **Behavior difference vs Linux** → entry in `08-linux-binary-compat.md` and the relevant page's "Differences from Linux" section.
- **Halcyon UX change** → entry in `12-halcyon.md` or `13-keyboard-shortcuts.md`.
- **Snapshot block** at top of this file: refresh tip hash, version, phase status on every chunk that moves them.

---

## Conventions

- **Examples are runnable.** Code blocks in this manual are exact commands you can paste into a Thylacine shell. If a command doesn't work as written, it's a documentation bug.
- **Man-page-quality syscall reference.** `05-syscalls.md` follows the structure of Linux man pages (NAME, SYNOPSIS, DESCRIPTION, RETURN VALUE, ERRORS, NOTES, SEE ALSO, EXAMPLES) with Thylacine-specific behavior called out explicitly.
- **No marketing language.** This manual describes what is, not what we hope to ship. Future features go in ROADMAP, not here.
- **Linux differences explicit.** Wherever Thylacine differs from Linux user expectations, the difference is named, and the rationale is referenced (typically a section in ARCHITECTURE.md).

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded (Phase 0 complete). | Index and template for the user manual. Per-topic pages appear as their surfaces ship from Phase 4 onward (Stratum admin), Phase 5 (Utopia), Phase 6 (Linux compat + containers + network), Phase 8 (Halcyon). |
