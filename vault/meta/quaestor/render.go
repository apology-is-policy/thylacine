package main

// View renderers. Output is byte-identical to the reference
// implementation's (the committed generated views are the oracle: a
// renderer drift shows up as a spurious "stale generated body" fail).

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

const genBegin = "<!-- generated:begin -->"
const genEnd = "<!-- generated:end -->"

var prosecutionRe = regexp.MustCompile(`(?m)^##\s+Prosecution\s*$\n+(.+?)$`)
var dispositionRe = regexp.MustCompile(`(?m)^##\s+Disposition\s*$\n+(.+?)$`)

// line renders a front value for a table cell: lists join with ", ".
func line(n *Note, key string) string {
	v, ok := n.Front.Get(key)
	if !ok {
		return ""
	}
	if v.IsList {
		return strings.Join(v.List, ", ")
	}
	return v.Str
}

func sortByID(notes []*Note) []*Note {
	out := make([]*Note, len(notes))
	copy(out, notes)
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func renderDashboard(reg *Registry) string {
	arcs := reg.OfType("arc")
	var seams []*Note
	for _, n := range reg.OfType("seam") {
		if n.Front.Str("status") == "open" {
			seams = append(seams, n)
		}
	}
	chgs := reg.OfType("chg")
	sort.SliceStable(chgs, func(i, j int) bool {
		return chgs[i].Front.Str("date") > chgs[j].Front.Str("date")
	})
	if len(chgs) > 8 {
		chgs = chgs[:8]
	}
	out := []string{"## Arcs", "", "| arc | status | chunks |", "|---|---|---|"}
	for _, a := range sortByID(arcs) {
		out = append(out, fmt.Sprintf("| [[%s]] | %s | %d |",
			a.ID, a.Front.Str("status"), len(a.Front.ListOr("chunks"))))
	}
	if len(arcs) == 0 {
		out = append(out, "| (none yet) | | |")
	}
	out = append(out, "", fmt.Sprintf("## Open seams: %d", len(seams)), "")
	if len(seams) == 0 {
		out = append(out, "- (none)")
	} else {
		for _, s := range sortByID(seams) {
			out = append(out, fmt.Sprintf("- [[%s]] (%s)", s.ID, line(s, "surface")))
		}
	}
	out = append(out, "", "## Recent changes", "")
	if len(chgs) == 0 {
		out = append(out, "- (none yet)")
	} else {
		for _, c := range chgs {
			out = append(out, fmt.Sprintf("- %s [[%s]] — %s",
				c.Front.Str("date"), c.ID, c.Front.Str("title")))
		}
	}
	return strings.Join(out, "\n")
}

func renderInvariants(reg *Registry) string {
	invs := sortByID(reg.OfType("inv"))
	out := []string{"| # | invariant | strength | guards | validated by |",
		"|---|---|---|---|---|"}
	for _, i := range invs {
		out = append(out, fmt.Sprintf("| %s | [[%s]] | %s | %s | %s |",
			i.Front.Str("number"), i.ID, i.Front.Str("strength"),
			line(i, "guards"), line(i, "validated-by")))
	}
	if len(invs) == 0 {
		out = append(out, "| (none yet) | | | | |")
	}
	return strings.Join(out, "\n")
}

func renderSeams(reg *Registry) string {
	seams := reg.OfType("seam")
	sort.Slice(seams, func(i, j int) bool {
		si, sj := seams[i].Front.Str("status"), seams[j].Front.Str("status")
		if si != sj {
			return si < sj
		}
		return seams[i].ID < seams[j].ID
	})
	out := []string{"| seam | status | surface | opened by | tracker |",
		"|---|---|---|---|---|"}
	for _, s := range seams {
		out = append(out, fmt.Sprintf("| [[%s]] | %s | %s | %s | %s |",
			s.ID, s.Front.Str("status"), line(s, "surface"),
			line(s, "opened-by"), s.Front.Str("tracker")))
	}
	if len(seams) == 0 {
		out = append(out, "| (none yet) | | | | |")
	}
	return strings.Join(out, "\n")
}

func renderAuditTriggers(reg *Registry) string {
	var subs []*Note
	for _, n := range reg.OfType("sub") {
		if n.Front.Str("audit") == "hard" {
			subs = append(subs, n)
		}
	}
	out := []string{"| surface | code | invariants | prosecution |",
		"|---|---|---|---|"}
	for _, s := range sortByID(subs) {
		pros := ""
		if m := prosecutionRe.FindStringSubmatch(s.Body); m != nil {
			r := []rune(strings.TrimSpace(m[1]))
			if len(r) > 160 {
				r = r[:160]
			}
			pros = string(r)
		}
		out = append(out, fmt.Sprintf("| [[%s]] | %s | %s | %s |",
			s.ID, line(s, "code"), line(s, "guarded-by"), pros))
	}
	if len(subs) == 0 {
		out = append(out, "| (none yet) | | | |")
	}
	return strings.Join(out, "\n")
}

func renderRoadmap(reg *Registry) string {
	arcs := reg.OfType("arc")
	sort.Slice(arcs, func(i, j int) bool {
		si, sj := arcs[i].Front.Str("status"), arcs[j].Front.Str("status")
		if si != sj {
			return si < sj
		}
		return arcs[i].ID < arcs[j].ID
	})
	out := []string{"| arc | status | chunks landed | follow-ons |",
		"|---|---|---|---|"}
	for _, a := range arcs {
		out = append(out, fmt.Sprintf("| [[%s]] | %s | %d | %s |",
			a.ID, a.Front.Str("status"), len(a.Front.ListOr("chunks")),
			line(a, "follow-ons")))
	}
	if len(arcs) == 0 {
		out = append(out, "| (none yet) | | | |")
	}
	return strings.Join(out, "\n")
}

// renderClosed: the do-not-re-report preamble for a surface (replaces the
// memory/audit_*_closed_list.md files; transcluded into prosecutor
// prompts). Membership preserves the reference implementation's
// semantics: a LIST surface is exact membership; a SCALAR surface is a
// substring test (Python's `x in str`).
func renderClosed(reg *Registry, subID string) string {
	var fnds []*Note
	for _, n := range reg.OfType("fnd") {
		st := n.Front.Str("status")
		if st != "fixed" && st != "documented" && st != "withdrawn" {
			continue
		}
		v, ok := n.Front.Get("surface")
		if !ok {
			continue
		}
		match := false
		if v.IsList {
			for _, s := range v.List {
				if s == subID {
					match = true
					break
				}
			}
		} else {
			match = strings.Contains(v.Str, subID)
		}
		if match {
			fnds = append(fnds, n)
		}
	}
	out := []string{
		fmt.Sprintf("%d closed findings on [[%s]] — do NOT re-report", len(fnds), subID),
		"these in a future round (open/deferred findings are NOT listed",
		"here; see the seam inbox):", ""}
	for _, n := range sortByID(fnds) {
		disp := ""
		if m := dispositionRe.FindStringSubmatch(n.Body); m != nil {
			disp = " — " + strings.TrimSpace(m[1])
		}
		out = append(out, fmt.Sprintf("- [[%s]] [%s] %s (%s)%s",
			n.ID, n.Front.Str("severity"), n.Front.Str("title"),
			n.Front.Str("status"), disp))
	}
	return strings.Join(out, "\n")
}

const absorbedMarker = "ABSORBED INTO THE VAULT"

var vaultPathRe = regexp.MustCompile(`vault/(?:[A-Za-z0-9_.-]+/)*([A-Za-z0-9_.-]+)\.md`)

// srcRe: what counts as a source file for the coverage census. Deliberately
// the implementation languages only — a .py or .sh under tools/ is substrate
// and is swept as harness prose, not as an owned translation unit.
var srcRe = regexp.MustCompile(`^(?:kernel|arch|mm|usr)/.*\.(?:c|h|S|rs)$`)

// vaultRoot recovers the repo root from any note: a Note carries both an
// absolute Path and the repo-relative Rel, so the root is the difference.
// Renderers take only a Registry, and this is the one that needs to look
// outside vault/.
func vaultRoot(reg *Registry) string {
	for _, n := range reg.Notes() {
		p := filepath.ToSlash(n.Path)
		if strings.HasSuffix(p, n.Rel) {
			return strings.TrimSuffix(p, n.Rel)
		}
	}
	return ""
}

// renderAbsorption: the absorption ledger — the one view whose subject sits
// OUTSIDE the vault. It reads docs/reference/ and reports, per document,
// whether the vault has absorbed it and into which notes.
//
// Computed rather than hand-kept, because a hand-kept ledger is exactly what
// rotted: the sweep ran ahead of the absorption for twenty batches and the
// drift was found only by accident. Since a stub lives in docs/reference/,
// editing one without re-rendering now fails the linter, which is the whole
// point — the count cannot silently disagree with the tree again.
//
// What it does NOT check: whether a dossier actually covers everything its
// stub claims. A reference document routinely spans more code than its title
// names, and the pre-stub text is only in git history, which a renderer
// cannot see. That check stays manual.
func renderAbsorption(reg *Registry) string {
	root := vaultRoot(reg)
	if root == "" {
		return "**(registry empty — cannot locate the repo root.)**"
	}
	ents, err := os.ReadDir(filepath.Join(root, "docs", "reference"))
	if err != nil {
		return "**(docs/reference is unreadable.)**"
	}
	type row struct{ doc, state, into string }
	var rows []row
	var absorbed, live int
	for _, e := range ents {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		b, rerr := os.ReadFile(filepath.Join(root, "docs", "reference", e.Name()))
		if rerr != nil {
			continue
		}
		txt := string(b)
		if !strings.Contains(txt, absorbedMarker) {
			live++
			rows = append(rows, row{e.Name(), "live", "—"})
			continue
		}
		absorbed++
		var into []string
		seen := map[string]bool{}
		for _, m := range vaultPathRe.FindAllStringSubmatch(txt, -1) {
			// vault/meta/ is machinery, deliberately outside the registry
			// (schema.md, workflow.md, quaestor) — a stub citing it is
			// citing prose, not a note.
			if strings.HasPrefix(m[0], "vault/meta/") {
				continue
			}
			if id := m[1]; !seen[id] {
				seen[id] = true
				if reg.Has(id) {
					into = append(into, "[["+id+"]]")
				} else {
					into = append(into, "**dangling: "+id+"**")
				}
			}
		}
		sort.Strings(into)
		state := "absorbed"
		if len(into) == 0 {
			state = "**absorbed, names no note**"
		}
		rows = append(rows, row{e.Name(), state, strings.Join(into, ", ")})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].doc < rows[j].doc })
	out := []string{
		fmt.Sprintf("**%d absorbed · %d live · %d total.**",
			absorbed, live, absorbed+live),
		"",
		"| document | state | absorbed into |",
		"|---|---|---|",
	}
	for _, r := range rows {
		out = append(out, fmt.Sprintf("| %s | %s | %s |", r.doc, r.state, r.into))
	}
	if len(rows) == 0 {
		out = append(out, "| (none) | | |")
	}
	return strings.Join(out, "\n")
}

