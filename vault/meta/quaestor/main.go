package main

// quaestor -- the vault's registrar (vault/meta/schema.md section 8).
// The one schema authority: lint (the pre-commit gate), the typed note
// factory, queries, closure flips, view rendering, and the MCP layer.
// Naming: vault/glossary/gls-quaestor.md.

import (
	"fmt"
	"os"
)

const usage = `quaestor -- the vault registrar (vault/meta/schema.md section 8)

  quaestor lint [--staged]      validate the vault (pre-commit gate with --staged)
  quaestor render               rewrite generated view bodies, then validate
  quaestor closed <sub-id>      print the do-not-re-report preamble for a surface
  quaestor new <type> <id> [--title T] [--dir D] [--set k=v]...
                                create a typed note from its template
  quaestor query <fnd|seam> [--surface S] [--status ST] [--json]
  quaestor backlinks <id> [--json]
  quaestor close <id> --status S [--fixed-by C] [--regression R] [--seam S]
  quaestor id <candidate>       check an id for shape + collisions
  quaestor owner <path>... [--json]
                                does the vault carry this surface? (the
                                per-surface cutover test) exit 0 = go to the
                                vault, 1 = write the reference doc
  quaestor stale [--all] [--json]
                                dossiers whose code changed after 'updated:'
  quaestor serve                MCP server on stdio (newline-delimited JSON-RPC)

  --root DIR (any command)      explicit repo root -- hooks pass it (relative
                                GIT_DIR + go -C defeat git-based discovery)
`

func lintRun(root, mode string) int {
	reg, preErrors := loadRegistry(root)
	if mode == "--render" {
		for _, c := range renderViews(reg) {
			fmt.Println("rendered: " + c)
		}
		reg, preErrors = loadRegistry(root)
	}
	fails, warns := validate(reg, preErrors)
	fails = append(fails, checkViews(reg)...)
	fails = append(fails, checkCodePaths(reg)...)
	fails = append(fails, checkAbiLiterals(root, reg)...)
	if reg.Len() == 0 {
		// FAIL CLOSED: an empty registry means the root is wrong or the
		// vault is gone -- a gate that validates nothing must not pass
		// (the first live hook run passed open exactly this way).
		fails = append(fails, fmt.Sprintf(
			"no notes found under %s/vault -- wrong root? "+
				"an empty registry never passes", root))
	}
	if mode == "--staged" {
		sf, sw := stagedChecks(root, reg)
		fails = append(fails, sf...)
		warns = append(warns, sw...)
	}
	// A WARN, never a FAIL. Staleness is a property of the world moving,
	// not of the commit in hand: a merge that brings in 88 upstream commits
	// makes dozens of dossiers stale at once, and a gate that refuses the
	// merge commit recording that fact would be unusable and would be
	// switched off. One line, so it stays legible next to real failures.
	warns = append(warns, staleSummary(root, reg)...)
	for _, w := range warns {
		fmt.Println("WARN " + w)
	}
	for _, f := range fails {
		fmt.Println("FAIL " + f)
	}
	fmt.Printf("vault-lint: %d notes, %d fail(s), %d warn(s) [%s]\n",
		reg.Len(), len(fails), len(warns), mode)
	if len(fails) > 0 {
		return 1
	}
	return 0
}

func main() {
	// Global --root DIR (stripped wherever it appears): the pre-commit
	// hook passes it explicitly because git-hook context (relative
	// GIT_DIR + the `go -C` cwd) defeats git-based root discovery.
	rootFlag := ""
	var args []string
	raw := os.Args[1:]
	for i := 0; i < len(raw); i++ {
		if raw[i] == "--root" && i+1 < len(raw) {
			rootFlag = raw[i+1]
			i++
			continue
		}
		args = append(args, raw[i])
	}
	root := repoRoot(rootFlag)
	if len(args) == 0 {
		fmt.Print(usage)
		os.Exit(2)
	}
	cmd, rest := args[0], args[1:]
	code := 0
	switch cmd {
	case "lint":
		mode := "--all"
		if len(rest) > 0 && rest[0] == "--staged" {
			mode = "--staged"
		}
		code = lintRun(root, mode)
	case "render":
		code = lintRun(root, "--render")
	case "closed":
		if len(rest) < 1 {
			fmt.Println("usage: quaestor closed <sub-id>")
			os.Exit(2)
		}
		reg, _ := loadRegistry(root)
		fmt.Printf("# Do-not-re-report preamble for %s\n\n", rest[0])
		fmt.Println(renderClosed(reg, rest[0]))
	case "new":
		code = cmdNew(root, rest)
	case "query":
		code = cmdQuery(root, rest)
	case "backlinks":
		code = cmdBacklinks(root, rest)
	case "close":
		code = cmdClose(root, rest)
	case "id":
		code = cmdID(root, rest)
	case "owner":
		code = cmdOwner(root, rest)
	case "stale":
		code = cmdStale(root, rest)
	case "serve":
		code = cmdServe(root)
	default:
		fmt.Print(usage)
		os.Exit(2)
	}
	os.Exit(code)
}
