package main

// quaestor owner -- who owns this file?
//
// The cheap authoritative test behind the per-surface cutover rule (CLAUDE.md):
// if a surface HAS a dossier, an edit goes to the dossier; if it does not, the
// reference doc is written as today and that edit is the signal to sweep it.
// The rule is only worth adopting if the test is one command. Telling a session
// to "grep the vault for the path" does not work, because the two conventions
// below are exactly where a grep gets the wrong answer confidently:
//
//   1. `kernel/foo.{c,h}` is a HOUSE CONVENTION, not a path. The .c is in
//      kernel/ and the .h is in kernel/include/thylacine/. A grep for the
//      header finds nothing while the dossier that owns the surface sits one
//      lookup away -- the same convention that manufactured ~20 phantom
//      headers in the audit-trigger ledger's first run.
//   2. A row can name a DIRECTORY (`usr/tapestryd/`) where dossiers claim
//      individual files under it.
//
// The failure that actually matters here is a false UNOWNED. It sends a session
// to write a reference doc for a surface the vault already carries, which
// re-opens the two-sources-of-truth divergence the cutover exists to close --
// silently, because both documents will look fine. So an unowned answer never
// stops at "no": it reports the .c/.h twin and the owned neighbours in the same
// directory, which is the information that turns "no dossier" into "extend
// sub-kernel-proc" nine times out of ten.

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

type ownerHit struct {
	Note    string `json:"note"`
	Rel     string `json:"rel"`
	Audit   string `json:"audit"`
	Updated string `json:"updated"`
	Claim   string `json:"claim"` // the `code:` entry that matched
	Stale   string `json:"stale,omitempty"`
	// The dossier's own title, reported because an id is not a scope. A large
	// file can be sole-owned by a dossier that describes one part of it, and
	// then OWNED routes a writer somewhere their prose does not belong. The id
	// `sub-stratum-boot` reads as "the whole boot"; its title, "Bringup --
	// spawn, wait for an event, attach, pivot", is what lets the reader see
	// that a foreign-shell gate is not covered by it. Printing the title does
	// not make the ownership record right -- it makes a wrong one visible at
	// the moment the decision is taken.
	Title string `json:"title,omitempty"`
}

// Owned and Covered are deliberately two fields, and the exit code follows
// COVERED.
//
// Owned is the strict fact: a `sub` dossier names this exact path in `code:`.
// That is the definition the coverage ledger counts, arrived at after the first
// ledger rotted by letting prose mention count, and this command must not
// quietly widen it -- two readings of "owned" is the drift half this codebase
// exists to catch.
//
// Covered is the DECISION: does the vault already carry this surface, so an
// edit belongs in the vault rather than in a new reference doc? A header whose
// .c twin is swept is covered and not owned. So is errno.h, which no dossier
// claims and the errno registry pins.
//
// Keying the exit status to Owned would have made the command contradict
// itself: it printed "extend the twin's dossier" and exited 1, so a human and a
// `||` in a shell got opposite answers from one invocation.
type ownerAnswer struct {
	Path       string     `json:"path"`
	Owned      bool       `json:"owned"`
	Covered    bool       `json:"covered"`
	Kind       string     `json:"kind"` // file | dir
	Owners     []ownerHit `json:"owners"`
	Twin       string     `json:"twin,omitempty"`
	TwinExists bool       `json:"twin-exists,omitempty"`
	TwinOwner  []string   `json:"twin-owner,omitempty"`
	RefBy      []string   `json:"referenced-by,omitempty"`
	// NotCode marks a path this command should refuse rather than answer:
	// a reference doc or a user-manual page. See notCodeSurface.
	NotCode    bool       `json:"not-code-surface,omitempty"`
	Neighbours
}

// notCodeSurface -- a documentation path is not a code surface, and answering
// UNOWNED about one is the single most damaging wrong answer this command can
// give. Reported by main 2026-08-15: every docs/reference/NN-*.md probed comes
// back UNOWNED while its code surface comes back OWNED, so the tool gives
// OPPOSITE answers for a file and the document describing it -- and the doc's
// answer is the one that routes AWAY from the vault ("write the reference doc
// as today"), re-opening the two-sources divergence the cutover closes.
//
// CLAUDE.md's step 0 is correct as written -- it says `owner <changed paths>`,
// meaning the code -- so this is a guard against the available slip, not a
// normal path. The sentence step 0 sits under is about writing a reference
// doc, which is exactly what makes naming the doc the natural mistake.
//
// It REFUSES rather than resolving. A mapping from a reference doc to its code
// surfaces is not recorded anywhere: two dossiers name one in `design:`, out of
// the whole corpus, so any resolution would be a guess from the filename. A
// guess here fails in the away-from-vault direction, which is the one that
// costs something. An honest "ask about the code instead" costs one re-run.
func notCodeSurface(p string) bool {
	return strings.HasPrefix(p, "docs/reference/") ||
		strings.HasPrefix(p, "docs/manual/")
}

