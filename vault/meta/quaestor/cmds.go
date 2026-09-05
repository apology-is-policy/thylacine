package main

// The workflow subcommands beyond lint: new / query / backlinks / close /
// id. All operate on plain files -- Obsidian stays a viewer (R8).

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Default directory per type (repo-relative). moc/sub live inside the
// subsystem spine and need an explicit --dir.
var typeDirs = map[string]string{
	"inv":  "vault/invariants",
	"spec": "vault/specs",
	"abi":  "vault/abis",
	"lock": "vault/locks",
	"lin":  "vault/lineages",
	"haz":  "vault/hazards",
	"gls":  "vault/glossary",
	"gate": "vault/gates",
	"seam": "vault/seams",
	"msr":  "vault/measurements",
	"wkf":  "vault/workflows",
	"view": "vault/views",
	"arc":  "vault/record/arcs",
	"chg":  "vault/record/changes",
	"adt":  "vault/record/audits",
	"fnd":  "vault/record/findings",
	"dec":  "vault/record/decisions",
}

var placeholderDateRe = regexp.MustCompile(`\{YYYY-MM-DD\}`)

// checkID validates an id candidate against the shape rules + registry.
// Returns problem strings (empty = ok).
func checkID(reg *Registry, id string) []string {
	var probs []string
	if NO_PREFIX_IDS[id] {
		probs = append(probs, "'"+id+"' is a reserved singleton id")
		return probs
	}
	if !idRe.MatchString(id) {
		probs = append(probs, "'"+id+"' is not kebab-case type-prefixed "+
			"(^<type>-[a-z0-9][a-z0-9.-]*$)")
	}
	if n, ok := reg.Get(id); ok {
		probs = append(probs, "'"+id+"' already exists: "+n.Rel)
	}
	if owner, ok := aliasMap(reg)[id]; ok {
		probs = append(probs, "'"+id+"' collides with an alias of "+owner)
	}
	return probs
}

func cmdID(root string, args []string) int {
	if len(args) != 1 {
		fmt.Println("usage: quaestor id <candidate-id>")
		return 2
	}
	reg, _ := loadRegistry(root)
	probs := checkID(reg, args[0])
	if len(probs) == 0 {
		fmt.Println("ok: " + args[0])
		return 0
	}
	for _, p := range probs {
		fmt.Println("FAIL " + p)
	}
	return 1
}

// setFrontLine rewrites (or inserts before the terminator) a single
// frontmatter line "key: value" in raw note text, preserving everything
// else byte-for-byte. Values containing ": " or starting with a reserved
// char are quoted.
func setFrontLine(raw, key, value string) (string, error) {
	end, _ := frontBounds(raw)
	if end == -1 {
		return "", fmt.Errorf("missing or unterminated frontmatter")
	}
	needQuote := key == "title" || strings.ContainsAny(value, ":# ") ||
		strings.HasPrefix(value, "[") || strings.HasPrefix(value, "{") ||
		strings.HasPrefix(value, "'")
	rendered := value
	if needQuote {
		rendered = "\"" + value + "\""
	}
	newLine := key + ": " + rendered
	block := raw[4:end]
	lines := strings.Split(block, "\n")
	lineRe := regexp.MustCompile(`^` + regexp.QuoteMeta(key) + `:`)
	replaced := false
	for i, ln := range lines {
		if lineRe.MatchString(ln) {
			lines[i] = newLine
			replaced = true
			break
		}
	}
	if !replaced {
		lines = append(lines, newLine)
	}
	return raw[:4] + strings.Join(lines, "\n") + raw[end:], nil
}

// newNote creates a typed note from its template. fields overrides
// individual scalar frontmatter lines. Returns the repo-relative path.
func newNote(root, typ, id, title, dir string, fields map[string]string) (string, error) {
	if !typeKnown(typ) {
		return "", fmt.Errorf("unknown type '%s'", typ)
	}
	if !strings.HasPrefix(id, typ+"-") {
		return "", fmt.Errorf("id '%s' does not carry the '%s-' prefix", id, typ)
	}
	reg, _ := loadRegistry(root)
	if probs := checkID(reg, id); len(probs) > 0 {
		return "", fmt.Errorf("%s", strings.Join(probs, "; "))
	}
	if dir == "" {
		dir = typeDirs[typ]
		if dir == "" {
			return "", fmt.Errorf("type '%s' needs an explicit --dir "+
				"(it lives in the subsystem spine)", typ)
		}
	}
	tpl, err := os.ReadFile(filepath.Join(root, "vault/meta/templates", typ+".md"))
	if err != nil {
		return "", fmt.Errorf("no template for type '%s': %v", typ, err)
	}
	today := time.Now().Format("2006-01-02")
	text := placeholderDateRe.ReplaceAllString(string(tpl), today)
	if text, err = setFrontLine(text, "id", id); err != nil {
		return "", err
	}
	if title != "" {
		if text, err = setFrontLine(text, "title", title); err != nil {
			return "", err
		}
	}
	var fkeys []string
	for k := range fields {
		fkeys = append(fkeys, k)
	}
	sort.Strings(fkeys)
	for _, k := range fkeys {
		if text, err = setFrontLine(text, k, fields[k]); err != nil {
			return "", err
		}
	}
	rel := filepath.ToSlash(filepath.Join(dir, id+".md"))
	abs := filepath.Join(root, rel)
	if _, err := os.Stat(abs); err == nil {
		return "", fmt.Errorf("%s already exists", rel)
	}
	if err := os.MkdirAll(filepath.Dir(abs), 0o755); err != nil {
		return "", err
	}
	if err := os.WriteFile(abs, []byte(text), 0o644); err != nil {
		return "", err
	}
	return rel, nil
}