// specNoteID maps a TLA+ module filename to the note id that dossiers it:
// specs/sched_oncpu.tla -> spec-sched-oncpu. The convention is the whole
// mapping; there is no registry of exceptions and there should not be one.
func specNoteID(file string) string {
	base := strings.TrimSuffix(file, ".tla")
	return "spec-" + strings.ReplaceAll(base, "_", "-")
}

// renderSpecCoverage: which TLA+ modules have a spec note, and which do not.
//
// The second view reading outside the vault, and for the same reason as
// renderAbsorption. A spec note is what holds a module's action-to-site map
// (schema section 4: it "absorbs SPEC-TO-CODE.md for this module"), so a
// reference document that carries such a map cannot be absorbed until its
// module has one. The gap was found in prose at the registry pass; stating
// it in prose is what let the absorption ledger rot, so it is computed here
// instead and cannot fall behind the tree.
func renderSpecCoverage(reg *Registry) string {
	root := vaultRoot(reg)
	if root == "" {
		return "**(registry empty — cannot locate the repo root.)**"
	}
	ents, err := os.ReadDir(filepath.Join(root, "specs"))
	if err != nil {
		return "**(specs/ is unreadable.)**"
	}
	type row struct{ module, note, state string }
	var rows []row
	var have, missing int
	for _, e := range ents {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".tla") {
			continue
		}
		id := specNoteID(e.Name())
		if reg.Has(id) {
			have++
			rows = append(rows, row{e.Name(), "[[" + id + "]]", "dossiered"})
		} else {
			missing++
			rows = append(rows, row{e.Name(), "`" + id + "`", "**missing**"})
		}
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].module < rows[j].module })
	out := []string{
		fmt.Sprintf("**%d dossiered · %d missing · %d modules.**",
			have, missing, have+missing),
		"",
		"| module | spec note | state |",
		"|---|---|---|",
	}
	for _, r := range rows {
		out = append(out, fmt.Sprintf("| %s | %s | %s |", r.module, r.note, r.state))
	}
	if len(rows) == 0 {
		out = append(out, "| (none) | | |")
	}
	return strings.Join(out, "\n")
}