// Neighbours is the "nearest thing" report for an unowned path: of the files
// beside it, how many are swept and by whom. Embedded rather than inlined so
// the JSON shape stays flat for a caller that only wants the counts.
type Neighbours struct {
	Dir        string   `json:"dir,omitempty"`
	DirOwned   int      `json:"dir-owned,omitempty"`
	DirTotal   int      `json:"dir-total,omitempty"`
	DirOwners  []string `json:"dir-owners,omitempty"`
	Unswept    []string `json:"unswept,omitempty"` // dir mode only
	UnsweptTot int      `json:"unswept-total,omitempty"`
}

// ownerIndex maps a claimed path to the dossiers claiming it. Parsing goes
// through codeTarget for the same reason staleScan does: an entry the validator
// accepts must be an entry every census can read, and two readings of one field
// is the drift all three are written to catch.
//
// Sibling-repo entries (`stratum: src/...`) are indexed under their qualified
// form, so a query has to spell the repo to hit them. That is deliberate --
// a bare `src/block/bdev.c` is ambiguous across trees, and answering it from
// whichever repo happened to sort first is worse than not answering.
func ownerIndex(reg *Registry) map[string][]*Note {
	idx := map[string][]*Note{}
	for _, n := range reg.OfType("sub") {
		for _, c := range n.Front.ListOr("code") {
			if c = strings.TrimSpace(c); c == "" {
				continue
			}
			repo, path := codeTarget(c)
			key := path
			if repo != "" {
				key = repo + ":" + path
			}
			idx[key] = append(idx[key], n)
		}
	}
	return idx
}

// twinOf applies convention 1. Returns "" when the path is not one of the two
// shapes -- an unrecognised shape must produce no twin rather than a guess,
// since a fabricated twin would be reported with the same confidence as a real
// one.
func twinOf(p string) string {
	base := filepath.Base(p)
	switch {
	case strings.HasPrefix(p, "kernel/include/thylacine/") && strings.HasSuffix(p, ".h"):
		return "kernel/" + strings.TrimSuffix(base, ".h") + ".c"
	case strings.HasPrefix(p, "kernel/") && strings.HasSuffix(p, ".c") &&
		!strings.Contains(strings.TrimPrefix(p, "kernel/"), "/"):
		return "kernel/include/thylacine/" + strings.TrimSuffix(base, ".c") + ".h"
	}
	return ""
}

// referencedBy: notes that NAME the path in a frontmatter field other than
// `code:`. Not ownership -- `abi-errno` pins errno.h through `pinned-by:` and
// has swept nothing -- but the cutover decision needs it, because a session
// that adds an errno and is told "unowned, write the reference doc" has just
// been sent to duplicate the errno registry.
//
// The two questions are kept apart rather than merged: "has this been swept?"
// (`code:` on a `sub`, strict, what the coverage ledger counts) and "does the
// vault carry anything about this?" (wider). Merging them is precisely how the
// first coverage ledger rotted -- prose mention counted as coverage.
//
// Every frontmatter key but `code` is scanned rather than a list of the fields
// that seemed relevant. Enumerating a vocabulary you do not control breaks once
// per unlisted term, and these fields are added by schema edits this function
// will never hear about.
func referencedBy(reg *Registry, path string) []string {
	var out []string
	for _, n := range reg.Notes() {
		hit := false
		for _, k := range n.Front.Keys() {
			if k == "code" {
				continue
			}
			for _, v := range n.Front.ListOr(k) {
				i := strings.Index(v, path)
				if i < 0 {
					continue
				}
				// Bound the match so kernel/pipe.c does not match
				// kernel/pipe.cpp, and so a path is not found inside a longer
				// filename ending in the same characters.
				if i > 0 && !strings.ContainsRune(" \t\"'(,:", rune(v[i-1])) {
					continue
				}
				if e := i + len(path); e < len(v) &&
					!strings.ContainsRune(" \t\"')(,:;.", rune(v[e])) {
					continue
				}
				hit = true
				break
			}
			if hit {
				break
			}
		}
		if hit {
			out = append(out, n.ID)
		}
	}
	sort.Strings(out)
	return out
}

