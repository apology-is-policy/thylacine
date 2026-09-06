package main

// The code->dossier reminder: the mirror image of the audit:hard advisory in
// stagedChecks. There, a staged CHG that touches an audit:hard dossier warns
// when the dossier is not co-staged; here, staged CODE owned by a dossier
// reminds you the dossier may be owed an update. Tiered per the operator's
// ratified call (2026-09-06): an audit:hard owner BLOCKS, any other owner WARNS.
//
// It runs from the commit-msg hook rather than pre-commit, and that placement
// is load-bearing, not incidental. The one escape that serves every track is a
// `No-dossier-change: <why>` git trailer -- the code tracks (main, aux) write no
// vault chg notes and, per CLAUDE.md's cutover rule, ring the vault for owned
// prose rather than co-staging it in a kernel commit, so a chg-field-only escape
// would leave them with a block and no clean way through. Only the commit
// message carries the trailer, and only the commit-msg hook sees the message;
// pre-commit runs before any message exists. The same field on a staged chg is
// honoured too, so the vault track reaches the escape identically.

import (
	"fmt"
	"os"
	"strings"
)

type stagedEntry struct{ st, path string }

// stagedEntries: the one reading of the git index shared by both staged gates.
// A second parse here would be a second definition of "what this commit
// stages", which is the drift the vault exists to catch, in the tool that
// enforces it. The status letter is truncated to its first char so a rename
// (`R100`) or copy (`C075`) reads as `R`/`C`; the path taken is the last
// tab-field, which is the destination for a rename.
func stagedEntries(root string) []stagedEntry {
	status := gitOut(root, "diff", "--cached", "--name-status")
	var entries []stagedEntry
	for _, ln := range strings.Split(status, "\n") {
		parts := strings.Split(ln, "\t")
		if len(parts) >= 2 {
			st := parts[0]
			if len(st) > 1 {
				st = st[:1]
			}
			entries = append(entries, stagedEntry{st, parts[len(parts)-1]})
		}
	}
	return entries
}

// dossierGate resolves each staged code surface to its owning sub-dossier(s)
// and reports the ones not co-staged, blocking on audit:hard and warning
// otherwise. `msg` is the commit-message text (the hook reads the file).
func dossierGate(root string, reg *Registry, msg string) (fails, warns []string) {
	entries := stagedEntries(root)
	// A blanket deferral, keyed exactly like the chg field so there is one
	// concept to learn: `No-dossier-change: <why>`. Honoured from the commit
	// trailer AND from any staged chg's frontmatter. A non-empty reason is
	// required for the trailer -- a bare key is a silent bypass, which is the
	// opposite of the reliable reminder this gate exists to be.
	if hasDeferTrailer(msg) || stagedChgDefers(root, entries) {
		return nil, nil
	}
	stagedPaths := map[string]bool{}
	for _, e := range entries {
		stagedPaths[e.path] = true
	}
	idx := ownerIndex(reg)

	// Owed owners, deduped by note id: one dossier owning several staged files
	// is reported once. The `.c`/`.h` twin is resolved (convention 1), because
	// editing either half touches the one surface a sub-dossier describes -- the
	// header carries the ABI the .c implements, and a dossier keyed on the .c
	// is exactly as owed by an edit to its .h.
	seen := map[string]bool{}
	type owed struct {
		note *Note
		path string
	}
	var owedList []owed
	for _, e := range entries {
		if e.st == "D" || !srcRe.MatchString(e.path) {
			continue
		}
		owners := append([]*Note{}, idx[e.path]...)
		if tw := twinOf(e.path); tw != "" {
			owners = append(owners, idx[tw]...)
		}
		for _, n := range owners {
			if stagedPaths[n.Rel] || seen[n.ID] {
				continue
			}
			seen[n.ID] = true
			owedList = append(owedList, owed{n, e.path})
		}
	}

	for _, o := range owedList {
		base := fmt.Sprintf("%s changed but its dossier [[%s]] (%s) is not "+
			"in this commit", o.path, o.note.ID, o.note.Rel)
		if o.note.Front.Str("audit") == "hard" {
			fails = append(fails, base+" -- audit:hard: co-stage the dossier, "+
				"or ring the vault and add a 'No-dossier-change: <why>' commit trailer")
		} else {
			warns = append(warns, base+
				" (add 'No-dossier-change: <why>' to silence)")
		}
	}
	return fails, warns
}

// hasDeferTrailer scans the commit message for a `No-dossier-change:` line with
// a non-empty value. Comment lines (git strips leading `#`) never match, since
// a trimmed comment still begins with `#`, so the instructional text a template
// might carry does not become an escape.
func hasDeferTrailer(msg string) bool {
	const key = "no-dossier-change:"
	for _, ln := range strings.Split(msg, "\n") {
		ln = strings.TrimSpace(ln)
		if strings.HasPrefix(strings.ToLower(ln), key) &&
			strings.TrimSpace(ln[len(key):]) != "" {
			return true
		}
	}
	return false
}

// stagedChgDefers: a staged chg carrying the `no-dossier-change` field defers
// the whole commit, matching the field's meaning in stagedChecks (there it
// suppresses the chg->dossier advisory; here the code->dossier block).
func stagedChgDefers(root string, entries []stagedEntry) bool {
	for _, e := range entries {
		if e.st == "D" || !strings.HasPrefix(e.path, "vault/record/changes/") {
			continue
		}
		if f, _, ok := parseFront(gitOut(root, "show", ":"+e.path)); ok &&
			f.Has("no-dossier-change") {
			return true
		}
	}
	return false
}

func cmdDossierGate(root string, args []string) int {
	msgPath := ""
	for i := 0; i < len(args); i++ {
		if args[i] == "--msg" && i+1 < len(args) {
			msgPath = args[i+1]
			i++
		}
	}
	msg := ""
	if msgPath != "" {
		if b, err := os.ReadFile(msgPath); err == nil {
			msg = string(b)
		}
	}
	reg, _ := loadRegistry(root)
	if reg.Len() == 0 {
		// Fail OPEN, unlike pre-commit's fail-closed. pre-commit is the
		// authoritative infra gate (it refuses an empty registry); by the time
		// this secondary reminder runs, pre-commit has already passed, so an
		// empty registry here means the gate was bypassed (--no-verify) or the
		// vault is gone -- and a commit-msg hook that blocked on that would be a
		// worse failure than a missed reminder.
		return 0
	}
	fails, warns := dossierGate(root, reg, msg)
	for _, w := range warns {
		fmt.Println("WARN " + w)
	}
	for _, f := range fails {
		fmt.Println("FAIL " + f)
	}
	if len(fails) > 0 {
		fmt.Printf("dossier-gate: %d block(s), %d warn(s) -- update the dossier, "+
			"ring the vault, or add a 'No-dossier-change: <why>' trailer "+
			"(vault/meta/schema.md section 8)\n", len(fails), len(warns))
		return 1
	}
	return 0
}
