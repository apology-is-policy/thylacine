package main

// The sabotage-probe suite: every class the one-shot parity gate ran
// against lint.py before it retired, carried forward as fixtures. Each
// test builds a fresh minimal vault (plus a git history where the staged
// checks need one) and proves the linter FAILS it -- a probe that
// quietly passes is a broken fixture, not a green result.

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func writeNote(t *testing.T, root, rel, content string) {
	t.Helper()
	abs := filepath.Join(root, rel)
	if err := os.MkdirAll(filepath.Dir(abs), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(abs, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func subBody() string {
	var b strings.Builder
	for _, s := range SUB_SECTIONS {
		b.WriteString("## " + s + "\np\n")
	}
	return b.String()
}

// fixture builds a minimal valid vault: home + moc + sub + seam on the
// Present plane; arc + chg + adt + a fixed fnd + a deferred-with-seam
// fnd on the Record plane.
func fixture(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	writeNote(t, root, "vault/home.md",
		"---\nid: home\ntype: moc\ntitle: \"Home\"\n---\nRoot.\n")
	writeNote(t, root, "vault/system/t/moc-t.md",
		"---\nid: moc-t\ntype: moc\nparent: home\n---\nArea.\n")
	writeNote(t, root, "vault/system/t/sub-t-x.md",
		"---\nid: sub-t-x\ntype: sub\nparent: moc-t\ncode: [kernel/t.c]\n"+
			"audit: light\nguarded-by: []\nvalidated-by: [prose]\n---\n"+subBody())
	writeNote(t, root, "vault/seams/seam-t-1.md",
		"---\nid: seam-t-1\ntype: seam\nstatus: open\nsurface: [sub-t-x]\n"+
			"opened-by: chg-2026-01-01-t\n---\nOpen seam.\n")
	writeNote(t, root, "vault/record/arcs/arc-t.md",
		"---\nid: arc-t\ntype: arc\nstatus: active\nchunks: [chg-2026-01-01-t]\n"+
			"follow-ons: []\n---\nArc body.\n")
	writeNote(t, root, "vault/record/changes/chg-2026-01-01-t.md",
		"---\nid: chg-2026-01-01-t\ntype: chg\ndate: 2026-01-01\narc: arc-t\n"+
			"commits: [\"aaaa1111\"]\ntouched: []\ndepth: skeletal\n---\nChange body.\n")
	writeNote(t, root, "vault/record/audits/adt-t-r1.md",
		"---\nid: adt-t-r1\ntype: adt\ndate: 2026-01-02\nscope: [sub-t-x]\n"+
			"reviewer: self\nmodel-start: claude\nmodel-end: claude\n"+
			"verdict: clean\ncounts: {p0: 0}\nfindings: [fnd-t-r1-f1, fnd-t-r1-f2]\n"+
			"---\nRound body.\n")
	writeNote(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"---\nid: fnd-t-r1-f1\ntype: fnd\ntitle: \"A test defect\"\n"+
			"round: adt-t-r1\nseverity: P2\nstatus: fixed\nsurface: [sub-t-x]\n"+
			"threatens: []\nfixed-by: chg-2026-01-01-t\n---\n"+
			"## Prosecution\nChain.\n\n## Disposition\nFixed by the change.\n")
	writeNote(t, root, "vault/record/findings/fnd-t-r1-f2.md",
		"---\nid: fnd-t-r1-f2\ntype: fnd\ntitle: \"A deferred defect\"\n"+
			"round: adt-t-r1\nseverity: P3\nstatus: deferred\nsurface: [sub-t-x]\n"+
			"threatens: []\nseam: seam-t-1\n---\n## Prosecution\nChain.\n")
	return root
}

func runLint(root string, staged bool) (fails, warns []string) {
	reg, pre := loadRegistry(root)
	fails, warns = validate(reg, pre)
	fails = append(fails, checkViews(reg)...)
	if staged {
		sf, sw := stagedChecks(root, reg)
		fails = append(fails, sf...)
		warns = append(warns, sw...)
	}
	return fails, warns
}

func wantClean(t *testing.T, fails, warns []string) {
	t.Helper()
	if len(fails) != 0 || len(warns) != 0 {
		t.Fatalf("want clean, got fails=%v warns=%v", fails, warns)
	}
}

func wantFailContaining(t *testing.T, fails []string, substr string) {
	t.Helper()
	for _, f := range fails {
		if strings.Contains(f, substr) {
			return
		}
	}
	t.Fatalf("no failure containing %q in %v", substr, fails)
}

func mutate(t *testing.T, root, rel, old, new string) {
	t.Helper()
	abs := filepath.Join(root, rel)
	b, err := os.ReadFile(abs)
	if err != nil {
		t.Fatal(err)
	}
	s := strings.Replace(string(b), old, new, 1)
	if s == string(b) {
		t.Fatalf("mutation had no effect: %q not in %s", old, rel)
	}
	if err := os.WriteFile(abs, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestFixtureLintsClean(t *testing.T) {
	fails, warns := runLint(fixture(t), false)
	wantClean(t, fails, warns)
}

func TestEmptyRegistryFailsClosed(t *testing.T) {
	// The gate must never pass open: a root with no notes (wrong root,
	// hook-context cwd drift, a vanished vault) is a FAILURE, not a
	// vacuous green. The first live hook run passed exactly this way.
	root := t.TempDir()
	code := lintRun(root, "--all")
	if code == 0 {
		t.Fatal("lint on an empty root returned success (the gate passed open)")
	}
}

func TestDanglingEdgeFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"fixed-by: chg-2026-01-01-t", "fixed-by: chg-2099-01-01-nope")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "fixed-by -> unknown id 'chg-2099-01-01-nope'")
}

func TestDanglingWikilinkFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/record/arcs/arc-t.md",
		"Arc body.", "Arc body. See [[no-such-note]].")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "dangling wikilink [[no-such-note]]")
}

func TestDroppedDossierSectionFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/system/t/sub-t-x.md",
		"## Performance", "## Perf-formerly")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "dossier sections missing (no waiver): ['Performance']")
}

func TestWaivedDossierSectionAccepted(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/system/t/sub-t-x.md",
		"## Performance\np\n", "> waived: Performance -- not measured yet\n")
	fails, warns := runLint(root, false)
	wantClean(t, fails, warns)
}

func TestDeferredWithoutSeamFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f2.md",
		"seam: seam-t-1\n", "")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "status=deferred without a seam-* link")
}

func TestUnterminatedFlowListFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/seams/seam-t-1.md",
		"type: seam\n", "type: seam\nbogus: [a,\n  b]\n")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "unterminated flow list")
}

func TestUpdatedForbiddenOnRecord(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/record/changes/chg-2026-01-01-t.md",
		"depth: skeletal\n", "depth: skeletal\nupdated: 2026-01-05\n")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "'updated' is forbidden on the Record plane")
}

func TestEnumViolationFails(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"severity: P2", "severity: P9")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "severity='P9' not in ['P0', 'P1', 'P2', 'P3']")
}

func TestMirrorsCheckedRequired(t *testing.T) {
	root := fixture(t)
	writeNote(t, root, "vault/abis/abi-t.md",
		"---\nid: abi-t\ntype: abi\nkind: struct\nstability: append-only\n"+
			"pinned-by: []\nmirrors: [usr/lib/a.rs, usr/lib/b.rs]\n---\nAn ABI.\n")
	mutate(t, root, "vault/record/changes/chg-2026-01-01-t.md",
		"touched: []", "touched: [abi-t]")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "touched abi has 2 mirrors; mirrors-checked covers 0")
	mutate(t, root, "vault/record/changes/chg-2026-01-01-t.md",
		"depth: skeletal\n", "depth: skeletal\nmirrors-checked: [usr/lib/a.rs, usr/lib/b.rs]\n")
	fails, warns := runLint(root, false)
	wantClean(t, fails, warns)
}

