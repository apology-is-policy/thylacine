package main

// View renderers. Output is byte-identical to the reference
// implementation's (the committed generated views are the oracle: a
// renderer drift shows up as a spurious "stale generated body" fail).

import (
	"fmt"
	"os"
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

var renderers = map[string]func(*Registry) string{
	"dashboard":      renderDashboard,
	"invariants":     renderInvariants,
	"seams":          renderSeams,
	"audit-triggers": renderAuditTriggers,
	"roadmap":        renderRoadmap,
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

func checkViews(reg *Registry) []string {
	var fails []string
	for _, v := range viewNotes(reg) {
		nu, errMsg := renderedBody(v, reg)
		if errMsg != "" {
			fails = append(fails, errMsg)
		} else if nu != v.Raw {
			fails = append(fails, v.Rel+
				": stale generated body (run quaestor render)")
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
