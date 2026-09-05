package main

// Derive an ABI's mirror set from the tree instead of trusting a hand-written
// list. The boot-banner registry is the worked case: its prose named four
// files, one of which had never existed and one of which matches neither of the
// strings it is a "mandatory co-update target" for, while FOURTEEN real
// matchers went unnamed. A list nobody checks is a list that drifts, and the
// only fix that is safe-BY-DEFAULT rather than safe-if-remembered is one that
// fails.
//
// Two directions, because they catch different failures and only one of them is
// the one people think of:
//
//   UNDECLARED -- a file matches a literal and the note does not name it. This
//   is the new-consumer case: someone writes another .exp, it silently becomes
//   part of the ABI's blast radius, and nothing connects it to the registry.
//
//   UNMATCHED -- the note names a file that no longer contains any literal.
//   This is the rename/retire case, and it is what turns a mirror list into
//   fiction one entry at a time. It is exactly how `tools/agent-protocol.md`
//   survived: nothing ever asked whether the named file matched anything.
//
// The judgement this CANNOT make is comment-vs-code. `tools/warp-host.sh` names
// the banner in a usage comment and `tools/interactive/go8d.exp` in a prose
// note; both go stale rather than break, so neither is a mirror. That call is
// made once, per file, and recorded in `literal-mentions` on the note. The
// limitation is real and worth stating plainly: if a file listed there later
// grows a genuine matcher, this check stays quiet, because from the outside the
// two look identical. What it does guarantee is that no file joins the hit set
// unnoticed -- the mention list is a set of decisions somebody made, not a set
// of files somebody skipped.

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

func execGitLsFiles(root, dir string) ([]string, error) {
	out, err := exec.Command("git", "-C", root, "ls-files", dir).Output()
	if err != nil {
		return nil, err
	}
	return strings.Fields(string(out)), nil
}

// declaredPath pulls the path out of a frontmatter entry that may carry a
// human annotation: `kernel/main.c (boot_mark_complete)` and
// `tools/test-fault.sh (also the extinction MESSAGE bodies)` both yield their
// first token. Taking the whole value would make every annotated pin
// unmatchable, which is the bug that hid abi-boot-banner from `owner
// kernel/main.c` -- annotated entries are the norm here, not the exception.
func declaredPath(v string) string {
	f := strings.Fields(v)
	if len(f) == 0 {
		return ""
	}
	return strings.Trim(f[0], "`\"'")
}

// abiLiteralSpec is one abi note's declaration, resolved.
type abiLiteralSpec struct {
	ID       string
	Literals []string
	Scan     []string
	Declared map[string]bool // mirrors + pinned-by + literal-mentions
	Mirrors  map[string]bool // mirrors alone -- the UNMATCHED direction
}

// abiLiteralSpecs collects every abi note that opts in by declaring `literals`.
// Absence of the field means the note is not making a derivable claim, which is
// the correct default: most ABIs are struct layouts and syscall numbers, where
// a fixed string to grep for does not exist.
func abiLiteralSpecs(reg *Registry) []abiLiteralSpec {
	var out []abiLiteralSpec
	for _, n := range reg.Notes() {
		if n.Front.Str("type") != "abi" {
			continue
		}
		lits := n.Front.ListOr("literals")
		if len(lits) == 0 {
			continue
		}
		s := abiLiteralSpec{
			ID:       n.ID,
			Literals: lits,
			Scan:     n.Front.ListOr("literal-scan"),
			Declared: map[string]bool{},
			Mirrors:  map[string]bool{},
		}
		if len(s.Scan) == 0 {
			s.Scan = []string{"tools"}
		}
		for _, v := range n.Front.ListOr("mirrors") {
			if p := declaredPath(v); p != "" {
				s.Declared[p] = true
				s.Mirrors[p] = true
			}
		}
		for _, k := range []string{"pinned-by", "literal-mentions"} {
			for _, v := range n.Front.ListOr(k) {
				if p := declaredPath(v); p != "" {
					s.Declared[p] = true
				}
			}
		}
		out = append(out, s)
	}
	return out
}

// trackedFiles: every tracked file under dir, unfiltered.
//
// NOT treeFiles, and the difference cost a wrong answer. treeFiles filters
// through srcRe -- `^(kernel|arch|mm|usr)/.*\.(c|h|S|rs)$` -- because it serves
// the COVERAGE LEDGER, which counts source files. Reused here it returned
// nothing at all for `tools`, so the hit set was empty and every declared
// mirror reported as unmatched: fifteen confident findings measured against no
// data. The helper was right for its own job and silently wrong for this one,
// which is what makes borrowing a filtered walker a trap rather than a typo.
func trackedFiles(root, dir string) []string {
	out, err := execGitLsFiles(root, dir)
	if err != nil {
		return nil
	}
	return out
}

