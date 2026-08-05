package main

// `quaestor stale` -- does a dossier still describe its code?
//
// The coverage ledger answers "does every file have an owner". It says so
// itself that it cannot answer the next question: whether a dossier that
// genuinely covered a file still covers it after the file changes. This is
// that question, and it is computable from git because a dossier states
// both halves in its own frontmatter -- `code:` names the files, `updated:`
// dates the reading.
//
// Why it lives here rather than in a script. The first attempt at this
// census was an ad-hoc regex over frontmatter, and it silently skipped
// every dossier whose `code:` is written in flow style -- 42 of 112, 38% of
// the corpus -- because the regex only matched the block form. It then
// reported a confident count from the 63% it could see. quaestor's parser
// reads both forms because the lint gate depends on it, so the census has
// to run on the parser rather than beside it.
//
// The three-way verdict is deliberate. `updated:` is a date and a commit is
// a timestamp, so a commit landing the same day the dossier was written is
// genuinely UNKNOWN -- the sweeper may have read that change or may have
// been an hour ahead of it. Forcing it into "current" hides real staleness;
// forcing it into "stale" cries wolf on every same-day sweep, which is the
// common case. Reporting it as its own class is the only honest option, and
// it keeps the STALE count meaning exactly one thing.

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// edit is one dated change to one path, with its churn. Churn is carried
// because a staleness list without magnitude is an alarm rather than a
// work order: a dossier whose file gained a comment and one whose file
// gained four new resolver gates are not the same problem, and 45 of them
// have to be triaged in some order.
type edit struct {
	date  string
	churn int // added + deleted
}

type history struct {
	last  map[string]string // path -> most recent change date on this branch
	edits map[string][]edit // path -> every dated change, newest first
}

// lastTouched reads the whole branch history once: for each repo-relative
// path, the date it last changed ON THIS BRANCH and every dated change
// with its churn. One `git log` walk rather than one per file -- at ~440
// owned files the per-file form is 440 processes.
//
// The walk is newest-first, so the FIRST sighting of a path is its most
// recent change; later (older) sightings must not overwrite it.
//
// --first-parent is the whole correctness of this function, and the first
// version without it was wrong. A commit's own date is when it was
// WRITTEN, which on a branch that merges is not when it became true here:
// the stalk resolver's four new gates are dated 2026-07-30 on the branch
// they were authored on, arrived here on 2026-08-05, and the dossier that
// describes that file was written on 2026-08-01 -- inside the gap. Against
// the commit date the dossier looks current, because it is newer than a
// change it could not possibly have seen. Walking first parents attributes
// each change to the merge that carried it in, which is the only date a
// reader of this branch could have known.
//
// `since` bounds the walk: a change older than EVERY dossier cannot make
// any dossier stale, so history before the oldest `updated:` is dead
// weight. Passing the oldest date takes the whole-history walk from ~3.6s
// to well under the lint gate's budget, and it is sound rather than
// approximate -- a path dropped by the bound is a path whose last change
// predates every reader, which is precisely "not stale" for all of them.
func lastTouched(repo, since string) (*history, error) {
	args := []string{"log", "--first-parent",
		"--diff-merges=first-parent", "--pretty=format:%x01%cI",
		"--numstat", "--no-renames"}
	if since != "" {
		args = append(args, "--since="+since)
	}
	cmd := exec.Command("git", args...)
	cmd.Dir = repo
	out, err := cmd.Output()
	if err != nil {
		return nil, err
	}
	h := &history{last: map[string]string{}, edits: map[string][]edit{}}
	date := ""
	for _, line := range strings.Split(string(out), "\n") {
		if strings.HasPrefix(line, "\x01") {
			date = line[1:]
			if i := strings.IndexByte(date, 'T'); i > 0 {
				date = date[:i]
			}
			continue
		}
		line = strings.TrimRight(line, "\r")
		if line == "" || date == "" {
			continue
		}
		// numstat: "<added>\t<deleted>\t<path>"; a binary file reports "-".
		parts := strings.SplitN(line, "\t", 3)
		if len(parts) != 3 {
			continue
		}
		p := strings.TrimSpace(parts[2])
		if p == "" {
			continue
		}
		churn := atoiOr0(parts[0]) + atoiOr0(parts[1])
		if _, seen := h.last[p]; !seen {
			h.last[p] = date
		}
		h.edits[p] = append(h.edits[p], edit{date, churn})
	}
	return h, nil
}

