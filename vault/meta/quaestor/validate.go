package main

// Validation: the schema's teeth (vault/meta/schema.md section 8).
// Message strings are the reference implementation's, verbatim -- the
// parity gate compared them line-for-line before lint.py retired.

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"unicode"
)

var TYPES = []string{"moc", "sub", "inv", "spec", "abi", "lock", "lin",
	"haz", "gls", "gate", "seam", "msr", "arc", "chg", "adt", "fnd",
	"dec", "wkf", "view"}

var NO_PREFIX_IDS = map[string]bool{"home": true, "dashboard": true}

var idRe = regexp.MustCompile(
	`^(` + strings.Join(TYPES, "|") + `)-[a-z0-9][a-z0-9.-]*$`)

var REQUIRED = map[string][]string{
	"moc":  {"parent"},
	"sub":  {"parent", "code", "audit", "guarded-by", "validated-by"},
	"inv":  {"number", "guards", "validated-by", "strength"},
	"spec": {"models", "pins", "cfgs", "gate"},
	"abi":  {"kind", "stability", "pinned-by", "mirrors"},
	"lock": {"kind", "guards"},
	"lin":  {"surfaces", "members"},
	"haz":  {"applies-to"},
	"gls":  {},
	"gate": {"proves", "blind-to", "invocation"},
	"seam": {"status", "surface", "opened-by"},
	"msr":  {"metric", "unit"},
	"arc":  {"status", "chunks"},
	"chg":  {"date", "arc", "commits", "touched", "depth"},
	"adt": {"date", "scope", "reviewer", "model-start", "model-end",
		"verdict", "counts", "findings"},
	"fnd":  {"round", "severity", "status", "surface", "threatens"},
	"dec":  {"date", "status", "decided-by", "affects"},
	"wkf":  {},
	"view": {"query"},
}

type enumRule struct {
	typ, field string
	allowed    map[string]bool
}

var ENUMS = []enumRule{
	{"sub", "audit", set("hard", "light", "none")},
	{"inv", "strength", set("spec", "test", "prose")},
	{"abi", "kind", set("syscall", "wire", "struct", "registry", "contract")},
	{"abi", "stability", set("frozen", "append-only", "internal")},
	{"seam", "status", set("open", "closed")},
	{"arc", "status", set("active", "complete", "abandoned")},
	{"chg", "depth", set("rich", "skeletal")},
	{"adt", "reviewer", set("fable", "opus", "self")},
	{"adt", "verdict", set("clean", "dirty")},
	{"fnd", "severity", set("P0", "P1", "P2", "P3")},
	{"fnd", "status", set("fixed", "deferred", "documented", "withdrawn")},
	{"dec", "status", set("standing", "superseded")},
	{"dec", "decided-by", set("user-vote", "autonomous", "research-collapsed")},
}

func set(items ...string) map[string]bool {
	m := map[string]bool{}
	for _, s := range items {
		m[s] = true
	}
	return m
}

func sortedKeys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// Fields whose id-shaped values must resolve in the registry.
var STRICT_EDGE_FIELDS = set(
	"parent", "guarded-by", "guards", "validated-by", "models", "pins",
	"hazards", "locks", "abis", "surfaces", "members", "applies-to",
	"touched", "established", "closed", "opened", "scope", "findings",
	"threatens", "surface", "arc", "chunks", "follow-ons", "round",
	"prior-round", "supersedes", "superseded-by", "fixed-by", "closed-by",
	"opened-by", "seam", "hazard", "instances", "orders-before",
	"refers-to", "affects", "round-of")

var EDGE_LITERALS = set("prose", "global")

// Record-plane closure fields (schema section 5.3).
var CLOSURE = map[string]map[string]bool{
	"fnd": set("status", "fixed-by", "regression", "seam"),
	"dec": set("superseded-by", "status"),
	"chg": set("commits"),
	"adt": {},
}

var SUB_SECTIONS = []string{"Purpose", "Contract", "Mechanism",
	"Data structures", "Concurrency", "Invariants enforced", "Error paths",
	"Performance", "Prosecution", "Seams", "Caveats", "Provenance"}

var fileLineRe = regexp.MustCompile(
	`\b[\w./-]+\.(?:c|h|rs|go|py|S|s|tla|md):\d+\b`)
var wikilinkRe = regexp.MustCompile(`\[\[([^\]#|]+)`)
var waivedRe = regexp.MustCompile(`(?m)^>\s*waived:\s*(.+?)\s+--`)
var sectionRe = regexp.MustCompile(`(?m)^##\s+(.+?)\s*$`)