// isHarness: a program whose purpose is to exercise the system rather than to
// be part of it. Swept as harness prose (see the substrate dossiers), for the
// same reason tools/ is — so it is counted and named here, never silently
// dropped, because a census that quietly narrows its own denominator is the
// failure this view exists to prevent.
func isHarness(p string) bool {
	if !strings.HasPrefix(p, "usr/") {
		return false
	}
	name := p[4:]
	if i := strings.Index(name, "/"); i >= 0 {
		name = name[:i]
	}
	for _, suf := range []string{"-probe", "-smoke", "-test", "-bench", "-torture"} {
		if strings.HasSuffix(name, suf) {
			return true
		}
	}
	switch name {
	case "hello", "hello-rs", "u-test", "cpubench", "netperf", "fsbench",
		"tlsperf", "nettest", "pipe-src", "pipe-sink", "debug-child",
		"stack-child", "parley-echo", "hwbp-verify", "jit-prover",
		"coreutil-smoke", "tapestry-battery", "tapestry-demo", "legate-prover":
		return true
	}
	return false
}

// sweepArea buckets a source path into the area whose sweep would own it.
// Coarse on purpose: the point is to say where the remaining work is, not to
// mirror the MOC tree, which is the thing this view exists to distrust.
func sweepArea(p string) string {
	switch {
	case strings.HasPrefix(p, "arch/"):
		return "arch"
	case strings.HasPrefix(p, "mm/"):
		return "mm"
	case strings.HasPrefix(p, "usr/lib/libthyla-rs/"):
		return "usr/libthyla-rs"
	case strings.HasPrefix(p, "usr/lib/"):
		return "usr/lib"
	case strings.HasPrefix(p, "usr/"):
		if i := strings.Index(p[4:], "/"); i >= 0 {
			return "usr/" + p[4:4+i]
		}
		return "usr"
	}
	return "kernel"
}

