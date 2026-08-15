package main

// The ownership answer's hard property: it must never say "no dossier, write a
// reference doc" about a surface the vault already carries. That is the one
// failure mode with a lasting cost -- it re-opens the two-sources-of-truth
// divergence the per-surface cutover exists to close, and it does so silently,
// because both documents look fine afterwards.
//
// Each test below is a regression against a defect the live corpus produced,
// not a demonstration of an intended behaviour.

import (
	"os"
	"path/filepath"
	"testing"
)

// TestTwinOfEncodesTheHouseConvention: kernel/foo.{c,h} is a convention, not a
// path -- the .c is in kernel/ and the .h is in kernel/include/thylacine/. A
// grep for the header finds nothing while the dossier that swept the surface
// sits one lookup away. In the live corpus this arm fires for 7 real headers;
// without it each would have answered UNOWNED.
//
// The negatives matter as much as the positives: an unrecognised shape must
// produce NO twin rather than a guess, because a fabricated twin is reported
// with exactly the confidence of a real one.
func TestTwinOfEncodesTheHouseConvention(t *testing.T) {
	cases := []struct{ in, want string }{
		{"kernel/include/thylacine/devcap.h", "kernel/devcap.c"},
		{"kernel/devcap.c", "kernel/include/thylacine/devcap.h"},
		// Not the convention: a nested kernel path, userspace, a doc.
		{"kernel/sub/dir/thing.c", ""},
		{"usr/corvus/src/main.rs", ""},
		{"docs/ARCHITECTURE.md", ""},
		{"kernel/include/thylacine/errno.h", "kernel/errno.c"},
	}
	for _, c := range cases {
		if got := twinOf(c.in); got != c.want {
			t.Errorf("twinOf(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// TestTwinExistenceIsReportedSeparately: the first version printed "twin
// kernel/errno.c is unowned too" for a file that has never existed. That is the
// phantom-path class the whole coverage arc was built to catch, committed by
// the coverage tooling itself -- and it is worse than saying nothing, because a
// reader takes "unowned" to mean "exists, unswept" and goes looking.
//
// twinOf is deliberately unconditional (errno.h yields kernel/errno.c above);
// the existence check is what separates a lead from an invention.
func TestTwinExistenceIsReportedSeparately(t *testing.T) {
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "kernel", "include", "thylacine"))
	writeFile(t, filepath.Join(root, "kernel", "devcap.c"), "x")
	// No kernel/errno.c -- that is the point.

	reg := &Registry{byID: map[string]*Note{}}
	idx := map[string][]*Note{}

	real := answerOwner(root, reg, idx, nil, "kernel/include/thylacine/devcap.h")
	if real.Twin != "kernel/devcap.c" || !real.TwinExists {
		t.Errorf("a twin that EXISTS must be marked so: %+v", real)
	}
	phantom := answerOwner(root, reg, idx, nil, "kernel/include/thylacine/errno.h")
	if phantom.TwinExists {
		t.Errorf("kernel/errno.c does not exist; TwinExists must be false: %+v", phantom)
	}
}

// TestCoveredIsNotOwned pins the two fields apart. Owned is the strict `code:`
// fact the coverage ledger counts; Covered is the cutover decision. Collapsing
// them either widens the ledger (how the FIRST coverage ledger rotted -- prose
// mention counted as coverage) or makes the command contradict itself (it
// printed "extend the twin's dossier" while exiting 1, so a human and a `||`
// got opposite answers from one invocation).
func TestCoveredIsNotOwned(t *testing.T) {
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "kernel", "include", "thylacine"))
	writeFile(t, filepath.Join(root, "kernel", "devcap.c"), "x")

	owner := &Note{ID: "sub-kernel-caps", Rel: "vault/system/sub-kernel-caps.md",
		Front: frontWith("code", "kernel/devcap.c")}
	reg := &Registry{byID: map[string]*Note{owner.ID: owner}, ordered: []*Note{owner}}
	idx := map[string][]*Note{"kernel/devcap.c": {owner}}

	a := answerOwner(root, reg, idx, nil, "kernel/include/thylacine/devcap.h")
	if a.Owned {
		t.Error("the header is NOT claimed in code: -- Owned must stay false " +
			"or the coverage ledger's definition has been widened by the back door")
	}
	if !a.Covered {
		t.Error("the surface IS swept via its twin -- Covered must be true, " +
			"or the command sends a session to duplicate a swept surface")
	}
	if len(a.TwinOwner) != 1 || a.TwinOwner[0] != "sub-kernel-caps" {
		t.Errorf("the lead must name the owning dossier: %+v", a.TwinOwner)
	}
}

