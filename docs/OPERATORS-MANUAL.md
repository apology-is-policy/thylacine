# Thylacine Operator's Manual

This index describes the *Thylacine Operator's Manual*: whom it is written for, how its source is organised, how it reaches a running system, and which sections exist. The manual itself is the set of section files under `docs/manual/`.

---

## Readers

The manual is written for an operator who is comfortable with commands, files, processes, and configuration, but who does not yet know Thylacine's architecture or terminology. A section serves two readers at once: one who needs a working invocation, and one who wants to understand the mechanism behind it.

The technical reference in the vault (`vault/system/`) is written for the developers of Thylacine itself. The manual describes the same system from the operator's side and is never authoritative on internal semantics; where the two disagree, the vault and the code decide.

---

## The writing guide

Every section is written to `docs/thylacine-operators-manual-writing-guide.md`, which is binding for all manual prose. The guide fixes the shape of a section (an untitled opening, then **In Practice**, then **Technical Details**), the reference voice, the language to avoid, and the requirement that every command, option, default, and error be verified against the current tree. Read it before drafting or revising a section.

---

## Source, installation, and rendering

Each section is one Markdown file, `docs/manual/NN-<topic>.md`. The number orders the sections for sequential reading and does not appear in any heading. Operating-system topics are numbered below 40; ported applications start at 40.

The source is installed in the operating system under `/manual` and read through a reader that emits Beacon markup (`docs/BEACON.md`). Under Halcyon a section is rendered with its headings, tables, and preformatted blocks; on a serial console or through a pipe the same section reads as plain text.

Neither the reader nor the `/manual` installation exists yet. Both are built before further sections are written (operator decision, 2026-09-16), and the reader defines the supported source format: a section may use only the Markdown forms that the reader maps onto Beacon. New section content waits for those rules.

---

## Sections

| File | Section | State |
|---|---|---|
| `manual/00-overview.md` | Overview | Phase-0 plan (2026-05-04). It describes intended phases rather than the current system and is to be replaced. |
| `manual/40-dosbox.md` | DOSBox-X | Written 2026-09-05 to the earlier page template; to be rewritten to the writing guide. |
| `manual/41-audio.md` | Audio (Nocturne) | Written 2026-09-05 to 2026-09-07 to the earlier page template; to be rewritten to the writing guide. |

Containers is the first section to be written to the guide, once the reader exists.

---

## Maintenance

A section is added when the facility it describes has settled, so sections appear one at a time as deliberate work. Once a section exists, a change to the user-visible behaviour of its facility updates that section in the same commit, and the update follows the writing guide.

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded as `docs/USER-MANUAL.md` (Phase 0). | Index and page template for a user manual. |
| 2026-09-05 | Revived; added the `/manual` plan and the first chapter (`40-dosbox.md`). | Operator decision; supersedes the 2026-05-31 deferral to v1.0-rc. |
| 2026-09-16 | Renamed to `docs/OPERATORS-MANUAL.md`; the writing guide adopted and the earlier page template withdrawn; the reader and `/manual` installation to be built before further sections. | Operator decisions. |