// renderCodeCoverage: which source files an existing dossier claims, and which
// none does.
//
// The third view reading outside the vault, and it exists because the sweep
// ledger repeated the absorption ledger's failure one level down. Completeness
// was declared by walking the area MOCs — every area has children, therefore
// the sweep is done — when the question was whether every FILE has an owner.
// It did not, by 26 files and ~9.5k lines, including the page tables, the fault
// dispatcher, note delivery and exec. The claim's subject was narrower than the
// claim, which is the defect this arc keeps finding in other people's work.
//
// Ownership is `code:` on a `sub` note and nothing else. Prose mention does not
// count: a dossier that discusses a neighbour's file has not swept it, and
// treating a mention as coverage is how the first ledger rotted.
//
// What it does NOT check: whether a dossier that claims a file covers it WELL.
// Whether it STILL covers it after the file changes is a separate property, and
// is now computed -- `quaestor stale` compares each dossier's `updated:` against
// the arrival date of every file it claims.
func renderCodeCoverage(reg *Registry) string {
	root := vaultRoot(reg)
	if root == "" {
		return "**(registry empty — cannot locate the repo root.)**"
	}
	owner := map[string][]string{}
	for _, n := range reg.OfType("sub") {
		for _, c := range n.Front.ListOr("code") {
			c = strings.TrimSpace(c)
			if c != "" {
				owner[c] = append(owner[c], n.ID)
			}
		}
	}
	cmd := exec.Command("git", "-C", root, "ls-files")
	b, err := cmd.Output()
	if err != nil {
		return "**(git ls-files failed — cannot census the tree.)**"
	}
	lines := func(p string) int {
		fb, ferr := os.ReadFile(filepath.Join(root, p))
		if ferr != nil {
			return 0
		}
		return len(strings.Split(strings.TrimRight(string(fb), "\n"), "\n"))
	}
	type area struct{ owned, unowned, unsweptLines int }
	areas := map[string]*area{}
	type row struct {
		path  string
		lines int
	}
	var un []row
	var harnessFiles, harnessLines int
	for _, p := range strings.Fields(string(b)) {
		if !srcRe.MatchString(p) || strings.Contains(p, "/test") ||
			strings.HasPrefix(p, "usr/apps/") || strings.Contains(p, "/vendor/") {
			continue
		}
		if isHarness(p) {
			harnessFiles++
			harnessLines += lines(p)
			continue
		}
		a := sweepArea(p)
		if areas[a] == nil {
			areas[a] = &area{}
		}
		if len(owner[p]) > 0 {
			areas[a].owned++
			continue
		}
		areas[a].unowned++
		nl := lines(p)
		areas[a].unsweptLines += nl
		un = append(un, row{p, nl})
	}
	var names []string
	var to, tu, tl int
	for k, v := range areas {
		names = append(names, k)
		to += v.owned
		tu += v.unowned
		tl += v.unsweptLines
	}
	sort.Slice(names, func(i, j int) bool {
		if areas[names[i]].unsweptLines != areas[names[j]].unsweptLines {
			return areas[names[i]].unsweptLines > areas[names[j]].unsweptLines
		}
		return names[i] < names[j]
	})
	sort.Slice(un, func(i, j int) bool {
		if un[i].lines != un[j].lines {
			return un[i].lines > un[j].lines
		}
		return un[i].path < un[j].path
	})

	pct := 0
	if to+tu > 0 {
		pct = to * 100 / (to + tu)
	}
	out := []string{
		fmt.Sprintf("**%d owned · %d unowned · %d files (%d%% owned) · ~%d unswept lines.**",
			to, tu, to+tu, pct, tl),
		"",
		fmt.Sprintf("Excluded as harness and counted here rather than dropped: **%d files, ~%d lines** "+
			"(probes, smokes, benches, torture and the `u-test` family — programs whose purpose is to "+
			"exercise the system, swept as harness prose like `tools/`).", harnessFiles, harnessLines),
		"",
		"| area | owned | unowned | unswept lines |",
		"|---|---:|---:|---:|",
	}
	for _, k := range names {
		v := areas[k]
		out = append(out, fmt.Sprintf("| %s | %d | %d | %d |", k, v.owned, v.unowned, v.unsweptLines))
	}
	out = append(out, "", "### Unowned, largest first", "",
		"| file | lines |", "|---|---:|")
	for _, r := range un {
		out = append(out, fmt.Sprintf("| %s | %d |", r.path, r.lines))
	}
	if len(un) == 0 {
		out = append(out, "| (none) | |")
	}
	return strings.Join(out, "\n")
}

// declPathRe pulls path-shaped tokens out of an audit-trigger row. The table
// writes them inside backticks, with brace sets for sibling extensions
// (kernel/dma_handle.{c,h}) and bare directories for whole-program surfaces
// (usr/tapestryd/).
var declPathRe = regexp.MustCompile(`(?:kernel|arch|mm|usr|lib|tools|specs|init)/[A-Za-z0-9_./{},-]*`)

