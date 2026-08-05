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

var renderers = map[string]func(*Registry) string{
	"dashboard":      renderDashboard,
	"invariants":     renderInvariants,
	"seams":          renderSeams,
	"audit-triggers": renderAuditTriggers,
	"roadmap":        renderRoadmap,
	"absorption":     renderAbsorption,
	"spec-coverage":  renderSpecCoverage,
	"code-coverage":  renderCodeCoverage,
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
