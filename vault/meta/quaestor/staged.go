package main

// Pre-commit checks: Record-plane immutability against the git INDEX
// (schema R3 + section 5.3 closure fields), and the audit:hard
// dossier-diff advisory. Reads `git show :path` (the staged content),
// never the working tree.

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

func gitOut(root string, args ...string) string {
	cmd := exec.Command("git", args...)
	cmd.Dir = root
	out, _ := cmd.Output() // errors -> empty output, like the reference impl
	return string(out)
}

func stagedChecks(root string, reg *Registry) (fails, warns []string) {
	status := gitOut(root, "diff", "--cached", "--name-status")
	type entry struct{ st, path string }
	var entries []entry
	for _, ln := range strings.Split(status, "\n") {
		parts := strings.Split(ln, "\t")
		if len(parts) >= 2 {
			st := parts[0]
			if len(st) > 1 {
				st = st[:1]
			}
			entries = append(entries, entry{st, parts[len(parts)-1]})
		}
	}
	stagedPaths := map[string]bool{}
	for _, e := range entries {
		stagedPaths[e.path] = true
	}
	addedChgRaw := ""
	for _, e := range entries {
		if e.st == "A" && strings.HasPrefix(e.path, "vault/record/changes/") {
			addedChgRaw += gitOut(root, "show", ":"+e.path)
		}
	}
	for _, e := range entries {
		if e.st != "M" || !strings.HasPrefix(e.path, "vault/record/") {
			continue
		}
		oldRaw := gitOut(root, "show", "HEAD:"+e.path)
		newRaw := gitOut(root, "show", ":"+e.path)
		of, ob, okOld := parseFront(oldRaw)
		nf, nb, okNew := parseFront(newRaw)
		if !okOld || !okNew {
			continue
		}
		t := nf.Str("type")
		if t == "arc" && of.Str("status") == "active" {
			continue // arcs are mutable until frozen
		}
		if strings.TrimSpace(ob) != strings.TrimSpace(nb) {
			fails = append(fails, e.path+
				": Record-plane body changed (R3: append-only; "+
				"correct via a superseding note)")
			continue
		}
		changed := map[string]bool{}
		for _, k := range of.Keys() {
			ov, _ := of.Get(k)
			nv, okN := nf.Get(k)
			if !okN || !valueEqual(ov, nv) {
				changed[k] = true
			}
		}
		for _, k := range nf.Keys() {
			if !of.Has(k) {
				changed[k] = true
			}
		}
		allowed := CLOSURE[t]
		var illegal []string
		for k := range changed {
			if !allowed[k] {
				illegal = append(illegal, k)
			}
		}
		if len(illegal) > 0 {
			sort.Strings(illegal)
			fails = append(fails, fmt.Sprintf(
				"%s: non-closure Record fields changed: %s",
				e.path, pyList(illegal)))
			continue
		}
		if len(changed) > 0 {
			noteID := strings.TrimSuffix(filepath.Base(e.path), ".md")
			selfFixup := t == "chg" && len(changed) == 1 && changed["commits"]
			if !selfFixup && !strings.Contains(addedChgRaw, noteID) {
				fails = append(fails, fmt.Sprintf(
					"%s: closure fields %s changed with no staged "+
						"chg-* note linking '%s'",
					e.path, pyList(sortedKeys(changed)), noteID))
			}
		}
	}
	// audit:hard dossier-diff warning for staged chg notes.
	for _, e := range entries {
		if e.st != "A" || !strings.HasPrefix(e.path, "vault/record/changes/") {
			continue
		}
		f, _, ok := parseFront(gitOut(root, "show", ":"+e.path))
		if !ok || f.Len() == 0 {
			continue
		}
		if f.Has("no-dossier-change") {
			continue
		}
		for _, tid := range edgeVals(f, "touched") {
			tn, okT := reg.Get(tid)
			if okT && tn.Front.Str("type") == "sub" &&
				tn.Front.Str("audit") == "hard" && !stagedPaths[tn.Rel] {
				warns = append(warns, fmt.Sprintf(
					"%s: touches audit:hard [[%s]] but %s is not in this "+
						"commit (add a 'no-dossier-change: <why>' field "+
						"if deliberate)", e.path, tid, tn.Rel))
			}
		}
	}
	return fails, warns
}