// declResolves: does a cited path name a real file? Plain existence is not
// enough, because the table writes `kernel/foo.{c,h}` under a project
// convention where the .c sits in kernel/ and the .h in
// kernel/include/thylacine/. Expanding that brace literally invents a header
// path that has never existed, so a resolver that only stats the literal
// string reports ~20 phantom headers and drowns the two real ones.
func declResolves(root, p string) bool {
	if _, err := os.Stat(filepath.Join(root, p)); err == nil {
		return true
	}
	if strings.HasSuffix(p, ".h") {
		alt := filepath.Join(root, "kernel", "include", "thylacine", filepath.Base(p))
		if _, err := os.Stat(alt); err == nil {
			return true
		}
	}
	return false
}

// negClaimRe: the row may be asserting a path's ABSENCE. Matched against the
// text immediately preceding a token so a documented negative is flagged for
// a human read rather than reported as drift.
var negClaimRe = regexp.MustCompile(`(?i)\b(no|not|never|without|removed|deleted|retired|absent)\b[^.]{0,24}$`)

// expandDecl normalizes one extracted token into the concrete paths it names.
// Brace sets expand; trailing punctuation swept up by the character class is
// dropped. Returns nothing for a token that degenerates to a bare directory
// root, which would otherwise match half the tree.
func expandDecl(tok string) []string {
	tok = strings.TrimRight(tok, ".,-/")
	if tok == "" || !strings.Contains(tok, "/") {
		return nil
	}
	i, j := strings.Index(tok, "{"), strings.Index(tok, "}")
	if i < 0 || j < i {
		return []string{tok}
	}
	stem, exts, tail := tok[:i], tok[i+1:j], tok[j+1:]
	var out []string
	for _, e := range strings.Split(exts, ",") {
		if e = strings.TrimSpace(e); e != "" {
			out = append(out, stem+e+tail)
		}
	}
	return out
}