func TestFileLineCitationWarnsOnPresentOnly(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/system/t/sub-t-x.md",
		"## Purpose\np\n", "## Purpose\nSee kernel/t.c:42 for the guard.\n")
	// Record plane: the same citation is allowed (frozen prosecution).
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"Chain.", "Chain at kernel/t.c:42.")
	fails, warns := runLint(root, false)
	if len(fails) != 0 {
		t.Fatalf("want no fails, got %v", fails)
	}
	if len(warns) != 1 || !strings.Contains(warns[0], "file:line citation 'kernel/t.c:42'") {
		t.Fatalf("want exactly the Present-plane R4 warn, got %v", warns)
	}
}

// --- views ---

func addClosedView(t *testing.T, root string) {
	writeNote(t, root, "vault/views/view-closed-sub-t-x.md",
		"---\nid: view-closed-sub-t-x\ntype: view\nquery: \"closed:sub-t-x\"\n---\n"+
			genBegin+"\n"+genEnd+"\n")
	reg, _ := loadRegistry(root)
	renderViews(reg)
}

func TestRenderedViewIsClean(t *testing.T) {
	root := fixture(t)
	addClosedView(t, root)
	fails, warns := runLint(root, false)
	wantClean(t, fails, warns)
	b, _ := os.ReadFile(filepath.Join(root, "vault/views/view-closed-sub-t-x.md"))
	if !strings.Contains(string(b), "fnd-t-r1-f1") {
		t.Fatal("preamble missing the fixed finding")
	}
	if strings.Contains(string(b), "fnd-t-r1-f2") {
		t.Fatal("preamble lists a deferred finding (must be excluded)")
	}
	if !strings.Contains(string(b), "— Fixed by the change.") {
		t.Fatal("preamble missing the Disposition excerpt")
	}
}

func TestStaleViewFails(t *testing.T) {
	root := fixture(t)
	addClosedView(t, root)
	mutate(t, root, "vault/views/view-closed-sub-t-x.md",
		genBegin, genBegin+"\nHAND EDIT")
	fails, _ := runLint(root, false)
	wantFailContaining(t, fails, "stale generated body (run quaestor render)")
}

func TestClosedScalarSurfaceSubstringLaw(t *testing.T) {
	// Inherited law (ported deliberately): a SCALAR surface matches by
	// substring, a LIST surface by exact membership.
	root := fixture(t)
	writeNote(t, root, "vault/record/findings/fnd-t-r1-f3.md",
		"---\nid: fnd-t-r1-f3\ntype: fnd\ntitle: \"Scalar surface\"\n"+
			"round: adt-t-r1\nseverity: P3\nstatus: documented\n"+
			"surface: sub-t-x-extra\nthreatens: []\n---\n## Prosecution\nc.\n")
	reg, _ := loadRegistry(root)
	if !strings.Contains(renderClosed(reg, "sub-t-x"), "fnd-t-r1-f3") {
		t.Fatal("scalar-surface substring semantics changed")
	}
}

// --- staged (git-index) checks ---

func gitT(t *testing.T, root string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = root
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
}

func fixtureGit(t *testing.T) string {
	root := fixture(t)
	gitT(t, root, "init", "-q")
	gitT(t, root, "config", "user.email", "t@t")
	gitT(t, root, "config", "user.name", "t")
	gitT(t, root, "add", "-A")
	gitT(t, root, "commit", "-qm", "fixture")
	return root
}

func TestRecordBodyEditFailsStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"Chain.", "Chain. RETROACTIVE EDIT.")
	gitT(t, root, "add", "-A")
	fails, _ := runLint(root, true)
	wantFailContaining(t, fails, "Record-plane body changed (R3: append-only")
}

func TestNonClosureFieldFailsStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"severity: P2", "severity: P3")
	gitT(t, root, "add", "-A")
	fails, _ := runLint(root, true)
	wantFailContaining(t, fails, "non-closure Record fields changed: ['severity']")
}

func TestClosureWithoutChgFailsStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"status: fixed", "status: withdrawn")
	gitT(t, root, "add", "-A")
	fails, _ := runLint(root, true)
	wantFailContaining(t, fails,
		"closure fields ['status'] changed with no staged chg-* note linking 'fnd-t-r1-f1'")
}

func TestClosureWithLinkingChgPassesStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/findings/fnd-t-r1-f1.md",
		"status: fixed", "status: withdrawn")
	writeNote(t, root, "vault/record/changes/chg-2026-01-03-close.md",
		"---\nid: chg-2026-01-03-close\ntype: chg\ndate: 2026-01-03\narc: arc-t\n"+
			"commits: [\"bbbb2222\"]\ntouched: []\nclosed: [fnd-t-r1-f1]\n"+
			"depth: skeletal\n---\nCloses the finding.\n")
	gitT(t, root, "add", "-A")
	fails, warns := runLint(root, true)
	wantClean(t, fails, warns)
}

func TestChgCommitsSelfFixupAllowedStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/changes/chg-2026-01-01-t.md",
		"commits: [\"aaaa1111\"]", "commits: [\"cccc3333\"]")
	gitT(t, root, "add", "-A")
	fails, warns := runLint(root, true)
	wantClean(t, fails, warns)
}

func TestActiveArcMutableStaged(t *testing.T) {
	root := fixtureGit(t)
	mutate(t, root, "vault/record/arcs/arc-t.md",
		"chunks: [chg-2026-01-01-t]", "chunks: [chg-2026-01-01-t, chg-2026-01-03-close]")
	writeNote(t, root, "vault/record/changes/chg-2026-01-03-close.md",
		"---\nid: chg-2026-01-03-close\ntype: chg\ndate: 2026-01-03\narc: arc-t\n"+
			"commits: [\"bbbb2222\"]\ntouched: []\ndepth: skeletal\n---\nBody.\n")
	gitT(t, root, "add", "-A")
	fails, warns := runLint(root, true)
	wantClean(t, fails, warns)
}

func TestFrozenArcImmutableStaged(t *testing.T) {
	root := fixture(t)
	writeNote(t, root, "vault/record/arcs/arc-u.md",
		"---\nid: arc-u\ntype: arc\nstatus: complete\nchunks: [chg-2026-01-01-t]\n"+
			"follow-ons: []\n---\nDone arc.\n")
	gitT(t, root, "init", "-q")
	gitT(t, root, "config", "user.email", "t@t")
	gitT(t, root, "config", "user.name", "t")
	gitT(t, root, "add", "-A")
	gitT(t, root, "commit", "-qm", "fixture")
	mutate(t, root, "vault/record/arcs/arc-u.md",
		"follow-ons: []", "follow-ons: [chg-2026-01-01-t]")
	gitT(t, root, "add", "-A")
	fails, _ := runLint(root, true)
	wantFailContaining(t, fails, "non-closure Record fields changed: ['follow-ons']")
}

func TestAuditHardDossierDiffWarnsStaged(t *testing.T) {
	root := fixture(t)
	mutate(t, root, "vault/system/t/sub-t-x.md", "audit: light", "audit: hard")
	gitT(t, root, "init", "-q")
	gitT(t, root, "config", "user.email", "t@t")
	gitT(t, root, "config", "user.name", "t")
	gitT(t, root, "add", "-A")
	gitT(t, root, "commit", "-qm", "fixture")
	writeNote(t, root, "vault/record/changes/chg-2026-01-04-hard.md",
		"---\nid: chg-2026-01-04-hard\ntype: chg\ndate: 2026-01-04\narc: arc-t\n"+
			"commits: [\"dddd4444\"]\ntouched: [sub-t-x]\ndepth: skeletal\n---\nBody.\n")
	gitT(t, root, "add", "-A")
	_, warns := runLint(root, true)
	found := false
	for _, w := range warns {
		if strings.Contains(w, "touches audit:hard [[sub-t-x]]") {
			found = true
		}
	}
	if !found {
		t.Fatalf("want the dossier-diff warn, got %v", warns)
	}
}

// --- subcommand internals ---