func noteIDs(ns []*Note) []string {
	var out []string
	for _, n := range ns {
		out = append(out, n.ID)
	}
	sort.Strings(out)
	return out
}

// staleFor indexes the corpus-wide staleness census by (dossier, file) so an
// OWNED answer can say whether the dossier it is sending you to has seen the
// file since it last changed. Reusing staleScan rather than asking git about
// one file keeps a single definition of "stale" -- a second, cheaper definition
// here would drift from the one the lint gate reports.
func staleFor(root string, reg *Registry) map[string]string {
	out := map[string]string{}
	stale, _, _, _ := staleScan(root, reg)
	for _, s := range stale {
		out[s.note+"\x00"+s.file] = fmt.Sprintf(
			"dossier updated %s, file changed %s (+%d lines since)",
			s.updated, s.changed, s.churn)
	}
	return out
}

// treeFiles lists tracked source files under dir. The two callers want
// different depths and it is not a detail: a FILE query asks "what sits beside
// this one", where recursing turns kernel/ into 200 unrelated rows; a DIRECTORY
// query asks "is this subtree swept", and stopping at depth 1 answered that
// with 0 of 0 for usr/tapestryd/ -- whose sources are all one level down --
// which reads as complete coverage of nothing.
func treeFiles(root, dir string, recurse bool) []string {
	out, err := exec.Command("git", "-C", root, "ls-files", dir).Output()
	if err != nil {
		return nil
	}
	var files []string
	for _, p := range strings.Fields(string(out)) {
		if !recurse && filepath.Dir(p) != dir {
			continue
		}
		if srcRe.MatchString(p) {
			files = append(files, p)
		}
	}
	sort.Strings(files)
	return files
}