// renderAuditTriggerCoverage: does every surface main DECLARES audit-bearing
// have a hard-audit dossier?
//
// The fourth view reading outside the vault, and the one that closes a gap the
// other three structurally could not see. renderAuditTriggers enumerates what
// the VAULT has written (`audit: hard` dossiers); docs/AUDIT-TRIGGERS.md
// enumerates what MAIN has declared. Those are different populations, nothing
// compared them, and the difference is silent in both directions: main's edits
// to its table merge cleanly (the vault has never stubbed it), and the view
// re-renders from note fields looking complete. Found 2026-08-14 when a
// 124-commit merge added the whole Warp/I-45 GPU seam and the view did not move.
//
// Matching is on FILES, never on row counts, because the mapping is
// many-to-many: one dossier routinely covers several declared surfaces and a
// declared surface routinely spans several dossiers. Counting rows would report
// a difference that means nothing.
//
// Three outcomes, deliberately distinguished because they have different
// remedies: a surface whose files no dossier names at all needs a SWEEP; one
// whose files are owned only by soft-audit dossiers needs an `audit:` lift; one
// whose paths could not be parsed is a LEDGER defect and is reported as such —
// never folded into "covered", because a check that cannot read its input must
// not answer yes (the skip-reported-as-pass lesson).
func renderAuditTriggerCoverage(reg *Registry) string {
	root := vaultRoot(reg)
	if root == "" {
		return "**(registry empty — cannot locate the repo root.)**"
	}
	b, err := os.ReadFile(filepath.Join(root, "docs", "AUDIT-TRIGGERS.md"))
	if err != nil {
		return "**(docs/AUDIT-TRIGGERS.md is unreadable.)**"
	}
	hard := map[string][]string{}
	soft := map[string][]string{}
	for _, n := range reg.OfType("sub") {
		dst := soft
		if n.Front.Str("audit") == "hard" {
			dst = hard
		}
		for _, c := range n.Front.ListOr("code") {
			if c = strings.TrimSpace(c); c != "" {
				dst[c] = append(dst[c], n.ID)
			}
		}
	}
	// A declared token matches a claimed file exactly, or — when it names a
	// directory or an extension-less program — by path prefix, because the
	// table writes `usr/tapestryd/` where the dossier claims its .rs files.
	owners := func(m map[string][]string, p string) []string {
		if o, ok := m[p]; ok {
			return o
		}
		var out []string
		for k, o := range m {
			if strings.HasPrefix(k, p+"/") {
				out = append(out, o...)
			}
		}
		return out
	}
	type row struct{ surface, state, detail string }
	var gaps []row
	var covered, softOnly, unowned, unparsed int
	// The second arm (main's suggestion, 2026-08-14): a cited path that does
	// not resolve. Orthogonal to ownership -- a row can be fully covered AND
	// cite a phantom -- so it is counted and reported separately rather than
	// as a fifth state. A wrong path is strictly worse than an empty column:
	// an empty column announces that it tells you nothing, while a wrong path
	// reads as authoritative and sends the reader somewhere that never was.
	type ghost struct{ surface, path, note string }
	var ghosts []ghost
	for _, ln := range strings.Split(string(b), "\n") {
		if !strings.HasPrefix(ln, "| ") || strings.HasPrefix(ln, "|---") ||
			strings.HasPrefix(ln, "| Surface ") {
			continue
		}
		// Split tolerantly: prose in the later columns contains bare `|`.
		f := strings.Split(strings.Trim(ln, "|"), "|")
		if len(f) < 2 {
			continue
		}
		surface := strings.TrimSpace(f[0])
		var paths []string
		seen := map[string]bool{}
		for _, loc := range declPathRe.FindAllStringIndex(ln, -1) {
			for _, p := range expandDecl(ln[loc[0]:loc[1]]) {
				if seen[p] {
					continue
				}
				seen[p] = true
				paths = append(paths, p)
				if declResolves(root, p) {
					continue
				}
				// A leading '/' makes it a GUEST filesystem path, not a source
				// path (`/lib/ndb/local` is the Plan 9 network database, not a
				// file in this repo). The regex starts matching at the segment
				// name, so the slash is only visible by looking back one char.
				if loc[0] > 0 && ln[loc[0]-1] == '/' {
					continue
				}
				// A path may legitimately be absent because the row SAYS it is
				// absent ("there is no `mm/vm.c`"). Those are reported, never
				// suppressed -- a silent skip is how a sweep launders an error
				// -- but they are flagged so the reader knows to read the
				// CLAIM rather than the token.
				note := "no such file in the tree"
				lo := loc[0] - 48
				if lo < 0 {
					lo = 0
				}
				if negClaimRe.MatchString(ln[lo:loc[0]]) {
					note = "**possibly a documented NEGATIVE — read the claim**"
				}
				ghosts = append(ghosts, ghost{surface, p, note})
			}
		}
		if len(paths) == 0 {
			unparsed++
			gaps = append(gaps, row{surface, "**unparsed**",
				"no path token extracted — the ledger cannot judge this row"})
			continue
		}
		var hardHits, softHits []string
		for _, p := range paths {
			hardHits = append(hardHits, owners(hard, p)...)
			softHits = append(softHits, owners(soft, p)...)
		}
		switch {
		case len(hardHits) > 0:
			covered++
		case len(softHits) > 0:
			softOnly++
			gaps = append(gaps, row{surface, "soft-owned",
				"owned by " + uniqJoin(softHits) + ", none `audit: hard`"})
		default:
			unowned++
			gaps = append(gaps, row{surface, "**unowned**",
				"no dossier names " + firstN(paths, 3)})
		}
	}
	total := covered + softOnly + unowned + unparsed
	out := []string{
		fmt.Sprintf("**%d declared surfaces · %d covered by a hard-audit dossier · "+
			"%d soft-owned · %d unowned · %d unparsed · %d cited path(s) that do "+
			"not resolve.**",
			total, covered, softOnly, unowned, unparsed, len(ghosts)),
		"",
	}
	if len(gaps) == 0 && len(ghosts) == 0 {
		out = append(out, "Every declared surface has a hard-audit dossier, and every cited path resolves.")
		return strings.Join(out, "\n")
	}
	if len(ghosts) > 0 {
		out = append(out, "### Cited paths that do not resolve", "",
			"| declared surface | cited path | |", "|---|---|---|")
		sort.Slice(ghosts, func(i, j int) bool {
			if ghosts[i].surface != ghosts[j].surface {
				return ghosts[i].surface < ghosts[j].surface
			}
			return ghosts[i].path < ghosts[j].path
		})
		for _, g := range ghosts {
			out = append(out, fmt.Sprintf("| %s | `%s` | %s |",
				truncRunes(g.surface, 70), g.path, g.note))
		}
		out = append(out, "")
	}
	if len(gaps) == 0 {
		return strings.Join(out, "\n")
	}
	out = append(out, "### Ownership gaps", "",
		"| declared surface | state | why |", "|---|---|---|")
	sort.Slice(gaps, func(i, j int) bool { return gaps[i].surface < gaps[j].surface })
	for _, g := range gaps {
		// Only the surface title is character-truncated: it comes from main's
		// table and carries no wikilinks. The detail column is bounded by
		// construction (a capped id list, a capped path list, or a fixed
		// string) precisely so it is never cut through a link.
		out = append(out, fmt.Sprintf("| %s | %s | %s |",
			truncRunes(g.surface, 90), g.state, g.detail))
	}
	return strings.Join(out, "\n")
}

// uniqJoin caps the LIST rather than the string. Truncating a cell mid-way
// through a `[[wikilink]]` manufactures a dangling link and fails the linter —
// found the first time this view rendered.
func uniqJoin(ids []string) string {
	seen := map[string]bool{}
	var out []string
	for _, i := range ids {
		if !seen[i] {
			seen[i] = true
			out = append(out, "[["+i+"]]")
		}
	}
	sort.Strings(out)
	if len(out) > 4 {
		return strings.Join(out[:4], ", ") +
			fmt.Sprintf(" (+%d more)", len(out)-4)
	}
	return strings.Join(out, ", ")
}

