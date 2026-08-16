package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A seam must be closable through the tool. Schema 5.3 designates
// `seam.status` and `seam.closed-by` as closure fields, and CLOSURE listed
// four types without seam -- so closing a seam, the single most ordinary
// lifecycle event the type has, was impossible except by the hand-edit the
// tool exists to replace. Found by trying to use it.
//
// The second half is the plane guard. `isRecord` demanded `vault/record/`,
// but a seam lives at `vault/seams/` BY DESIGN -- schema 5.3: "Seams are
// Present-plane (debt is a fact about the system now)". Both rules are right
// and they are about different things: the Record plane is append-only, and
// its closure fields are the exception to THAT. A seam is not subject to the
// append-only rule at all, so requiring the Record plane for its closure was
// reading one rule as though it were the other.
func TestSeamIsClosableThroughTheTool(t *testing.T) {
	root := t.TempDir()
	rel := "vault/seams/seam-t-x.md"
	abs := filepath.Join(root, rel)
	mkdirAll(t, filepath.Dir(abs))
	writeFile(t, abs, "---\nid: seam-t-x\ntype: seam\nstatus: open\n"+
		"surface: [sub-t-x]\nopened-by: chg-t-1\n---\n## Owed\nx\n")
	// The chg the closure points at must exist, or the edge check refuses.
	mkdirAll(t, filepath.Join(root, "vault/record/changes"))
	writeFile(t, filepath.Join(root, "vault/record/changes/chg-t-2.md"),
		"---\nid: chg-t-2\ntype: chg\ndate: 2026-01-01\narc: arc-t\n"+
			"touched: []\ncommits: []\ndepth: skeletal\n---\nbody\n")

	if _, err := closeNote(root, "seam-t-x", map[string]string{
		"status": "closed", "closed-by": "chg-t-2"}); err != nil {
		t.Fatalf("a seam must be closable: %v", err)
	}
	b, _ := os.ReadFile(abs)
	if !strings.Contains(string(b), "status: closed") ||
		!strings.Contains(string(b), "closed-by: chg-t-2") {
		t.Fatalf("both closure fields must land:\n%s", b)
	}

	// The control: the closure vocabulary must still be ENFORCED for seams,
	// not merely opened up. Without this the fix is satisfied by allowing any
	// field on any type, which would let a "closure" rewrite the body's claims.
	if _, err := closeNote(root, "seam-t-x", map[string]string{
		"surface": "sub-other"}); err == nil {
		t.Fatal("a non-closure field must still be refused on a seam")
	}
}
