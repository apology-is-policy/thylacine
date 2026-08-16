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
	"io"
	"os"
	"path/filepath"
	"strings"
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

// The title is reported because an ID IS NOT A SCOPE. `sub-stratum-boot` reads
// as "the whole Stratum boot"; its title -- "Bringup -- spawn, wait for an
// event, attach, pivot" -- is what tells a writer holding foreign-shell-gate
// prose that this dossier is not where it goes. Measured 2026-08-15: that
// dossier is the SOLE owner of a 9771-line file it describes a few hundred
// lines of, and the ratified cutover reads exit 0 as "the prose belongs there".
//
// The control is the second half: a hit with no title must print no title
// LINE, not an empty pair of quotes. Without it this test would pass against
// an implementation that emitted `""` for every dossier, which is noise
// wearing the shape of a signal.
func TestOwnerReportsTheDossierTitle(t *testing.T) {
	titled := ownerHit{
		Note: "sub-stratum-boot", Rel: "vault/system/stratum/sub-stratum-boot.md",
		Audit: "hard", Updated: "2026-08-15", Claim: "usr/joey/joey.c",
		Title: "Bringup -- spawn, wait for an event, attach, pivot",
	}
	out := captureOwner(ownerAnswer{
		Path: "usr/joey/joey.c", Kind: "file", Owned: true, Covered: true,
		Owners: []ownerHit{titled},
	})
	if !strings.Contains(out, `"Bringup -- spawn, wait for an event, attach, pivot"`) {
		t.Fatalf("owner did not report the title; a reader cannot judge scope:\n%s", out)
	}

	untitled := titled
	untitled.Title = ""
	out = captureOwner(ownerAnswer{
		Path: "usr/joey/joey.c", Kind: "file", Owned: true, Covered: true,
		Owners: []ownerHit{untitled},
	})
	if strings.Contains(out, `""`) {
		t.Fatalf("a title-less hit printed empty quotes -- noise, not signal:\n%s", out)
	}
}

// captureOwner runs printOwner and returns what it wrote. Kept here rather
// than asserting on the struct: the struct always had the data available, and
// what the finding was about is whether a HUMAN sees it at the decision point.
func captureOwner(a ownerAnswer) string {
	old := os.Stdout
	r, w, _ := os.Pipe()
	os.Stdout = w
	printOwner(a)
	_ = w.Close()
	os.Stdout = old
	b, _ := io.ReadAll(r)
	return string(b)
}

// An OWNED path must still report its pins. `if a.Owned { return }` used to sit
// ABOVE the RefBy print, so the better-covered a file was the LESS this command
// said about it -- and a pin is a CO-UPDATE obligation, orthogonal to the
// "where does my prose go?" question ownership answers.
//
// Raised by main 2026-08-16 as the condition on deriving the boot-banner mirror
// check: the check has to fire where the CHANGE happens, not only where the
// registry lives. The worked case is exactly that: `kernel/main.c` prints the
// banner and is owned by a boot dossier, so the single edit most likely to
// break the banner ABI reported nothing about it -- while unowned
// `kernel/extinction.c`, one line down in the same answer, did.
//
// Asserted on the OUTPUT, not on `a.RefBy`: the field was populated the whole
// time. Only the human could not see it, which is the same reason captureOwner
// exists at all.
func TestOwnedPathStillReportsItsPins(t *testing.T) {
	owned := ownerAnswer{
		Path: "kernel/main.c", Kind: "file", Owned: true, Covered: true,
		Owners: []ownerHit{{Note: "sub-kernel-boot-sequence", Rel: "vault/system/kernel/boot/sub-kernel-boot-sequence.md", Title: "The boot sequence"}},
		RefBy:  []string{"abi-boot-banner"},
	}
	out := captureOwner(owned)
	if !strings.Contains(out, "abi-boot-banner") {
		t.Fatalf("an owned path must still surface its pin -- this is the whole "+
			"co-update obligation:\n%s", out)
	}
	// The control: ownership itself must still be reported, and the UNOWNED-only
	// advice must NOT leak onto an owned path. Without this the test is
	// satisfied by printing everything unconditionally, which would tell a
	// covered surface to go write a reference doc.
	if !strings.Contains(out, "sub-kernel-boot-sequence") {
		t.Fatalf("the owner is still the primary answer:\n%s", out)
	}
	if strings.Contains(out, "write the reference doc") {
		t.Fatalf("an owned path must never be routed to the reference docs:\n%s", out)
	}
}