func atoiOr0(s string) int {
	n := 0
	for _, c := range strings.TrimSpace(s) {
		if c < '0' || c > '9' {
			return 0 // "-" for binary, or anything unexpected
		}
		n = n*10 + int(c-'0')
	}
	return n
}

// churnSince sums the churn of every change to path strictly after `after`.
// Strictly after, matching the STALE verdict: a same-day change is the
// unknown class and is not counted as churn the dossier missed.
func (h *history) churnSince(path, after string) int {
	total := 0
	for _, e := range h.edits[path] {
		if e.date > after {
			total += e.churn
		}
	}
	return total
}

type staleHit struct {
	note    string // dossier id
	rel     string // dossier path, for the report
	updated string
	file    string
	changed string
	churn   int
	unknown bool // same-day: ambiguous, not counted as stale
}

// codeTarget strips a `code:` entry down to (repo, path). It mirrors
// checkCodePaths' resolution deliberately: an entry that validator accepts
// is an entry this census must be able to read, and two different readings
// of one field is the drift both are written to catch.
func codeTarget(entry string) (repo, path string) {
	if i := strings.Index(entry, ":"); i > 0 {
		repo = strings.TrimSpace(entry[:i])
		entry = strings.TrimSpace(entry[i+1:])
	}
	if i := strings.IndexAny(entry, " \t"); i > 0 {
		entry = entry[:i]
	}
	return repo, entry
}

// staleScan is the census proper, shared by the command and by the lint
// gate's one-line summary. Returning both classes keeps the caller from
// re-deriving the same/after distinction and getting it differently.
func staleScan(root string, reg *Registry) (stale, unknown []staleHit, checked, dossiers int) {
	// The oldest `updated:` in the corpus bounds every walk (see
	// lastTouched). Computed before any git call so the bound is the
	// corpus's, not one dossier's.
	oldest := ""
	for _, n := range reg.OfType("sub") {
		if u := n.Front.Str("updated"); u != "" && (oldest == "" || u < oldest) {
			oldest = u
		}
	}

	// Per-repo history, loaded lazily: the sibling trees are large and most
	// runs never name one.
	hist := map[string]*history{}
	histFor := func(repo string) *history {
		if h, ok := hist[repo]; ok {
			return h
		}
		dir := root
		if repo != "" {
			parent := filepath.Dir(filepath.Clean(root))
			switch repo {
			case "stratum":
				dir = filepath.Join(parent, "stratum", "v2")
			default:
				hist[repo] = nil
				return nil
			}
			if _, err := os.Stat(dir); err != nil {
				hist[repo] = nil // not checked out; nothing to compare
				return nil
			}
		}
		h, err := lastTouched(dir, oldest)
		if err != nil {
			h = nil
		}
		hist[repo] = h
		return h
	}

	for _, n := range reg.OfType("sub") {
		upd := n.Front.Str("updated")
		codes := n.Front.ListOr("code")
		if len(codes) == 0 {
			continue
		}
		dossiers++
		for _, c := range codes {
			if c = strings.TrimSpace(c); c == "" {
				continue
			}
			repo, path := codeTarget(c)
			h := histFor(repo)
			if h == nil {
				continue
			}
			checked++
			when, ok := h.last[path]
			if !ok {
				// Outside the walk's window (its last change predates every
				// dossier, so it is stale for none), or never committed, or
				// path rot -- which lint owns. Counted as checked either way:
				// the answer is "not stale", and moving this below the lookup
				// would silently redefine the reported total as "files that
				// changed recently".
				continue
			}
			if upd == "" {
				continue
			}
			switch {
			case when > upd:
				stale = append(stale, staleHit{n.ID, n.Rel, upd, path, when,
					h.churnSince(path, upd), false})
			case when == upd:
				unknown = append(unknown, staleHit{n.ID, n.Rel, upd, path, when, 0, true})
			}
		}
	}

	// Churn-first: the list is a triage order, and 45 entries sorted by date
	// buries the +231-line resolver rewrite under a dozen one-line touches.
	return stale, unknown, checked, dossiers
}