func answerOwner(root string, reg *Registry, idx map[string][]*Note,
	stale map[string]string, q string) ownerAnswer {

	// Normalise: accept an absolute path inside the repo, a ./ prefix, and a
	// trailing slash. A caller pasting from a shell or from a table row should
	// not have to know which form the index uses.
	q = strings.TrimSpace(q)
	if abs, err := filepath.Abs(q); err == nil && !strings.Contains(q, ":") {
		if rel, rerr := filepath.Rel(root, abs); rerr == nil &&
			!strings.HasPrefix(rel, "..") && strings.HasPrefix(q, "/") {
			q = rel
		}
	}
	q = strings.TrimPrefix(q, "./")
	isDir := strings.HasSuffix(q, "/")
	q = strings.TrimSuffix(q, "/")
	if fi, err := os.Stat(filepath.Join(root, q)); err == nil && fi.IsDir() {
		isDir = true
	}

	a := ownerAnswer{Path: q, Kind: "file"}
	if isDir {
		a.Kind = "dir"
	}
	if notCodeSurface(q) {
		a.NotCode = true
		return a
	}

	add := func(ns []*Note, claim string) {
		for _, n := range ns {
			h := ownerHit{Note: n.ID, Rel: n.Rel, Audit: n.Front.Str("audit"),
				Updated: n.Front.Str("updated"), Claim: claim,
				Title: n.Front.Str("title")}
			if s, ok := stale[n.ID+"\x00"+claim]; ok {
				h.Stale = s
			}
			a.Owners = append(a.Owners, h)
		}
	}

	if isDir {
		// Every dossier claiming anything under the directory, plus the count
		// of files under it that nobody claims -- a directory answer that said
		// only "yes, 4 dossiers" would hide a half-swept directory, which is
		// the state most likely to produce a wrong cutover decision.
		seen := map[string]bool{}
		for k, ns := range idx {
			if k == q || strings.HasPrefix(k, q+"/") {
				for _, n := range ns {
					if !seen[n.ID] {
						seen[n.ID] = true
						add([]*Note{n}, k)
					}
				}
			}
		}
		for _, f := range treeFiles(root, q, true) {
			a.DirTotal++
			if len(idx[f]) > 0 {
				a.DirOwned++
				continue
			}
			a.UnsweptTot++
			if len(a.Unswept) < 8 {
				a.Unswept = append(a.Unswept, f)
			}
		}
		a.Owned = len(a.Owners) > 0
		a.Covered = a.Owned
		return a
	}

	if ns, ok := idx[q]; ok {
		add(ns, q)
		a.Owned, a.Covered = true, true
		// The pins are computed for an OWNED path too. Ownership answers
		// "where does my prose go?"; a pin is a CO-UPDATE obligation, and the
		// two are orthogonal -- so returning here suppressed exactly the
		// warning the best-covered files need. There were TWO returns on this
		// predicate, this one and one in the report, and each hid the other:
		// removing either alone changes no observable output.
		a.RefBy = referencedBy(reg, q)
		return a
	}

	// Unowned. Everything below exists to stop "no" from being the whole
	// answer -- see the file header.
	if t := twinOf(q); t != "" {
		// Whether the twin EXISTS is reported separately from whether it is
		// owned. The first version printed "twin kernel/errno.c is unowned
		// too" for a file that has never existed -- inventing a path and
		// reporting it as unswept, which is the phantom-path class this whole
		// ledger was built to catch, committed by the ledger's own tool.
		if _, err := os.Stat(filepath.Join(root, t)); err == nil {
			a.TwinExists = true
			if ns, ok := idx[t]; ok {
				a.TwinOwner = noteIDs(ns)
			}
		}
		a.Twin = t
	}
	a.RefBy = referencedBy(reg, q)
	// A reference is a LEAD, never coverage -- and getting this wrong was a
	// false positive exactly as bad as the false UNOWNED the rest of this file
	// defends against, in the opposite direction.
	//
	// The caller's real question at the doc step is "will the prose I am about
	// to write end up somewhere a future reader finds it?". A twin or a
	// directory claim answers yes: a DOSSIER describes that surface, so the
	// prose has a home. A `pinned-by:` on a registry note does not -- it means
	// the file is SPOKEN FOR, not that the surface is DESCRIBED. Both live
	// instances are `abi` notes (abi-errno pins errno.h, abi-boot-banner pins
	// extinction.c): they pin VALUES and STRINGS, and a description of a
	// mechanism has nowhere to go in either.
	//
	// So exit 0 on this route would send a session to the vault to write
	// something the vault cannot hold. The lead is still worth printing loudly
	// -- an errno addition genuinely does need abi-errno updated -- but it is
	// printed BESIDE the "write the reference doc" verdict, not instead of it.
	a.Covered = len(a.TwinOwner) > 0
	dir := filepath.Dir(q)
	if dir != "." {
		a.Dir = dir
		owners := map[string]bool{}
		for _, f := range treeFiles(root, dir, false) {
			a.DirTotal++
			if ns, ok := idx[f]; ok {
				a.DirOwned++
				for _, n := range ns {
					owners[n.ID] = true
				}
			}
		}
		for id := range owners {
			a.DirOwners = append(a.DirOwners, id)
		}
		sort.Strings(a.DirOwners)
	}
	return a
}