// TestReferencedByBoundsItsMatch: the reference arm exists because abi-errno
// pins errno.h through `pinned-by:` while claiming nothing in `code:`. It is a
// substring search over free-text fields, so it needs bounding -- an unbounded
// one reports kernel/pipe.c as referenced by a note that names kernel/pipe.cpp,
// and a check that matches everything answers nothing.
func TestReferencedByBoundsItsMatch(t *testing.T) {
	hit := &Note{ID: "abi-errno",
		Front: frontWith("pinned-by", "_Static_assert per value (kernel/include/thylacine/errno.h, 20 asserts)")}
	near := &Note{ID: "note-near", Front: frontWith("mirrors", "kernel/pipe.cpp: the C++ port")}
	// A `code:` claim is ownership and must NOT also surface here, or one fact
	// gets counted twice under two different names.
	owns := &Note{ID: "sub-owner", Front: frontWith("code", "kernel/pipe.c")}
	reg := &Registry{byID: map[string]*Note{}, ordered: []*Note{hit, near, owns}}

	if got := referencedBy(reg, "kernel/include/thylacine/errno.h"); len(got) != 1 || got[0] != "abi-errno" {
		t.Errorf("a bounded frontmatter mention must be found: %v", got)
	}
	if got := referencedBy(reg, "kernel/pipe.c"); len(got) != 0 {
		t.Errorf("kernel/pipe.c must not match kernel/pipe.cpp, nor its own code: claim: %v", got)
	}
}

// TestReferenceIsALeadNotCoverage: a `pinned-by:` on a registry note must NOT
// reach exit 0. Found by main at the doc step, and it is the mirror image of
// the false-UNOWNED this file is otherwise built against -- I defended one
// direction so hard I opened the other.
//
// The question the exit status answers is "will the prose I am about to write
// end up somewhere a future reader finds it?". A twin says yes: a DOSSIER
// describes the surface. A registry pin says only that the file is SPOKEN FOR
// -- abi-errno pins errno.h's VALUES and abi-boot-banner pins extinction.c's
// STRINGS, and a description of a mechanism has nowhere to go in either. Exit 0
// there sends a session to the vault to write something the vault cannot hold.
//
// The lead is still reported, loudly, beside the write-the-reference-doc
// verdict -- an errno addition genuinely does need abi-errno updated too.
func TestReferenceIsALeadNotCoverage(t *testing.T) {
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "kernel", "include", "thylacine"))

	pin := &Note{ID: "abi-errno",
		Front: frontWith("pinned-by", "_Static_assert per value (kernel/include/thylacine/errno.h, 20 asserts)")}
	reg := &Registry{byID: map[string]*Note{}, ordered: []*Note{pin}}

	a := answerOwner(root, reg, map[string][]*Note{}, nil, "kernel/include/thylacine/errno.h")
	if a.Covered {
		t.Error("a registry pin is a LEAD, not coverage -- exit 0 here sends the " +
			"author to write a mechanism description into a value registry")
	}
	if len(a.RefBy) != 1 || a.RefBy[0] != "abi-errno" {
		t.Errorf("the lead must still be reported: %v", a.RefBy)
	}
}

func mkdirAll(t *testing.T, p string) {
	t.Helper()
	if err := os.MkdirAll(p, 0o755); err != nil {
		t.Fatal(err)
	}
}

func writeFile(t *testing.T, p, body string) {
	t.Helper()
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func frontWith(k, v string) *Front {
	f := newFront()
	f.Set(k, list([]string{v}))
	return f
}