func cmdNew(root string, args []string) int {
	if len(args) < 2 {
		fmt.Println("usage: quaestor new <type> <id> [--title T] [--dir D] [--set key=value]...")
		return 2
	}
	typ, id := args[0], args[1]
	title, dir := "", ""
	fields := map[string]string{}
	rest := args[2:]
	for i := 0; i < len(rest); i++ {
		switch rest[i] {
		case "--title":
			i++
			if i < len(rest) {
				title = rest[i]
			}
		case "--dir":
			i++
			if i < len(rest) {
				dir = rest[i]
			}
		case "--set":
			i++
			if i < len(rest) {
				k, v, ok := strings.Cut(rest[i], "=")
				if !ok {
					fmt.Println("FAIL --set wants key=value, got: " + rest[i])
					return 2
				}
				fields[k] = v
			}
		default:
			fmt.Println("FAIL unknown flag: " + rest[i])
			return 2
		}
	}
	rel, err := newNote(root, typ, id, title, dir, fields)
	if err != nil {
		fmt.Println("FAIL " + err.Error())
		return 1
	}
	fmt.Println("created: " + rel)
	fmt.Println("fill the remaining placeholder fields, then: quaestor lint")
	return 0
}

// queryFindings filters fnd notes by surface (exact list membership or
// scalar equality) and status.
func queryFindings(reg *Registry, surface, status string) []*Note {
	var out []*Note
	for _, n := range reg.OfType("fnd") {
		if status != "" && n.Front.Str("status") != status {
			continue
		}
		if surface != "" {
			v, ok := n.Front.Get("surface")
			if !ok {
				continue
			}
			match := false
			for _, s := range v.Vals() {
				if s == surface {
					match = true
					break
				}
			}
			if !match {
				continue
			}
		}
		out = append(out, n)
	}
	return sortByID(out)
}

func querySeams(reg *Registry, status string) []*Note {
	var out []*Note
	for _, n := range reg.OfType("seam") {
		if status != "" && n.Front.Str("status") != status {
			continue
		}
		out = append(out, n)
	}
	return sortByID(out)
}

func cmdQuery(root string, args []string) int {
	if len(args) < 1 || (args[0] != "fnd" && args[0] != "seam") {
		fmt.Println("usage: quaestor query <fnd|seam> [--surface S] [--status ST] [--json]")
		return 2
	}
	kind := args[0]
	surface, status := "", ""
	asJSON := false
	rest := args[1:]
	for i := 0; i < len(rest); i++ {
		switch rest[i] {
		case "--surface":
			i++
			if i < len(rest) {
				surface = rest[i]
			}
		case "--status":
			i++
			if i < len(rest) {
				status = rest[i]
			}
		case "--json":
			asJSON = true
		default:
			fmt.Println("FAIL unknown flag: " + rest[i])
			return 2
		}
	}
	reg, _ := loadRegistry(root)
	var notes []*Note
	if kind == "fnd" {
		notes = queryFindings(reg, surface, status)
	} else {
		notes = querySeams(reg, status)
	}
	if asJSON {
		fmt.Println(queryJSON(kind, notes))
		return 0
	}
	for _, n := range notes {
		if kind == "fnd" {
			fmt.Printf("%s\t[%s]\t%s\t(%s)\n", n.ID,
				n.Front.Str("severity"), n.Front.Str("title"),
				n.Front.Str("status"))
		} else {
			fmt.Printf("%s\t(%s)\t%s\t[%s]\n", n.ID,
				n.Front.Str("status"), n.Front.Str("title"),
				line(n, "surface"))
		}
	}
	fmt.Printf("%d %s note(s)\n", len(notes), kind)
	return 0
}

func queryJSON(kind string, notes []*Note) string {
	type row map[string]any
	var rows []row
	for _, n := range notes {
		r := row{"id": n.ID, "title": n.Front.Str("title")}
		if kind == "fnd" {
			r["severity"] = n.Front.Str("severity")
			r["status"] = n.Front.Str("status")
			r["surface"] = edgeVals(n.Front, "surface")
			r["round"] = n.Front.Str("round")
			r["fixed-by"] = n.Front.Str("fixed-by")
		} else {
			r["status"] = n.Front.Str("status")
			r["surface"] = edgeVals(n.Front, "surface")
			r["tracker"] = n.Front.Str("tracker")
		}
		rows = append(rows, r)
	}
	b, _ := json.MarshalIndent(rows, "", "  ")
	return string(b)
}