func firstN(ps []string, n int) string {
	if len(ps) > n {
		return "`" + strings.Join(ps[:n], "`, `") + "` (+" +
			fmt.Sprint(len(ps)-n) + " more)"
	}
	return "`" + strings.Join(ps, "`, `") + "`"
}

func truncRunes(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n]) + "…"
	}
	return s
}

var renderers = map[string]func(*Registry) string{
	"dashboard":              renderDashboard,
	"invariants":             renderInvariants,
	"seams":                  renderSeams,
	"audit-triggers":         renderAuditTriggers,
	"audit-trigger-coverage": renderAuditTriggerCoverage,
	"invariant-registry":     renderInvariantRegistry,
	"roadmap":                renderRoadmap,
	"absorption":             renderAbsorption,
	"spec-coverage":          renderSpecCoverage,
	"code-coverage":          renderCodeCoverage,
}

func viewNotes(reg *Registry) []*Note { return reg.OfType("view") }

// renderedBody returns the view's full new raw text, or an error message.
func renderedBody(note *Note, reg *Registry) (string, string) {
	q := note.Front.Str("query")
	var body string
	if r, ok := renderers[q]; ok {
		body = r(reg)
	} else if strings.HasPrefix(q, "closed:") {
		body = renderClosed(reg, strings.SplitN(q, ":", 2)[1])
	} else {
		return "", fmt.Sprintf("%s: no renderer for query '%s'", note.Rel, q)
	}
	if !strings.Contains(note.Raw, genBegin) || !strings.Contains(note.Raw, genEnd) {
		return "", fmt.Sprintf("%s: missing %s/%s markers", note.Rel, genBegin, genEnd)
	}
	pre, rest, _ := strings.Cut(note.Raw, genBegin)
	_, post, _ := strings.Cut(rest, genEnd)
	return pre + genBegin + "\n" + body + "\n" + genEnd + post, ""
}

// danglingRe matches the marker renderAbsorption emits for a stub that
// names a note which does not exist. Rendered as plain text rather than a
// wikilink deliberately: a wikilink would fail the dangling-link check with
// a message pointing at the generated view instead of at the stub that is
// actually wrong.
var danglingRe = regexp.MustCompile(`\*\*dangling: ([A-Za-z0-9_.-]+)\*\*`)

func checkViews(reg *Registry) []string {
	var fails []string
	for _, v := range viewNotes(reg) {
		nu, errMsg := renderedBody(v, reg)
		if errMsg != "" {
			fails = append(fails, errMsg)
			continue
		}
		if nu != v.Raw {
			fails = append(fails, v.Rel+
				": stale generated body (run quaestor render)")
		}
		// A rendered dangling marker is a real broken reference in a
		// source document, not a staleness artifact -- it survives a
		// re-render, so without this it would be reported and never
		// block. Reported against the view because that is where it is
		// visible; the fix belongs in the stub the marker names.
		seen := map[string]bool{}
		for _, m := range danglingRe.FindAllStringSubmatch(nu, -1) {
			if !seen[m[1]] {
				seen[m[1]] = true
				fails = append(fails, v.Rel+
					": a stub names a note that does not exist: '"+m[1]+"'")
			}
		}
	}
	return fails
}

func renderViews(reg *Registry) []string {
	var changed []string
	for _, v := range viewNotes(reg) {
		nu, errMsg := renderedBody(v, reg)
		if errMsg != "" {
			fmt.Println("render: " + errMsg)
			continue
		}
		if nu != v.Raw {
			if err := os.WriteFile(v.Path, []byte(nu), 0o644); err != nil {
				fmt.Println("render: " + v.Rel + ": " + err.Error())
				continue
			}
			changed = append(changed, v.Rel)
		}
	}
	return changed
}

// invRe matches an I-NN citation anywhere in prose or a table cell.
var invRe = regexp.MustCompile(`\bI-([0-9]+)\b`)

// invRowRe matches a section-28-style table ROW (the leading cell is the id),
// which is what makes something a REGISTRY entry rather than a mention.
var invRowRe = regexp.MustCompile(`(?m)^\| ?(I-[0-9]+) `)

func invSet(re *regexp.Regexp, body string, group int) map[string]bool {
	out := map[string]bool{}
	for _, m := range re.FindAllStringSubmatch(body, -1) {
		id := m[group]
		if group == 1 && !strings.HasPrefix(id, "I-") {
			id = "I-" + id
		}
		out[id] = true
	}
	return out
}

func sortedInvs(m map[string]bool) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool { return atoiOr0(out[i][2:]) < atoiOr0(out[j][2:]) })
	return out
}