func validate(reg *Registry, preErrors []string) (fails, warns []string) {
	fails = append(fails, preErrors...)
	amap := aliasMap(reg)
	for _, n := range reg.ByRel() {
		f := n.Front
		t := f.Str("type")
		if f.Str("id") != n.ID {
			fails = append(fails, fmt.Sprintf(
				"%s: frontmatter id '%s' != filename '%s'",
				n.Rel, f.Str("id"), n.ID))
		}
		if !typeKnown(t) {
			fails = append(fails, fmt.Sprintf(
				"%s: unknown type '%s'", n.Rel, t))
			continue
		}
		if !NO_PREFIX_IDS[n.ID] {
			if !strings.HasPrefix(n.ID, t+"-") {
				fails = append(fails, fmt.Sprintf(
					"%s: id prefix does not match type '%s'", n.Rel, t))
			}
			if !idRe.MatchString(n.ID) {
				fails = append(fails, fmt.Sprintf(
					"%s: id '%s' not kebab-case type-prefixed", n.Rel, n.ID))
			}
		}
		for _, req := range REQUIRED[t] {
			if req == "parent" && n.ID == "home" {
				continue
			}
			if !f.Has(req) {
				fails = append(fails, fmt.Sprintf(
					"%s: required field '%s' missing", n.Rel, req))
			}
		}
		for _, e := range ENUMS {
			if e.typ != t {
				continue
			}
			v, ok := f.Get(e.field)
			if ok && !v.IsList && !e.allowed[v.Str] {
				fails = append(fails, fmt.Sprintf(
					"%s: %s='%s' not in %s", n.Rel, e.field, v.Str,
					pyList(sortedKeys(e.allowed))))
			}
		}
		if isRecord(n.Rel) && f.Has("updated") {
			fails = append(fails, n.Rel+
				": 'updated' is forbidden on the Record plane")
		}
		// A flow list split across lines silently degrades in this
		// parser -- fail loudly instead (use a block list).
		for _, field := range f.Keys() {
			v, _ := f.Get(field)
			if !v.IsList && strings.HasPrefix(v.Str, "[") &&
				!strings.HasSuffix(strings.TrimRightFunc(v.Str, unicode.IsSpace), "]") {
				fails = append(fails, fmt.Sprintf(
					"%s: '%s' looks like an unterminated flow list "+
						"(multi-line [..] is unsupported; use a block list)",
					n.Rel, field))
			}
		}
		// Edge resolution.
		for _, field := range f.Keys() {
			if !STRICT_EDGE_FIELDS[field] {
				continue
			}
			v, _ := f.Get(field)
			for _, val := range v.Vals() {
				if val == "" {
					continue
				}
				if reg.Has(val) {
					continue
				}
				if idRe.MatchString(val) {
					fails = append(fails, fmt.Sprintf(
						"%s: %s -> unknown id '%s'", n.Rel, field, val))
				} else if EDGE_LITERALS[val] || strings.ContainsAny(val, "/ .") {
					// path or literal: passes
				} else {
					warns = append(warns, fmt.Sprintf(
						"%s: %s value '%s' is neither a known id shape "+
							"nor a path/literal", n.Rel, field, val))
				}
			}
		}
		// Wikilinks.
		for _, m := range wikilinkRe.FindAllStringSubmatch(n.Body, -1) {
			tgt := strings.TrimSpace(m[1])
			if tgt == "" || strings.Contains(tgt, "/") {
				continue
			}
			if !reg.Has(tgt) {
				if _, ok := amap[tgt]; !ok {
					fails = append(fails, fmt.Sprintf(
						"%s: dangling wikilink [[%s]]", n.Rel, tgt))
				}
			}
		}
		// Type-specific.
		if t == "sub" {
			validateSub(n, &fails, &warns)
		}
		if t == "fnd" && f.Str("status") == "deferred" {
			linked := strings.HasPrefix(f.Str("seam"), "seam-") ||
				strings.HasPrefix(f.Str("regression"), "seam-") ||
				strings.Contains(n.Body, "seam-")
			if !linked {
				fails = append(fails, n.Rel+
					": status=deferred without a seam-* link "+
					"(silent drops cannot land)")
			}
		}
		if t == "chg" {
			mirrorsNeeded := 0
			for _, tid := range edgeVals(f, "touched") {
				tn, ok := reg.Get(tid)
				if ok && tn.Front.Str("type") == "abi" {
					if m := len(tn.Front.ListOr("mirrors")); m > mirrorsNeeded {
						mirrorsNeeded = m
					}
				}
			}
			checked := len(edgeVals(f, "mirrors-checked"))
			if mirrorsNeeded > 0 && checked < mirrorsNeeded {
				fails = append(fails, fmt.Sprintf(
					"%s: touched abi has %d mirrors; mirrors-checked "+
						"covers %d", n.Rel, mirrorsNeeded, checked))
			}
		}
		if !isRecord(n.Rel) && t != "view" {
			for _, m := range fileLineRe.FindAllString(n.Body, -1) {
				warns = append(warns, fmt.Sprintf(
					"%s: file:line citation '%s' on the Present plane "+
						"(R4: cite symbols/tests)", n.Rel, m))
			}
		}
	}
	return fails, warns
}