// literalHits: tracked files under the scan roots containing any literal.
func literalHits(root string, s abiLiteralSpec) map[string]bool {
	hits := map[string]bool{}
	for _, dir := range s.Scan {
		for _, f := range trackedFiles(root, dir) {
			b, err := os.ReadFile(filepath.Join(root, f))
			if err != nil {
				continue
			}
			body := string(b)
			for _, lit := range s.Literals {
				if strings.Contains(body, lit) {
					hits[f] = true
					break
				}
			}
		}
	}
	return hits
}

// checkAbiLiterals is the lint arm. Fails, not warns: this is the check whose
// whole purpose is to be un-ignorable, and a warn on a drifted ABI mirror set
// is the safe-if-remembered posture wearing a check's clothing.
func checkAbiLiterals(root string, reg *Registry) []string {
	var fails []string
	for _, s := range abiLiteralSpecs(reg) {
		hits := literalHits(root, s)
		// THE POSITIVE CONTROL, and it is not decoration. A declared ABI whose
		// literals appear nowhere means the scan is broken or the literals are
		// wrong -- never that the tree is clean. Without it an empty hit set
		// reads as "nothing undeclared", which is the shape of every detector
		// that passes by measuring nothing. This exact failure happened while
		// writing this file (a filtered tree-walk returned no files), and it
		// was caught only because the OTHER direction happened to be loud.
		// Relying on that was luck; this is the check.
		if len(hits) == 0 {
			fails = append(fails, fmt.Sprintf(
				"%s: declares %d literal(s) and NOTHING under %s matches any of "+
					"them -- the scan is broken or the literals are wrong; an "+
					"empty hit set is never a clean result",
				s.ID, len(s.Literals), strings.Join(s.Scan, " ")))
			continue
		}
		var undeclared []string
		for f := range hits {
			if !s.Declared[f] {
				undeclared = append(undeclared, f)
			}
		}
		var unmatched []string
		for f := range s.Mirrors {
			// Only mirrors inside a scan root are checkable; a mirror
			// elsewhere is out of this check's reach and must not be reported
			// as missing, which would be a phantom finding of exactly the kind
			// this file exists to prevent.
			if !inScan(f, s.Scan) {
				continue
			}
			if !hits[f] {
				unmatched = append(unmatched, f)
			}
		}
		sort.Strings(undeclared)
		sort.Strings(unmatched)
		if len(undeclared) > 0 {
			fails = append(fails, fmt.Sprintf(
				"%s: %d file(s) match its literals and are undeclared -- add to "+
					"`mirrors` (it breaks) or `literal-mentions` (it only goes "+
					"stale): %s",
				s.ID, len(undeclared), strings.Join(undeclared, " ")))
		}
		if len(unmatched) > 0 {
			fails = append(fails, fmt.Sprintf(
				"%s: %d declared mirror(s) contain none of its literals -- "+
					"renamed, retired, or never real: %s",
				s.ID, len(unmatched), strings.Join(unmatched, " ")))
		}
	}
	sort.Strings(fails)
	return fails
}

func inScan(path string, scan []string) bool {
	for _, d := range scan {
		if path == d || strings.HasPrefix(path, d+"/") {
			return true
		}
	}
	return false
}

// abiLiteralLead is the `owner` arm, and it is the half that answers the
// question the lint cannot: the lint fires when SOMEBODY runs the vault's lint
// suite, and the instance rewording a banner string is on another track and
// never does. `owner` runs at the mandatory doc-update step on the very paths
// being changed, so a new consumer is named to the person creating it.
//
// Returns one line per abi whose literals this path contains but which does not
// declare it.
func abiLiteralLead(root string, reg *Registry, path string) []string {
	b, err := os.ReadFile(filepath.Join(root, path))
	if err != nil {
		return nil
	}
	body := string(b)
	var out []string
	for _, s := range abiLiteralSpecs(reg) {
		if s.Declared[path] {
			continue
		}
		for _, lit := range s.Literals {
			if strings.Contains(body, lit) {
				out = append(out, fmt.Sprintf(
					"MATCHES %s's ABI literal %q and is not in its mirror set",
					s.ID, lit))
				break
			}
		}
	}
	sort.Strings(out)
	return out
}