// The leg above is NOT sufficient, and finding out why is the finding.
//
// It hand-builds an ownerAnswer with RefBy already populated -- a state
// answerOwner could NOT produce, because it also returned early on `Owned`
// before ever calling referencedBy. So there were TWO returns on the same
// predicate, in the computation and in the report, and each hid the other:
// removing either alone changes no observable output, and a unit test on the
// half you fixed passes while the command's behaviour is unchanged. It did.
//
// This leg goes through answerOwner, so it is satisfiable only by a path that
// really produces the pin.
func TestOwnedPathComputesItsPinsEndToEnd(t *testing.T) {
	root := t.TempDir()
	mkdirAll(t, filepath.Join(root, "kernel"))
	writeFile(t, filepath.Join(root, "kernel", "main.c"), "x")

	owner := &Note{ID: "sub-kernel-boot-sequence", Rel: "vault/system/sub-boot.md",
		Front: frontWith("code", "kernel/main.c")}
	pin := &Note{ID: "abi-boot-banner",
		Front: frontWith("pinned-by", "kernel/main.c (boot_mark_complete)")}
	reg := &Registry{byID: map[string]*Note{owner.ID: owner, pin.ID: pin},
		ordered: []*Note{owner, pin}}
	idx := map[string][]*Note{"kernel/main.c": {owner}}

	a := answerOwner(root, reg, idx, nil, "kernel/main.c")
	if !a.Owned {
		t.Fatalf("precondition: the path must be owned, or this proves nothing: %+v", a)
	}
	if len(a.RefBy) != 1 || a.RefBy[0] != "abi-boot-banner" {
		t.Fatalf("an owned path must still COMPUTE its pins, not only print "+
			"them when handed them: %+v", a.RefBy)
	}
	// The annotated form is the one that matters and the one live in the
	// corpus: `pinned-by` entries carry a "(symbol)" suffix, and a bare-path
	// fixture would pass against a matcher that cannot handle the real data.
	if !strings.Contains(captureOwner(a), "abi-boot-banner") {
		t.Fatalf("and it must reach the human")
	}
}

// A document is not a surface, and UNOWNED about one is the most damaging wrong
// answer this command can give: it routes the caller AWAY from the vault
// ("write the reference doc as today") for a surface a dossier may well carry.
// Reported by main 2026-08-15 -- every docs/reference/NN-*.md answered UNOWNED
// while its code surface answered OWNED, so the tool gave opposite answers for a
// file and the document describing it.
//
// Three legs, because the refusal has to hold in three different places and
// each could regress alone.
func TestOwnerRefusesDocumentPaths(t *testing.T) {
	for _, p := range []string{
		"docs/reference/145-vivarium.md",
		"docs/manual/10-shells.md",
	} {
		if !notCodeSurface(p) {
			t.Fatalf("%s should be refused as a document", p)
		}
	}
	// The control: the refusal must not swallow real code. A docs-SHAPED path
	// outside the two document trees, and an ordinary source file, both stay
	// answerable -- without this leg the test would pass against
	// `return strings.HasPrefix(p, "docs/")` or even `return true`.
	for _, p := range []string{
		"docs/ARCHITECTURE.md", // scripture, not a per-surface reference doc
		"kernel/vivarium.c",
		"tools/build.sh",
	} {
		if notCodeSurface(p) {
			t.Fatalf("%s must still be answerable, not refused", p)
		}
	}

	out := captureOwner(ownerAnswer{
		Path: "docs/reference/145-vivarium.md", Kind: "file", NotCode: true,
	})
	// The VERDICT line, not the whole output. A bare Contains(out, "UNOWNED")
	// fails here and the failure is the test's, not the code's: the refusal
	// EXPLAINS why UNOWNED would be wrong, so the word appears in its own
	// reasoning. A substring check over prose matches more than it means --
	// which is the same mistake, in miniature, as the unbounded `sid` grep
	// that matched inside ASID earlier today.
	verdict := strings.SplitN(out, "\n", 2)[0]
	if strings.Contains(verdict, "UNOWNED") {
		t.Fatalf("a document path VERDICT was UNOWNED -- the away-from-vault routing:\n%s", out)
	}
	if !strings.Contains(verdict, "NOT A CODE SURFACE") {
		t.Fatalf("a document path did not print the refusal verdict:\n%s", out)
	}
}
