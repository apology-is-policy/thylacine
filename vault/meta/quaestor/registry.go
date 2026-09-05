package main

// Registry: walk vault/ (excluding vault/meta/ -- prose + machinery, and
// vault/journal/ -- the operator's Obsidian scratch, never load-bearing
// per R8), load every .md as a note. Walk order is lexical per directory
// (matches the reference implementation's sorted-filenames discipline;
// the ordered slice preserves load order for deterministic rendering).

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

type Note struct {
	ID    string
	Path  string // absolute
	Rel   string // repo-root-relative, forward slashes
	Front *Front
	Body  string
	Raw   string
}

type Registry struct {
	byID    map[string]*Note
	ordered []*Note // load order
}

func (r *Registry) Get(id string) (*Note, bool) { n, ok := r.byID[id]; return n, ok }
func (r *Registry) Has(id string) bool          { _, ok := r.byID[id]; return ok }
func (r *Registry) Len() int                    { return len(r.byID) }

// Notes returns the registry in load order.
func (r *Registry) Notes() []*Note { return r.ordered }

// ByRel returns notes sorted by repo-relative path (the validation order).
func (r *Registry) ByRel() []*Note {
	out := make([]*Note, len(r.ordered))
	copy(out, r.ordered)
	sort.Slice(out, func(i, j int) bool { return out[i].Rel < out[j].Rel })
	return out
}

// OfType returns notes of a type in load order.
func (r *Registry) OfType(t string) []*Note {
	var out []*Note
	for _, n := range r.ordered {
		if n.Front.Str("type") == t {
			out = append(out, n)
		}
	}
	return out
}

// repoRoot resolves the vault's repo root. Order: an explicit --root flag
// (the hook passes it -- inside a git hook GIT_DIR is a RELATIVE path and
// `go -C` moves the child's cwd, so `git rev-parse` there resolves
// nothing: the fail-open the first live hook run exposed); then git; then
// a walk-up from cwd looking for vault/meta. Callers must still fail
// CLOSED on an empty registry -- a root with no notes is never "clean".
func repoRoot(explicit string) string {
	if explicit != "" {
		return explicit
	}
	out, err := exec.Command("git", "rev-parse", "--show-toplevel").Output()
	if err == nil {
		top := strings.TrimSpace(string(out))
		if _, serr := os.Stat(filepath.Join(top, "vault", "meta")); serr == nil {
			return top
		}
	}
	wd, _ := os.Getwd()
	for dir := wd; ; {
		if _, serr := os.Stat(filepath.Join(dir, "vault", "meta")); serr == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return wd
		}
		dir = parent
	}
}

func isRecord(rel string) bool {
	return strings.HasPrefix(rel, "vault/record/")
}

func loadRegistry(root string) (*Registry, []string) {
	vaultDir := filepath.Join(root, "vault")
	reg := &Registry{byID: map[string]*Note{}}
	var errors []string
	_ = filepath.WalkDir(vaultDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if path == vaultDir {
				return nil
			}
			rel, _ := filepath.Rel(vaultDir, path)
			rel = filepath.ToSlash(rel)
			if rel == "meta" || rel == "journal" || strings.HasPrefix(d.Name(), ".") {
				return fs.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(d.Name(), ".md") {
			return nil
		}
		raw, rerr := os.ReadFile(path)
		if rerr != nil {
			return nil
		}
		rel, _ := filepath.Rel(root, path)
		rel = filepath.ToSlash(rel)
		front, body, ok := parseFront(string(raw))
		stem := strings.TrimSuffix(d.Name(), ".md")
		if !ok {
			errors = append(errors, rel+": missing or unterminated frontmatter")
			return nil
		}
		n := &Note{ID: stem, Path: path, Rel: rel, Front: front,
			Body: body, Raw: string(raw)}
		if reg.Has(stem) {
			errors = append(errors, rel+": duplicate id '"+stem+"'")
		} else {
			reg.ordered = append(reg.ordered, n)
		}
		reg.byID[stem] = n
		return nil
	})
	return reg, errors
}

// aliasMap: alias -> id, first-wins.
func aliasMap(reg *Registry) map[string]string {
	amap := map[string]string{}
	for _, n := range reg.ordered {
		for _, a := range n.Front.ListOr("aliases") {
			if _, ok := amap[a]; !ok {
				amap[a] = n.ID
			}
		}
	}
	return amap
}

// pyList renders a []string the way the reference implementation printed
// Python lists in fail messages: ['a', 'b'].
func pyList(items []string) string {
	var b strings.Builder
	b.WriteString("[")
	for i, s := range items {
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString("'" + s + "'")
	}
	b.WriteString("]")
	return b.String()
}
