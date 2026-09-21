package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// A cited path resolves against the TRACKED tree. The control is the tracked
// file beside it: without it, "the untracked one does not resolve" is also
// satisfied by a resolver that resolves nothing.
func TestDeclResolvesIgnoresUntrackedFiles(t *testing.T) {
	root := t.TempDir()
	run := func(args ...string) {
		t.Helper()
		cmd := exec.Command("git", append([]string{"-C", root}, args...)...)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git %v: %v\n%s", args, err, out)
		}
	}
	run("init", "-q")
	for _, f := range []string{"tools/tracked.sh", "tools/untracked.sh", "kernel/include/thylacine/foo.h"} {
		p := filepath.Join(root, f)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	run("add", "tools/tracked.sh", "kernel/include/thylacine/foo.h")

	if !declResolves(root, "tools/tracked.sh") {
		t.Fatal("a tracked file must resolve (the control)")
	}
	if !declResolves(root, "tools/") || !declResolves(root, "kernel/include") {
		t.Fatal("a directory holding a tracked file must resolve")
	}
	if !declResolves(root, "kernel/foo.h") {
		t.Fatal("the kernel/<x>.h -> kernel/include/thylacine/<x>.h convention must still resolve")
	}
	if declResolves(root, "tools/untracked.sh") {
		t.Fatal("an UNTRACKED file resolved: the view would differ between checkouts")
	}
}