// checkCodePaths: every path a dossier claims in `code:` must exist.
//
// A `code:` entry is the ownership assertion the coverage ledger counts, and it
// is resolvable, so it gets checked — the rule this arc arrived at after
// finding that `models:` (resolvable, checked) and `mirrors:` (free text, not)
// diverged for no better reason than what was cheap. It catches a fabricated
// path and, far more usefully, a path that rots when the file is renamed or
// deleted, which would otherwise leave a dossier owning nothing while the
// ledger still counted it.
//
// It does NOT catch a dossier claiming a real file it never swept. Nothing
// mechanical does; that is what reading is for.
//
// A `<repo>: path` entry names a file in a sibling tree (Stratum is in scope
// for this project and four dossiers cover it). Those are checked too when the
// sibling is on disk, and skipped when it is not — the check degrades rather
// than failing a vault that is merely checked out alone.
func checkCodePaths(reg *Registry) []string {
	root := vaultRoot(reg)
	if root == "" {
		return nil
	}
	// Siblings of the repo root: .../projects/thylacine-vault -> .../projects
	parent := filepath.Dir(filepath.Clean(root))
	siblings := map[string]string{
		"stratum": filepath.Join(parent, "stratum", "v2"),
	}
	var fails []string
	for _, n := range reg.OfType("sub") {
		for _, c := range n.Front.ListOr("code") {
			c = strings.TrimSpace(c)
			if c == "" {
				continue
			}
			base, rel := root, c
			if i := strings.Index(c, ":"); i > 0 {
				repo := strings.TrimSpace(c[:i])
				sib, known := siblings[repo]
				if !known {
					fails = append(fails, fmt.Sprintf(
						"%s: code -> unknown sibling repo '%s' in '%s'",
						n.Rel, repo, c))
					continue
				}
				if _, err := os.Stat(sib); err != nil {
					continue // sibling not checked out; nothing to verify against
				}
				base, rel = sib, strings.TrimSpace(c[i+1:])
			}
			if _, err := os.Stat(filepath.Join(base, rel)); err != nil {
				fails = append(fails, fmt.Sprintf(
					"%s: code -> no such file '%s'", n.Rel, c))
			}
		}
	}
	sort.Strings(fails)
	return fails
}

func validateSub(n *Note, fails, warns *[]string) {
	waived := map[string]bool{}
	for _, m := range waivedRe.FindAllStringSubmatch(n.Body, -1) {
		waived[strings.TrimSpace(m[1])] = true
	}
	var got []string
	for _, m := range sectionRe.FindAllStringSubmatch(n.Body, -1) {
		got = append(got, strings.TrimSpace(m[1]))
	}
	gotSet := set(got...)
	var need []string
	for _, s := range SUB_SECTIONS {
		if !waived[s] {
			need = append(need, s)
		}
	}
	var missing []string
	for _, s := range need {
		if !gotSet[s] {
			missing = append(missing, s)
		}
	}
	if len(missing) > 0 {
		*fails = append(*fails, fmt.Sprintf(
			"%s: dossier sections missing (no waiver): %s",
			n.Rel, pyList(missing)))
		return
	}
	needSet := set(need...)
	var order []string
	for _, s := range got {
		if needSet[s] {
			order = append(order, s)
		}
	}
	orderSet := set(order...)
	var expect []string
	for _, s := range need {
		if orderSet[s] {
			expect = append(expect, s)
		}
	}
	for i := range order {
		if order[i] != expect[i] {
			*warns = append(*warns,
				n.Rel+": dossier sections out of schema order")
			break
		}
	}
}

func typeKnown(t string) bool {
	for _, x := range TYPES {
		if x == t {
			return true
		}
	}
	return false
}

// edgeVals: the field's values as a slice (scalar -> one element),
// skipping the absent case.
func edgeVals(f *Front, field string) []string {
	v, ok := f.Get(field)
	if !ok {
		return nil
	}
	return v.Vals()
}