func printOwner(a ownerAnswer) {
	if a.NotCode {
		fmt.Printf("%s  NOT A CODE SURFACE\n", a.Path)
		fmt.Printf("  -> this is a document, not a surface. Ask about the CODE it\n")
		fmt.Printf("     describes; step 0 takes the paths your chunk CHANGED.\n")
		fmt.Printf("     Answering UNOWNED here would route you away from the vault\n")
		fmt.Printf("     for a surface a dossier may well carry.\n")
		return
	}
	verdict := "UNOWNED"
	switch {
	case a.Owned:
		verdict = "OWNED"
	case a.Covered:
		verdict = "COVERED (not swept under this path)"
	}
	fmt.Printf("%s  %s\n", a.Path, verdict)
	for _, h := range a.Owners {
		audit := h.Audit
		if audit == "" {
			audit = "-"
		}
		claim := ""
		if h.Claim != a.Path {
			claim = "  via " + h.Claim
		}
		fmt.Printf("  %-34s %s  audit:%-5s updated:%s%s\n",
			h.Note, h.Rel, audit, h.Updated, claim)
		// The title, on its own line, because an id is not a scope. See the
		// ownerHit.Title comment: OWNED means a dossier claims the path, not
		// that it describes the part of the file you are about to write about.
		if h.Title != "" {
			fmt.Printf("  %-34s %q\n", "", h.Title)
		}
		if h.Stale != "" {
			fmt.Printf("  %-34s STALE -- %s\n", "", h.Stale)
		}
	}

	if a.Kind == "dir" {
		if a.DirTotal > 0 {
			fmt.Printf("  %d of %d source files under it are claimed", a.DirOwned, a.DirTotal)
			if a.UnsweptTot > 0 {
				fmt.Printf("; %d unswept: %s", a.UnsweptTot, strings.Join(a.Unswept, " "))
				if a.UnsweptTot > len(a.Unswept) {
					fmt.Printf(" (+%d more)", a.UnsweptTot-len(a.Unswept))
				}
			}
			fmt.Println()
		}
		return
	}
	// The lead prints for OWNED paths too, and that ordering is the fix for a
	// real hole: `if a.Owned { return }` used to sit above this, so the
	// better-covered a file was, the LESS this said about it. A pin is a
	// CO-UPDATE obligation, not a statement about where prose goes -- the two
	// are orthogonal, and only the second is what ownership answers. The worked
	// case: `kernel/main.c` prints the boot banner and is owned by a boot
	// dossier, so the one edit most likely to break the banner ABI reported
	// nothing about it, while unowned `kernel/extinction.c` beside it did.
	if len(a.RefBy) > 0 {
		fmt.Printf("  ALSO named by %s -- a PIN, not a dossier: it cannot hold\n",
			strings.Join(firstNStr(a.RefBy, 4), ", "))
		fmt.Printf("  %-34s a description, and it names a co-update set. Check it TOO.\n", "")
		if len(a.RefBy) > 4 {
			fmt.Printf("  %-34s (+%d more)\n", "", len(a.RefBy)-4)
		}
	}
	if a.Owned {
		return
	}

	if a.Twin != "" {
		switch {
		case len(a.TwinOwner) > 0:
			fmt.Printf("  twin %s is owned by %s -- the same SURFACE; extend that dossier\n",
				a.Twin, strings.Join(a.TwinOwner, ", "))
		case a.TwinExists:
			fmt.Printf("  twin %s exists and is unowned too\n", a.Twin)
		}
	}
	if a.Dir != "" && a.DirTotal > 0 {
		fmt.Printf("  %s: %d of %d files claimed", a.Dir, a.DirOwned, a.DirTotal)
		if len(a.DirOwners) > 0 {
			fmt.Printf(" by %s", strings.Join(firstNStr(a.DirOwners, 4), ", "))
			if len(a.DirOwners) > 4 {
				fmt.Printf(" (+%d more)", len(a.DirOwners)-4)
			}
		}
		fmt.Println()
	}
	// The closing directive must agree with the lines above it. The first
	// version printed "no dossier: write the reference doc" unconditionally --
	// directly under a line saying the twin is owned by sub-kernel-caps. A
	// reader following the last line would have duplicated a swept surface,
	// which is the divergence this command exists to prevent, produced by the
	// command itself.
	switch {
	case len(a.TwinOwner) > 0:
		fmt.Println("  -> the surface IS swept: extend the twin's dossier, not a reference doc.")
	case len(a.RefBy) > 0:
		fmt.Println("  -> no dossier: write the reference doc as today, file the sweep,")
		fmt.Println("     AND check the note above -- it may need the same change.")
	default:
		fmt.Println("  -> no dossier: write the reference doc as today, and file the sweep.")
	}
}

