package main

// The code->dossier gate's sabotage suite. Each test proves a DISCRIMINATION,
// not just a detection: audit:hard blocks where light only warns (the tier),
// a co-stage clears it, each escape works AND an empty escape does not, the
// .c/.h twin resolves, and an unowned edit is silent. A gate that only ever
// fired would pass a "does it block?" test while breaking every commit; the
// pairs below are what separate the gate that works from the one that is stuck.

import (
	"strings"
	"testing"
)

func gitInitCommit(t *testing.T, root, msg string) {
	t.Helper()
	gitT(t, root, "init", "-q")
	gitT(t, root, "config", "user.email", "t@t")
	gitT(t, root, "config", "user.name", "t")
	gitT(t, root, "add", "-A")
	gitT(t, root, "commit", "-qm", msg)
}

// setupHardCode: sub-t-x flipped to audit:hard and committed, so kernel/t.c is
// a committed, audit:hard-owned surface. A code edit staged alone is then the
// blocking case.
func setupHardCode(t *testing.T) string {
	root := fixture(t)
	mutate(t, root, "vault/system/t/sub-t-x.md", "audit: light", "audit: hard")
	gitInitCommit(t, root, "fixture (sub-t-x audit:hard)")
	return root
}

func wantGateClean(t *testing.T, fails, warns []string) {
	t.Helper()
	if len(fails) != 0 || len(warns) != 0 {
		t.Fatalf("want gate clean, got fails=%v warns=%v", fails, warns)
	}
}

func TestDossierGateBlocksHardCode(t *testing.T) {
	root := setupHardCode(t)
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	gitT(t, root, "add", "kernel/t.c")
	reg, _ := loadRegistry(root)
	fails, _ := dossierGate(root, reg, "kernel: tweak t")
	wantFailContaining(t, fails, "kernel/t.c changed but its dossier [[sub-t-x]]")
	wantFailContaining(t, fails, "audit:hard")
}

// The tier boundary: the SAME edit, to code owned by an audit:light dossier,
// must WARN and never block. Paired with TestDossierGateBlocksHardCode, this is
// the whole of "block audit:hard, warn rest".
func TestDossierGateWarnsNonHardCode(t *testing.T) {
	root := fixtureGit(t) // sub-t-x is audit:light here
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	gitT(t, root, "add", "kernel/t.c")
	reg, _ := loadRegistry(root)
	fails, warns := dossierGate(root, reg, "kernel: tweak t")
	if len(fails) != 0 {
		t.Fatalf("audit:light must not block; got fails=%v", fails)
	}
	found := false
	for _, w := range warns {
		if strings.Contains(w, "kernel/t.c changed but its dossier [[sub-t-x]]") {
			found = true
		}
	}
	if !found {
		t.Fatalf("want the non-hard WARN, got %v", warns)
	}
}

func TestDossierGateClearedByCoStage(t *testing.T) {
	root := setupHardCode(t)
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	// Co-stage the dossier (a body edit is enough to put it in the index).
	mutate(t, root, "vault/system/t/sub-t-x.md", "## Purpose\np\n", "## Purpose\np more\n")
	gitT(t, root, "add", "-A")
	reg, _ := loadRegistry(root)
	fails, warns := dossierGate(root, reg, "kernel + dossier")
	wantGateClean(t, fails, warns)
}

func TestDossierGateEscapedByTrailer(t *testing.T) {
	root := setupHardCode(t)
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	gitT(t, root, "add", "kernel/t.c")
	reg, _ := loadRegistry(root)
	msg := "kernel: tweak t\n\nBody paragraph.\n\nNo-dossier-change: rung the vault (yip)\n"
	fails, warns := dossierGate(root, reg, msg)
	wantGateClean(t, fails, warns)
}

func TestDossierGateEscapedByChgField(t *testing.T) {
	root := setupHardCode(t)
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	writeNote(t, root, "vault/record/changes/chg-2026-01-05-defer.md",
		"---\nid: chg-2026-01-05-defer\ntype: chg\ndate: 2026-01-05\narc: arc-t\n"+
			"commits: [\"eeee5555\"]\ntouched: []\nno-dossier-change: deliberate\n"+
			"depth: skeletal\n---\nBody.\n")
	gitT(t, root, "add", "-A")
	reg, _ := loadRegistry(root)
	fails, warns := dossierGate(root, reg, "")
	wantGateClean(t, fails, warns)
}