// staleSummary is the lint gate's view: ONE line or none. Forty-five
// separate warnings would train a reader to scroll past the block, which
// is how a warning stops being one.
func staleSummary(root string, reg *Registry) []string {
	stale, _, _, _ := staleScan(root, reg)
	if len(stale) == 0 {
		return nil
	}
	seen := map[string]bool{}
	for _, s := range stale {
		seen[s.note] = true
	}
	return []string{fmt.Sprintf(
		"%d dossier(s) describe code that has changed since they were "+
			"written -- run `quaestor stale` for the churn-ordered list", len(seen))}
}

func cmdStale(root string, args []string) int {
	jsonOut := false
	verbose := false
	for _, a := range args {
		switch a {
		case "--json":
			jsonOut = true
		case "--all":
			verbose = true
		}
	}

	reg, _ := loadRegistry(root)
	if reg.Len() == 0 {
		fmt.Println("no notes found -- wrong root?")
		return 1
	}
	stale, unknown, checked, dossiers := staleScan(root, reg)

	// Churn-first: the list is a triage order, and 45 entries sorted by date
	// buries the +231-line resolver rewrite under a dozen one-line touches.
	sort.Slice(stale, func(i, j int) bool {
		if stale[i].churn != stale[j].churn {
			return stale[i].churn > stale[j].churn
		}
		return stale[i].note < stale[j].note
	})
	sort.Slice(unknown, func(i, j int) bool { return unknown[i].note < unknown[j].note })

	if jsonOut {
		fmt.Println("[")
		for i, s := range stale {
			comma := ","
			if i == len(stale)-1 {
				comma = ""
			}
			fmt.Printf("  {\"note\":%q,\"file\":%q,\"updated\":%q,\"changed\":%q,\"churn\":%d}%s\n",
				s.note, s.file, s.updated, s.changed, s.churn, comma)
		}
		fmt.Println("]")
		if len(stale) > 0 {
			return 1
		}
		return 0
	}

	// Group by dossier: the actionable unit is "this dossier needs re-reading",
	// not "this file changed".
	byNote := map[string][]staleHit{}
	var order []string
	for _, s := range stale {
		if _, seen := byNote[s.note]; !seen {
			order = append(order, s.note)
		}
		byNote[s.note] = append(byNote[s.note], s)
	}

	fmt.Printf("# Dossiers whose code changed after they were written\n\n")
	for _, id := range order {
		hits := byNote[id]
		tot := 0
		for _, h := range hits {
			tot += h.churn
		}
		fmt.Printf("%-42s updated=%s  %d file(s)  ~%d lines moved since\n",
			id, hits[0].updated, len(hits), tot)
		for _, h := range hits {
			fmt.Printf("    %-52s changed %s  (+/-%d)\n", h.file, h.changed, h.churn)
		}
	}
	if len(order) == 0 {
		fmt.Println("(none)")
	}

	if len(unknown) > 0 {
		fmt.Printf("\n# Same-day, so unverifiable either way (%d)\n\n", len(unknown))
		if verbose {
			for _, u := range unknown {
				fmt.Printf("%-42s %s on %s\n", u.note, u.file, u.changed)
			}
		} else {
			seen := map[string]bool{}
			var names []string
			for _, u := range unknown {
				if !seen[u.note] {
					seen[u.note] = true
					names = append(names, u.note)
				}
			}
			fmt.Println(strings.Join(names, ", "))
			fmt.Println("\n(--all lists the files)")
		}
	}

	fmt.Printf("\nquaestor-stale: %d dossier(s) stale, %d same-day, "+
		"%d code file(s) checked across %d dossier(s)\n",
		len(order), len(unknown), checked, dossiers)
	if len(stale) > 0 {
		return 1
	}
	return 0
}