func TestNewNoteAndIDCollision(t *testing.T) {
	root := fixture(t)
	writeNote(t, root, "vault/meta/templates/seam.md",
		"---\nid: seam-{slug}\ntype: seam\ntitle: \"{t}\"\nstatus: open\n"+
			"surface: []\nopened-by: {chg}\ncreated: {YYYY-MM-DD}\n---\nBody.\n")
	rel, err := newNote(root, "seam", "seam-t-2", "A new seam", "",
		map[string]string{"opened-by": "chg-2026-01-01-t"})
	if err != nil {
		t.Fatal(err)
	}
	if rel != "vault/seams/seam-t-2.md" {
		t.Fatalf("path: %s", rel)
	}
	b, _ := os.ReadFile(filepath.Join(root, rel))
	for _, want := range []string{"id: seam-t-2", "title: \"A new seam\"",
		"opened-by: chg-2026-01-01-t"} {
		if !strings.Contains(string(b), want) {
			t.Fatalf("created note missing %q:\n%s", want, b)
		}
	}
	if strings.Contains(string(b), "{YYYY-MM-DD}") {
		t.Fatal("date placeholder not filled")
	}
	if _, err := newNote(root, "seam", "seam-t-1", "", "", nil); err == nil {
		t.Fatal("collision accepted")
	}
	if _, err := newNote(root, "seam", "fnd-wrong-prefix", "", "", nil); err == nil {
		t.Fatal("wrong prefix accepted")
	}
}

func TestCloseNoteFlipsClosureOnly(t *testing.T) {
	root := fixture(t)
	if _, err := closeNote(root, "fnd-t-r1-f1",
		map[string]string{"status": "documented"}); err != nil {
		t.Fatal(err)
	}
	reg, _ := loadRegistry(root)
	n, _ := reg.Get("fnd-t-r1-f1")
	if n.Front.Str("status") != "documented" {
		t.Fatal("status not flipped")
	}
	if _, err := closeNote(root, "fnd-t-r1-f1",
		map[string]string{"severity": "P3"}); err == nil {
		t.Fatal("non-closure field accepted")
	}
	if _, err := closeNote(root, "fnd-t-r1-f1",
		map[string]string{"status": "bogus"}); err == nil {
		t.Fatal("enum violation accepted")
	}
	if _, err := closeNote(root, "fnd-t-r1-f1",
		map[string]string{"fixed-by": "chg-2099-01-01-nope"}); err == nil {
		t.Fatal("dangling closure edge accepted")
	}
	if _, err := closeNote(root, "sub-t-x",
		map[string]string{"status": "fixed"}); err == nil {
		t.Fatal("closure flip on a non-Record type accepted")
	}
}

func TestBacklinks(t *testing.T) {
	root := fixture(t)
	reg, _ := loadRegistry(root)
	links := backlinks(reg, "chg-2026-01-01-t")
	got := map[string]string{}
	for _, l := range links {
		got[l.From] = l.Via
	}
	for from, via := range map[string]string{
		"arc-t": "chunks", "fnd-t-r1-f1": "fixed-by", "seam-t-1": "opened-by"} {
		if got[from] != via {
			t.Fatalf("missing backlink %s via %s; got %v", from, via, links)
		}
	}
}

func TestMCPToolLayer(t *testing.T) {
	root := fixture(t)
	out, isErr := callTool(root, "vault_lint", nil)
	if isErr || !strings.Contains(out, "9 notes, 0 fail(s)") {
		t.Fatalf("vault_lint: isErr=%v out=%q", isErr, out)
	}
	out, isErr = callTool(root, "vault_note", map[string]any{"id": "sub-t-x"})
	if isErr || !strings.Contains(out, "id: sub-t-x") {
		t.Fatalf("vault_note: isErr=%v", isErr)
	}
	out, isErr = callTool(root, "vault_closed_preamble",
		map[string]any{"sub_id": "sub-t-x"})
	if isErr || !strings.Contains(out, "fnd-t-r1-f1") {
		t.Fatalf("vault_closed_preamble: isErr=%v out=%q", isErr, out)
	}
	if _, isErr = callTool(root, "no_such_tool", nil); !isErr {
		t.Fatal("unknown tool did not error")
	}
	out, isErr = callTool(root, "vault_query_findings",
		map[string]any{"status": "fixed"})
	if isErr || !strings.Contains(out, "fnd-t-r1-f1") ||
		strings.Contains(out, "fnd-t-r1-f2") {
		t.Fatalf("vault_query_findings: %q", out)
	}
}
