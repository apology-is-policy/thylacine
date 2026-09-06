package main

import (
	"path/filepath"
	"strings"
	"testing"
)

// A gitignored TLC counterexample dump (specs/*_TTrace_*.tla) is not a module.
// If the spec-coverage scan counts it, every leftover dump flips the view stale
// and blocks commits on every track until the junk is hand-removed (main's yip
// 0057). The assertion is DISCRIMINATING: a real module must still be counted
// (a blanket skip would drop it too) while the TTrace dump must not be.
func TestSpecCoverageExcludesTTraceDumps(t *testing.T) {
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "specs"))
	writeFile(t, filepath.Join(root, "specs", "alpha.tla"),
		"---- MODULE alpha ----\n====\n")
	writeFile(t, filepath.Join(root, "specs", "alpha_TTrace_7.tla"),
		"---- MODULE alpha_TTrace_7 ----\n====\n")

	// vaultRoot(reg) returns a note's Path with its Rel suffix trimmed, so a
	// single anchor note under root makes the scan read root/specs.
	anchor := &Note{ID: "sub-anchor", Rel: "vault/system/sub-anchor.md",
		Path: filepath.Join(root, "vault/system/sub-anchor.md")}
	reg := &Registry{byID: map[string]*Note{anchor.ID: anchor},
		ordered: []*Note{anchor}}

	out := renderSpecCoverage(reg)

	if !strings.Contains(out, "1 modules") {
		t.Fatalf("expected exactly 1 module (alpha; the TTrace dump excluded):\n%s", out)
	}
	if !strings.Contains(out, "alpha.tla") {
		t.Fatalf("the real module must still be counted (not a blanket skip):\n%s", out)
	}
	if strings.Contains(out, "TTrace") {
		t.Fatalf("the TTrace dump must be excluded from the view:\n%s", out)
	}
}