func firstNStr(s []string, n int) []string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func cmdOwner(root string, args []string) int {
	jsonOut := false
	var paths []string
	for _, a := range args {
		if a == "--json" {
			jsonOut = true
			continue
		}
		paths = append(paths, a)
	}
	if len(paths) == 0 {
		fmt.Println("usage: quaestor owner <path>... [--json]")
		return 2
	}
	reg, _ := loadRegistry(root)
	if reg.Len() == 0 {
		// Same fail-closed rule the lint gate learned: an empty registry means
		// the root is wrong, and answering UNOWNED from it would be a
		// confident wrong answer for every path asked.
		fmt.Println("no notes found -- wrong root? an empty registry cannot answer this")
		return 2
	}
	idx := ownerIndex(reg)
	stale := staleFor(root, reg)

	// This command answers from the VAULT WORKTREE's checkout, and its caller
	// runs it from a different tree (CLAUDE.md's doc step: `cd
	// ~/projects/thylacine-vault && ... owner <changed paths>`). Those two
	// trees drift, so an answer can be computed against a tree that has never
	// seen the file being asked about -- and the honest answer there is not
	// UNOWNED, it is "I cannot see that file".
	//
	// Both facts are reported rather than silently absorbed. UNOWNED is the
	// safe verdict either way (it sends the caller to write the reference doc,
	// which is recoverable), but a caller who is told "no dossier" about a file
	// this checkout simply lacks has been given a confident answer to a
	// question that was never evaluated -- the shape this whole protocol was
	// built to stop, reappearing in the tool that enforces it.
	behind := 0
	if out, err := exec.Command("git", "-C", root, "rev-list", "--count",
		"HEAD..main").Output(); err == nil {
		behind = atoiOr0(strings.TrimSpace(string(out)))
	}
	var unseen []string
	for _, p := range paths {
		if strings.Contains(p, ":") {
			continue // sibling-repo form; not resolvable here
		}
		q := strings.TrimSuffix(strings.TrimPrefix(strings.TrimSpace(p), "./"), "/")
		if _, err := os.Stat(filepath.Join(root, q)); err != nil {
			unseen = append(unseen, q)
		}
	}
	if len(unseen) > 0 {
		fmt.Printf("NOT IN THIS CHECKOUT -- the vault worktree cannot see: %s\n",
			strings.Join(unseen, " "))
		fmt.Printf("  The vault is %d commit(s) behind main. Those path(s) were NOT\n", behind)
		fmt.Printf("  evaluated against anything; treat their verdict as UNKNOWN, not\n")
		fmt.Printf("  as \"no dossier\". Sync the vault, or ring vault to sweep them.\n\n")
	} else if behind > 20 {
		fmt.Printf("(note: the vault worktree is %d commits behind main -- verdicts are\n", behind)
		fmt.Printf(" computed against that older tree.)\n\n")
	}

	var answers []ownerAnswer
	allCovered := true
	codePaths := 0
	for _, p := range paths {
		a := answerOwner(root, reg, idx, stale, p)
		answers = append(answers, a)
		if a.NotCode {
			continue // neither covered nor uncovered; it was not a question
		}
		codePaths++
		if !a.Covered {
			allCovered = false
		}
	}
	if jsonOut {
		b, _ := json.MarshalIndent(answers, "", "  ")
		fmt.Println(string(b))
	} else {
		for i, a := range answers {
			if i > 0 {
				fmt.Println()
			}
			printOwner(a)
		}
		// A MIXED answer is the common case and the exit status cannot
		// express it: a chunk usually touches several files, and one
		// unowned path among owned ones drives the whole run to exit 1.
		// A caller following "exit 1 -> write the reference section" then
		// writes one covering every path in the run, INCLUDING the surfaces
		// a dossier already describes -- which is precisely the divergence
		// this command exists to prevent, produced by the command.
		//
		// The exit code is one bit and the question is per-path, so no
		// choice of code fixes this; the answer is to say plainly that both
		// actions are owed. Printed only for multi-path runs, where it is
		// the whole point, and never for one path, where it would be noise.
		if len(answers) > 1 {
			var vault, refdoc, notcode []string
			for _, a := range answers {
				switch {
				case a.NotCode:
					notcode = append(notcode, a.Path)
				case a.Covered:
					vault = append(vault, a.Path)
				default:
					refdoc = append(refdoc, a.Path)
				}
			}
			if len(notcode) > 0 {
				fmt.Printf("\nNOT ANSWERED (documents, not surfaces): %s\n",
					strings.Join(notcode, " "))
				fmt.Printf("  Re-run against the code those describe.\n")
			}
			fmt.Println()
			switch {
			case len(refdoc) == 0:
				fmt.Printf("ALL %d paths are carried by the vault -- no reference section is owed.\n",
					len(vault))
			case len(vault) == 0:
				fmt.Printf("NONE of the %d paths is carried by the vault -- reference section owed for all.\n",
					len(refdoc))
			default:
				fmt.Printf("MIXED -- BOTH actions are owed, and the exit status below says only that the first is:\n")
				fmt.Printf("  reference section owed for: %s\n", strings.Join(refdoc, " "))
				fmt.Printf("  the vault already carries:   %s  <- do NOT write these into a reference doc\n",
					strings.Join(vault, " "))
			}
		}
	}
	if codePaths == 0 {
		// Every path was a document. Neither verdict applies, and returning 1
		// would be read as "no dossier -- write the reference doc", which is
		// the away-from-vault routing this refusal exists to prevent.
		return 2
	}
	if allCovered {
		return 0
	}
	return 1
}
