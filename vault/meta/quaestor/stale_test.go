package main

// The staleness census's one hard property: a change is dated by when it
// became visible on THIS branch, not by when its author wrote it.
//
// This is a regression, not a demonstration. The first version of
// lastTouched used a plain `git log`, which reports a commit's own date --
// and the live vault immediately showed why that is wrong: a dossier
// written on 2026-08-01 was reported current against resolver changes
// authored 2026-07-30 that did not reach the branch until 2026-08-05. The
// dossier looked newer than a change it could not have seen. The fixture
// below reproduces exactly that topology.

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func git(t *testing.T, dir string, env []string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(),
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@t",
		"GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@t")
	cmd.Env = append(cmd.Env, env...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
}

func at(date string) []string {
	return []string{"GIT_AUTHOR_DATE=" + date, "GIT_COMMITTER_DATE=" + date}
}

func write(t *testing.T, dir, name, body string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestLastTouchedUsesArrivalNotAuthorship builds the topology that broke
// the first implementation:
//
//	main:  A(01-01) ------------------- M(01-10)
//	side:      \-- B(01-05, edits f) --/
//
// f's own last commit is B, dated 01-05. But a reader of main saw nothing
// change in f until M on 01-10. A dossier dated 01-07 is therefore STALE,
// and only the arrival date says so.
func TestLastTouchedUsesArrivalNotAuthorship(t *testing.T) {
	dir := t.TempDir()
	git(t, dir, nil, "init", "-q", "-b", "main")

	write(t, dir, "f.c", "one\n")
	write(t, dir, "untouched.c", "steady\n")
	git(t, dir, nil, "add", ".")
	git(t, dir, at("2026-01-01T00:00:00+00:00"), "commit", "-q", "-m", "A")

	git(t, dir, nil, "checkout", "-q", "-b", "side")
	write(t, dir, "f.c", "two\n")
	git(t, dir, nil, "add", ".")
	git(t, dir, at("2026-01-05T00:00:00+00:00"), "commit", "-q", "-m", "B")

	git(t, dir, nil, "checkout", "-q", "main")
	git(t, dir, at("2026-01-10T00:00:00+00:00"),
		"merge", "-q", "--no-ff", "-m", "M", "side")

	h, err := lastTouched(dir, "")
	if err != nil {
		t.Fatalf("lastTouched: %v", err)
	}

	// The property. Authorship says 01-05; arrival says 01-10.
	if h.last["f.c"] != "2026-01-10" {
		t.Errorf("f.c: got %q, want 2026-01-10 (the merge that carried it in). "+
			"Got 2026-01-05 => the walk is reading authorship, not arrival",
			h.last["f.c"])
	}

	// Discrimination: a file the merge did not touch keeps its own date, so
	// the check cannot be passing by dating everything at the merge.
	if h.last["untouched.c"] != "2026-01-01" {
		t.Errorf("untouched.c: got %q, want 2026-01-01 -- a file the merge did "+
			"not touch must not inherit the merge's date", h.last["untouched.c"])
	}
}

// TestLastTouchedKeepsNewestOnRewrite pins the walk-order assumption: the
// log is newest-first, so the FIRST sighting of a path is its most recent
// change and a later (older) sighting must not overwrite it.
func TestLastTouchedKeepsNewestOnRewrite(t *testing.T) {
	dir := t.TempDir()
	git(t, dir, nil, "init", "-q", "-b", "main")

	write(t, dir, "f.c", "one\n")
	git(t, dir, nil, "add", ".")
	git(t, dir, at("2026-02-01T00:00:00+00:00"), "commit", "-q", "-m", "first")

	write(t, dir, "f.c", "two\n")
	git(t, dir, nil, "add", ".")
	git(t, dir, at("2026-02-09T00:00:00+00:00"), "commit", "-q", "-m", "second")

	h, err := lastTouched(dir, "")
	if err != nil {
		t.Fatalf("lastTouched: %v", err)
	}
	if h.last["f.c"] != "2026-02-09" {
		t.Errorf("f.c: got %q, want 2026-02-09 (the later edit)", h.last["f.c"])
	}
}

// TestCodeTargetMatchesValidatorReading pins the shared reading of a
// `code:` entry. The census and checkCodePaths must strip an entry the
// same way -- two readings of one field is the drift both exist to catch.
func TestCodeTargetMatchesValidatorReading(t *testing.T) {
	cases := []struct{ in, repo, path string }{
		{"kernel/stalk.c", "", "kernel/stalk.c"},
		{"stratum: src/fs/fs.c", "stratum", "src/fs/fs.c"},
		{"docs/ARCHITECTURE.md section 5", "", "docs/ARCHITECTURE.md"},
	}
	for _, c := range cases {
		repo, path := codeTarget(c.in)
		if repo != c.repo || path != c.path {
			t.Errorf("codeTarget(%q) = (%q,%q), want (%q,%q)",
				c.in, repo, path, c.repo, c.path)
		}
	}
}