type backlink struct {
	From string `json:"from"`
	Via  string `json:"via"`
}

// backlinks: every incoming reference to an id -- strict edge fields plus
// body wikilinks. The substrate of the deferred Provenance renderer.
func backlinks(reg *Registry, target string) []backlink {
	var out []backlink
	for _, n := range reg.ByRel() {
		if n.ID == target {
			continue
		}
		for _, field := range n.Front.Keys() {
			if !STRICT_EDGE_FIELDS[field] {
				continue
			}
			v, _ := n.Front.Get(field)
			for _, val := range v.Vals() {
				if val == target {
					out = append(out, backlink{n.ID, field})
					break
				}
			}
		}
		for _, m := range wikilinkRe.FindAllStringSubmatch(n.Body, -1) {
			if strings.TrimSpace(m[1]) == target {
				out = append(out, backlink{n.ID, "body"})
				break
			}
		}
	}
	return out
}

func cmdBacklinks(root string, args []string) int {
	asJSON := false
	var target string
	for _, a := range args {
		if a == "--json" {
			asJSON = true
		} else {
			target = a
		}
	}
	if target == "" {
		fmt.Println("usage: quaestor backlinks <id> [--json]")
		return 2
	}
	reg, _ := loadRegistry(root)
	if !reg.Has(target) {
		fmt.Println("FAIL unknown id '" + target + "'")
		return 1
	}
	links := backlinks(reg, target)
	if asJSON {
		b, _ := json.MarshalIndent(links, "", "  ")
		fmt.Println(string(b))
		return 0
	}
	for _, l := range links {
		fmt.Printf("%s\t%s\n", l.From, l.Via)
	}
	fmt.Printf("%d incoming reference(s) to %s\n", len(links), target)
	return 0
}

// closeNote flips closure fields on a Record note (fnd/dec), refusing
// everything else by construction. The pre-commit gate still requires a
// staged chg-* linking the note; close warns when none exists yet.
func closeNote(root, id string, fields map[string]string) (string, error) {
	reg, _ := loadRegistry(root)
	n, ok := reg.Get(id)
	if !ok {
		return "", fmt.Errorf("unknown id '%s'", id)
	}
	t := n.Front.Str("type")
	allowed := CLOSURE[t]
	if len(allowed) == 0 {
		return "", fmt.Errorf("type '%s' has no closure fields (schema 5.3)", t)
	}
	if !closurePlaneOK(t, n.Rel) {
		return "", fmt.Errorf("%s is not on the Record plane", n.Rel)
	}
	if len(fields) == 0 {
		return "", fmt.Errorf("nothing to set")
	}
	var keys []string
	for k := range fields {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if !allowed[k] {
			return "", fmt.Errorf("'%s' is not a closure field for type "+
				"'%s' (allowed: %s)", k, t, pyList(sortedKeys(allowed)))
		}
	}
	// Validate enum values + edge targets before touching the file.
	for _, e := range ENUMS {
		if e.typ == t {
			if v, ok := fields[e.field]; ok && !e.allowed[v] {
				return "", fmt.Errorf("%s='%s' not in %s", e.field, v,
					pyList(sortedKeys(e.allowed)))
			}
		}
	}
	for _, k := range keys {
		v := fields[k]
		if STRICT_EDGE_FIELDS[k] && idRe.MatchString(v) && !reg.Has(v) {
			return "", fmt.Errorf("%s -> unknown id '%s' (create it first)", k, v)
		}
	}
	raw := n.Raw
	var err error
	for _, k := range keys {
		if raw, err = setFrontLine(raw, k, fields[k]); err != nil {
			return "", err
		}
	}
	if err := os.WriteFile(n.Path, []byte(raw), 0o644); err != nil {
		return "", err
	}
	msg := "closed: " + n.Rel
	linked := false
	for _, c := range reg.OfType("chg") {
		if strings.Contains(c.Raw, id) {
			linked = true
			break
		}
	}
	if !linked {
		msg += "\nNOTE: no chg-* note links '" + id + "' yet -- the " +
			"pre-commit gate requires one staged in the same commit"
	}
	return msg, nil
}

func cmdClose(root string, args []string) int {
	if len(args) < 1 {
		fmt.Println("usage: quaestor close <id> --status S [--fixed-by C] [--regression R] [--seam S] [--superseded-by D] [--closed-by C]")
		return 2
	}
	id := args[0]
	fields := map[string]string{}
	rest := args[1:]
	for i := 0; i < len(rest); i++ {
		flag := strings.TrimPrefix(rest[i], "--")
		if flag == rest[i] {
			fmt.Println("FAIL unknown argument: " + rest[i])
			return 2
		}
		i++
		if i >= len(rest) {
			fmt.Println("FAIL --" + flag + " wants a value")
			return 2
		}
		fields[flag] = rest[i]
	}
	msg, err := closeNote(root, id, fields)
	if err != nil {
		fmt.Println("FAIL " + err.Error())
		return 1
	}
	fmt.Println(msg)
	return 0
}