// renderInvariantRegistry: do the three places that enumerate invariants agree?
//
// ARCHITECTURE.md section 28 is the registry CLAUDE.md calls authoritative and
// the prosecutor template tells an auditor to enumerate. CLAUDE.md carries a
// condensed copy under an explicit instruction to keep the row set in sync.
// docs/AUDIT-TRIGGERS.md cites invariants per surface. Nothing compared them.
//
// It was worth building because the drift it detects had already happened
// TWICE by the time this was written -- once repaired as RW-10 (the note is
// still in CLAUDE.md above the table), and once live: CLAUDE.md's table ended
// at I-39 while ARCH ran to I-44, and I-45 was the named invariant of an
// audit-trigger surface while appearing in NEITHER. A rule saying "keep these
// in sync" is safe-if-remembered; only a check that fails is safe-by-default,
// and the repaired-then-recurred history is the proof that remembering loses.
//
// A CITATION is any `I-NN` token. A ROW is a table line whose leading cell is
// the id -- that distinction is the whole check, because a document can discuss
// an invariant at length while not registering it, which is exactly the I-45
// state (GPU-DESIGN.md defines it in prose under a "(proposed)" heading).
func renderInvariantRegistry(reg *Registry) string {
	root := vaultRoot(reg)
	if root == "" {
		return "**(registry empty — cannot locate the repo root.)**"
	}
	read := func(p string) string {
		b, err := os.ReadFile(filepath.Join(root, p))
		if err != nil {
			return ""
		}
		return string(b)
	}
	arch, claude, trig := read("docs/ARCHITECTURE.md"), read("CLAUDE.md"), read("docs/AUDIT-TRIGGERS.md")
	if arch == "" || claude == "" || trig == "" {
		return "**(one of ARCHITECTURE.md / CLAUDE.md / AUDIT-TRIGGERS.md is unreadable — cannot compare.)**"
	}
	archRows := invSet(invRowRe, arch, 1)
	claudeRows := invSet(invRowRe, claude, 1)
	cited := invSet(invRe, trig, 1)
	// Guard the guard (main's, adopted after they probed it and I did not).
	// If a table reformats, `invRowRe` matches nothing and set-equality between
	// two empty sets passes -- the check reports agreement precisely when it
	// has stopped reading its inputs. Mine is also the more brittle regex of
	// the two: theirs absorbs arbitrary whitespace after the pipe, mine allows
	// one space, so a cosmetic reflow breaks this parser first.
	//
	// Worth stating how this was found, because it is the rule under test: I
	// probed their guard with a sabotage that did not break their parse, so it
	// passed, and "their guard did not fire" was a finding I nearly reported
	// off my own inert fixture. The real probe -- dropping the leading pipe --
	// fired it correctly. A negative result is only evidence once the sabotage
	// is shown to bite.
	if len(archRows) < 20 || len(claudeRows) < 20 {
		return fmt.Sprintf("**A TABLE PARSED AS NEAR-EMPTY (ARCH=%d, CLAUDE.md=%d) — "+
			"the row pattern no longer matches the table format, so every comparison "+
			"below would agree vacuously. This view is reporting nothing until the "+
			"parser is fixed.**", len(archRows), len(claudeRows))
	}
	notes := map[string]bool{}
	for _, n := range reg.OfType("inv") {
		if num := n.Front.Str("number"); num != "" {
			notes[num] = true
		}
	}

	var citedNotRegistered, archNotClaude, claudeNotArch, registeredNoNote []string
	for _, id := range sortedInvs(cited) {
		if !archRows[id] {
			citedNotRegistered = append(citedNotRegistered, id)
		}
	}
	for _, id := range sortedInvs(archRows) {
		if !claudeRows[id] {
			archNotClaude = append(archNotClaude, id)
		}
		if !notes[id] {
			registeredNoNote = append(registeredNoNote, id)
		}
	}
	for _, id := range sortedInvs(claudeRows) {
		if !archRows[id] {
			claudeNotArch = append(claudeNotArch, id)
		}
	}

	out := []string{fmt.Sprintf(
		"**ARCH §28: %d rows · CLAUDE.md: %d rows · AUDIT-TRIGGERS cites %d · vault notes: %d.**",
		len(archRows), len(claudeRows), len(cited), len(notes)), ""}
	row := func(label string, ids []string, why string) {
		if len(ids) == 0 {
			return
		}
		out = append(out, fmt.Sprintf("| %s | `%s` | %s |", label, strings.Join(ids, "`, `"), why))
	}
	body := []string{"| gap | invariants | why it matters |", "|---|---|---|"}
	head := len(out)
	out = append(out, body...)
	row("**cited by a trigger row, not registered in ARCH §28**", citedNotRegistered,
		"the prosecutor template says to enumerate §28, so a round on that surface is pointed at a list omitting the invariant it was spawned to prosecute")
	row("**in ARCH §28, missing from CLAUDE.md**", archNotClaude,
		"CLAUDE.md is loaded into every session, so this is what an instance believes by DEFAULT — a §28 row it lacks is one nobody reads unless they open ARCH")
	row("**in CLAUDE.md, missing from ARCH §28**", claudeNotArch,
		"drift the other way: the condensed copy asserts a row the registry does not have")
	row("registered but no vault note", registeredNoNote,
		"the vault has not written this one up; not a scripture defect")
	if len(out) == head+len(body) {
		return out[0] + "\n\nAll three enumerations agree, and every registered invariant has a note."
	}
	return strings.Join(out, "\n")
}
