package main

// The MCP layer: newline-delimited JSON-RPC 2.0 over stdio, hand-rolled
// on the stdlib (zero dependencies -- the same reason the parser is a
// hand port: nothing here may drift from the schema authority, and the
// tools are thin wrappers over the exact CLI internals). Implements the
// MCP subset a client needs: initialize / notifications/initialized /
// ping / tools/list / tools/call.

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strings"
)

const serverVersion = "0.1.0"

type rpcRequest struct {
	Jsonrpc string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func writeMsg(w *bufio.Writer, v any) {
	b, _ := json.Marshal(v)
	w.Write(b)
	w.WriteByte('\n')
	w.Flush()
}

func writeResult(w *bufio.Writer, id json.RawMessage, result any) {
	writeMsg(w, map[string]any{"jsonrpc": "2.0", "id": id, "result": result})
}

func writeError(w *bufio.Writer, id json.RawMessage, code int, msg string) {
	writeMsg(w, map[string]any{"jsonrpc": "2.0", "id": id,
		"error": rpcError{code, msg}})
}

type toolDef struct {
	Name        string         `json:"name"`
	Description string         `json:"description"`
	InputSchema map[string]any `json:"inputSchema"`
}

func obj(props map[string]any, required ...string) map[string]any {
	s := map[string]any{"type": "object", "properties": props}
	if len(required) > 0 {
		s["required"] = required
	} else {
		s["required"] = []string{}
	}
	return s
}

func strProp(desc string) map[string]any {
	return map[string]any{"type": "string", "description": desc}
}

var toolDefs = []toolDef{
	{"vault_lint", "Validate the whole vault (schema section 8). Returns the failure/warning lines and the summary.",
		obj(map[string]any{})},
	{"vault_note", "Read a note by id (raw markdown, frontmatter included).",
		obj(map[string]any{"id": strProp("note id, e.g. sub-kernel-ninep-client")}, "id")},
	{"vault_query_findings", "List fnd notes filtered by surface and/or status (fixed|deferred|documented|withdrawn). JSON.",
		obj(map[string]any{"surface": strProp("sub-* id the finding names in `surface`"),
			"status": strProp("finding status filter")})},
	{"vault_query_seams", "List seam notes filtered by status (open|closed). JSON.",
		obj(map[string]any{"status": strProp("seam status filter")})},
	{"vault_backlinks", "Every incoming reference to an id: strict edge fields + body wikilinks. JSON.",
		obj(map[string]any{"id": strProp("target note id")}, "id")},
	{"vault_closed_preamble", "The do-not-re-report preamble for a surface (transclude into prosecutor prompts).",
		obj(map[string]any{"sub_id": strProp("the surface's sub-* id")}, "sub_id")},
	{"vault_new_note", "Create a typed note from its template. Mutating: writes the file; fill remaining placeholders, then lint.",
		obj(map[string]any{
			"type":   strProp("note type (one of the 19)"),
			"id":     strProp("type-prefixed kebab-case id"),
			"title":  strProp("note title"),
			"dir":    strProp("repo-relative directory (required for moc/sub; defaulted for the rest)"),
			"fields": map[string]any{"type": "object", "description": "scalar frontmatter overrides, key -> value"},
		}, "type", "id")},
	{"vault_close_finding", "Flip closure fields on a Record note (fnd: status/fixed-by/regression/seam; dec: status/superseded-by). Mutating; the commit still needs a linking chg-* note.",
		obj(map[string]any{
			"id":            strProp("the fnd-*/dec-* id"),
			"status":        strProp("new status"),
			"fixed_by":      strProp("chg-* that fixed it"),
			"regression":    strProp("regression test name"),
			"seam":          strProp("seam-* carrying a deferral"),
			"superseded_by": strProp("dec-* superseding (dec only)"),
		}, "id")},
	{"vault_render", "Re-render every generated view body. Mutating: rewrites stale views in place.",
		obj(map[string]any{})},
	{"vault_stale", "Dossiers whose `code:` files changed after their `updated:` date, churn-ordered. Dates a change by when it arrived on this branch, not when it was authored.",
		obj(map[string]any{})},
}

func textResult(text string, isErr bool) map[string]any {
	return map[string]any{
		"content": []map[string]any{{"type": "text", "text": text}},
		"isError": isErr,
	}
}

func callTool(root, name string, args map[string]any) (string, bool) {
	str := func(k string) string {
		if v, ok := args[k].(string); ok {
			return v
		}
		return ""
	}
	switch name {
	case "vault_lint":
		reg, pre := loadRegistry(root)
		fails, warns := validate(reg, pre)
		fails = append(fails, checkViews(reg)...)
		// checkCodePaths and the staleness summary belong here because the
		// CLI gate runs them: an MCP lint that is quieter than the hook
		// teaches that the vault is clean when the gate would refuse it.
		fails = append(fails, checkCodePaths(reg)...)
		warns = append(warns, staleSummary(root, reg)...)
		var b strings.Builder
		for _, w := range warns {
			b.WriteString("WARN " + w + "\n")
		}
		for _, f := range fails {
			b.WriteString("FAIL " + f + "\n")
		}
		fmt.Fprintf(&b, "vault-lint: %d notes, %d fail(s), %d warn(s) [mcp]",
			reg.Len(), len(fails), len(warns))
		return b.String(), len(fails) > 0
	case "vault_stale":
		reg, _ := loadRegistry(root)
		if reg.Len() == 0 {
			return "no notes found -- wrong root?", true
		}
		stale, unknown, checked, dossiers := staleScan(root, reg)
		byNote := map[string][]staleHit{}
		var order []string
		sort.Slice(stale, func(i, j int) bool {
			if stale[i].churn != stale[j].churn {
				return stale[i].churn > stale[j].churn
			}
			return stale[i].note < stale[j].note
		})
		for _, h := range stale {
			if _, seen := byNote[h.note]; !seen {
				order = append(order, h.note)
			}
			byNote[h.note] = append(byNote[h.note], h)
		}
		var b strings.Builder
		for _, id := range order {
			hits := byNote[id]
			tot := 0
			for _, h := range hits {
				tot += h.churn
			}
			fmt.Fprintf(&b, "%s  updated=%s  %d file(s)  ~%d lines moved since\n",
				id, hits[0].updated, len(hits), tot)
			for _, h := range hits {
				fmt.Fprintf(&b, "    %s  changed %s  (+/-%d)\n", h.file, h.changed, h.churn)
			}
		}
		if len(order) == 0 {
			b.WriteString("(none)\n")
		}
		fmt.Fprintf(&b, "\nquaestor-stale: %d dossier(s) stale, %d same-day, "+
			"%d code file(s) checked across %d dossier(s)",
			len(order), len(unknown), checked, dossiers)
		return b.String(), false
	case "vault_note":
		reg, _ := loadRegistry(root)
		n, ok := reg.Get(str("id"))
		if !ok {
			return "unknown id '" + str("id") + "'", true
		}
		return n.Raw, false
	case "vault_query_findings":
		reg, _ := loadRegistry(root)
		return queryJSON("fnd", queryFindings(reg, str("surface"), str("status"))), false
	case "vault_query_seams":
		reg, _ := loadRegistry(root)
		return queryJSON("seam", querySeams(reg, str("status"))), false
	case "vault_backlinks":
		reg, _ := loadRegistry(root)
		if !reg.Has(str("id")) {
			return "unknown id '" + str("id") + "'", true
		}
		b, _ := json.MarshalIndent(backlinks(reg, str("id")), "", "  ")
		return string(b), false
	case "vault_closed_preamble":
		reg, _ := loadRegistry(root)
		return renderClosed(reg, str("sub_id")), false
	case "vault_new_note":
		fields := map[string]string{}
		if fo, ok := args["fields"].(map[string]any); ok {
			for k, v := range fo {
				if s, ok := v.(string); ok {
					fields[k] = s
				}
			}
		}
		rel, err := newNote(root, str("type"), str("id"), str("title"),
			str("dir"), fields)
		if err != nil {
			return err.Error(), true
		}
		return "created: " + rel, false
	case "vault_close_finding":
		fields := map[string]string{}
		for argKey, fieldKey := range map[string]string{
			"status": "status", "fixed_by": "fixed-by",
			"regression": "regression", "seam": "seam",
			"superseded_by": "superseded-by"} {
			if v := str(argKey); v != "" {
				fields[fieldKey] = v
			}
		}
		msg, err := closeNote(root, str("id"), fields)
		if err != nil {
			return err.Error(), true
		}
		return msg, false
	case "vault_render":
		reg, _ := loadRegistry(root)
		changed := renderViews(reg)
		if len(changed) == 0 {
			return "all views current", false
		}
		return "rendered: " + strings.Join(changed, ", "), false
	}
	return "unknown tool '" + name + "'", true
}

func cmdServe(root string) int {
	in := bufio.NewReaderSize(os.Stdin, 1<<20)
	out := bufio.NewWriter(os.Stdout)
	for {
		lineBytes, err := in.ReadBytes('\n')
		if len(lineBytes) == 0 && err != nil {
			return 0 // EOF: client closed the pipe
		}
		trimmed := strings.TrimSpace(string(lineBytes))
		if trimmed == "" {
			if err != nil {
				return 0
			}
			continue
		}
		var req rpcRequest
		if jerr := json.Unmarshal([]byte(trimmed), &req); jerr != nil {
			writeError(out, nil, -32700, "parse error: "+jerr.Error())
			continue
		}
		switch req.Method {
		case "initialize":
			var p struct {
				ProtocolVersion string `json:"protocolVersion"`
			}
			_ = json.Unmarshal(req.Params, &p)
			if p.ProtocolVersion == "" {
				p.ProtocolVersion = "2025-06-18"
			}
			writeResult(out, req.ID, map[string]any{
				"protocolVersion": p.ProtocolVersion,
				"capabilities":    map[string]any{"tools": map[string]any{}},
				"serverInfo": map[string]any{
					"name": "quaestor", "version": serverVersion},
			})
		case "notifications/initialized", "notifications/cancelled":
			// notifications: no response
		case "ping":
			writeResult(out, req.ID, map[string]any{})
		case "tools/list":
			writeResult(out, req.ID, map[string]any{"tools": toolDefs})
		case "tools/call":
			var p struct {
				Name      string         `json:"name"`
				Arguments map[string]any `json:"arguments"`
			}
			if jerr := json.Unmarshal(req.Params, &p); jerr != nil {
				writeError(out, req.ID, -32602, "bad params: "+jerr.Error())
				break
			}
			text, isErr := callTool(root, p.Name, p.Arguments)
			writeResult(out, req.ID, textResult(text, isErr))
		default:
			if req.ID != nil {
				writeError(out, req.ID, -32601, "method not found: "+req.Method)
			}
		}
		if err != nil {
			return 0
		}
	}
}