// The escape must carry a reason. A bare key -- or one that only appears in a
// stripped comment line -- must NOT bypass the block, or the escape becomes a
// silent off-switch and the gate stops being a reliable reminder.
func TestDossierGateEmptyOrCommentedTrailerDoesNotEscape(t *testing.T) {
	root := setupHardCode(t)
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* edit */")
	gitT(t, root, "add", "kernel/t.c")
	reg, _ := loadRegistry(root)
	for _, msg := range []string{
		"kernel: tweak\n\nNo-dossier-change:\n",           // bare key
		"kernel: tweak\n\nNo-dossier-change:   \n",        // key + whitespace
		"kernel: tweak\n\n# No-dossier-change: template\n", // a stripped comment
	} {
		fails, _ := dossierGate(root, reg, msg)
		wantFailContaining(t, fails, "audit:hard")
	}
}

// The .c/.h twin: editing the header of an audit:hard-owned .c blocks through
// the dossier keyed on the .c, because the two are one surface.
func TestDossierGateResolvesTwin(t *testing.T) {
	root := fixture(t)
	writeNote(t, root, "kernel/t2.c", "int t2;\n")
	writeNote(t, root, "kernel/include/thylacine/t2.h", "extern int t2;\n")
	writeNote(t, root, "vault/system/t/sub-t-two.md",
		"---\nid: sub-t-two\ntype: sub\nparent: moc-t\ncode: [kernel/t2.c]\n"+
			"audit: hard\nguarded-by: []\nvalidated-by: [prose]\n---\n"+subBody())
	gitInitCommit(t, root, "fixture + t2 pair, audit:hard")
	mutate(t, root, "kernel/include/thylacine/t2.h", "extern int t2;", "extern int t2; /* x */")
	gitT(t, root, "add", "kernel/include/thylacine/t2.h")
	reg, _ := loadRegistry(root)
	fails, _ := dossierGate(root, reg, "kernel: t2 header")
	wantFailContaining(t, fails,
		"kernel/include/thylacine/t2.h changed but its dossier [[sub-t-two]]")
}

func TestDossierGateSilentOnUnownedAndNonCode(t *testing.T) {
	root := fixtureGit(t)
	// A new unowned code file and a non-code file, staged together.
	writeNote(t, root, "kernel/unowned.c", "int u;\n")
	writeNote(t, root, "docs/reference/99-x.md", "prose\n")
	gitT(t, root, "add", "-A")
	reg, _ := loadRegistry(root)
	fails, warns := dossierGate(root, reg, "misc")
	wantGateClean(t, fails, warns)
}

// One dossier owning two staged files is reported once, not twice.
func TestDossierGateDedupsOwner(t *testing.T) {
	root := fixture(t)
	writeNote(t, root, "kernel/t3.c", "int t3;\n")
	// sub-t-x now owns both kernel/t.c and kernel/t3.c, at audit:hard.
	mutate(t, root, "vault/system/t/sub-t-x.md", "code: [kernel/t.c]", "code: [kernel/t.c, kernel/t3.c]")
	mutate(t, root, "vault/system/t/sub-t-x.md", "audit: light", "audit: hard")
	gitInitCommit(t, root, "fixture, sub-t-x owns t.c + t3.c, audit:hard")
	mutate(t, root, "kernel/t.c", "int t;", "int t; /* e */")
	mutate(t, root, "kernel/t3.c", "int t3;", "int t3; /* e */")
	gitT(t, root, "add", "kernel/t.c", "kernel/t3.c")
	reg, _ := loadRegistry(root)
	fails, _ := dossierGate(root, reg, "kernel: t + t3")
	n := 0
	for _, f := range fails {
		if strings.Contains(f, "[[sub-t-x]]") {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("want sub-t-x reported once, got %d in %v", n, fails)
	}
}
