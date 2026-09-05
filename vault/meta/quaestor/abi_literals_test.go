package main

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A git repo is required: the scan lists TRACKED files, so that an untracked
// scratch file in someone's worktree cannot fail the gate.
func litRepo(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "tools"))
	writeFile(t, filepath.Join(root, "tools", "gate.sh"), "grep -q 'BOOT OK' log\n")
	writeFile(t, filepath.Join(root, "tools", "note.sh"), "# mentions BOOT OK in a comment\n")
	writeFile(t, filepath.Join(root, "tools", "quiet.sh"), "echo nothing to see\n")
	for _, args := range [][]string{
		{"init", "-q"}, {"add", "-A"},
		{"-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "x"},
	} {
		c := exec.Command("git", append([]string{"-C", root}, args...)...)
		if out, err := c.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v %s", args, err, out)
		}
	}
	return root
}

func litReg(mirrors, mentions []string) *Registry {
	f := newFront()
	f.Set("type", scalar("abi")) // scalar, as the real corpus stores it
	f.Set("literals", list([]string{"BOOT OK"}))
	f.Set("literal-scan", list([]string{"tools"}))
	f.Set("mirrors", list(mirrors))
	f.Set("literal-mentions", list(mentions))
	n := &Note{ID: "abi-t", Front: f}
	return &Registry{byID: map[string]*Note{n.ID: n}, ordered: []*Note{n}}
}

// The three arms, each sabotaged independently. A check with two directions
// where only one is exercised is a check with one direction.
func TestAbiLiteralsDetectsBothDirections(t *testing.T) {
	root := litRepo(t)

	// ANTI-VACUITY FIRST. `len(clean) == 0` is satisfied by the check
	// examining NO abi notes at all, which is exactly what happened on the
	// first run here: the fixture stored `type` as a list, Str("type")
	// returned "", no spec was ever built, and the clean leg passed while
	// proving nothing. Assert the spec exists before asserting it is quiet.
	if specs := abiLiteralSpecs(litReg([]string{"tools/gate.sh"}, nil)); len(specs) != 1 {
		t.Fatalf("the fixture must produce exactly one spec, or every leg "+
			"below passes by examining nothing: %+v", specs)
	}

	// Clean: both matchers declared, one as a mirror and one as a mention.
	clean := checkAbiLiterals(root, litReg(
		[]string{"tools/gate.sh"}, []string{"tools/note.sh (a comment)"}))
	if len(clean) != 0 {
		t.Fatalf("a fully-declared set must pass: %v", clean)
	}

	// UNDECLARED: drop the mention, and note.sh is an unaccounted-for match.
	un := checkAbiLiterals(root, litReg([]string{"tools/gate.sh"}, nil))
	if len(un) != 1 || !strings.Contains(un[0], "tools/note.sh") {
		t.Fatalf("an undeclared matcher must be named: %v", un)
	}
	if strings.Contains(un[0], "tools/quiet.sh") {
		t.Fatalf("a non-matching file must never be reported: %v", un)
	}

	// UNMATCHED: a declared mirror that contains none of the literals. This is
	// the direction that turns a mirror list into fiction one entry at a time,
	// and it is the one a "did we miss anyone?" check does not think to have.
	um := checkAbiLiterals(root, litReg(
		[]string{"tools/gate.sh", "tools/gone.sh"},
		[]string{"tools/note.sh (a comment)"}))
	if len(um) != 1 || !strings.Contains(um[0], "tools/gone.sh") {
		t.Fatalf("a declared mirror matching nothing must be named: %v", um)
	}
}

// THE POSITIVE CONTROL. An empty hit set must fail, never pass.
//
// Written because it already happened: the first cut borrowed treeFiles, whose
// srcRe filter admits only kernel/arch/mm/usr C-family sources, so scanning
// `tools` yielded no files at all. The hit set was empty, and the check
// reported all fifteen declared mirrors as unmatched -- fifteen confident
// findings measured against no data.
//
// It was caught only because the UNMATCHED direction happened to be loud. Had
// the note declared no mirrors, the same broken scan would have printed a clean
// pass forever. That was luck, and this is the check that replaces it.
func TestAbiLiteralsEmptyHitSetIsNeverClean(t *testing.T) {
	root := litRepo(t)
	reg := litReg([]string{"tools/gate.sh"}, []string{"tools/note.sh (c)"})
	reg.Notes()[0].Front.Set("literal-scan", list([]string{"specs"})) // nothing there

	got := checkAbiLiterals(root, reg)
	if len(got) != 1 || !strings.Contains(got[0], "empty hit set is never a clean result") {
		t.Fatalf("a scan that matched nothing must FAIL: %v", got)
	}
}

// declaredPath must survive the annotated forms the live corpus actually uses.
// A matcher that only handles bare paths passes every hand-written fixture and
// fails on every real note -- which is precisely how abi-boot-banner stayed
// invisible to `owner kernel/main.c`.
func TestDeclaredPathStripsAnnotations(t *testing.T) {
	for in, want := range map[string]string{
		"kernel/main.c (boot_mark_complete)":                "kernel/main.c",
		"tools/test-fault.sh (also the MESSAGE bodies)":     "tools/test-fault.sh",
		"tools/stall-watch.py (`kernel base:` — see below)": "tools/stall-watch.py",
		"tools/test.sh": "tools/test.sh",
		"":              "",
	} {
		if got := declaredPath(in); got != want {
			t.Errorf("declaredPath(%q) = %q, want %q", in, got, want)
		}
	}
}
