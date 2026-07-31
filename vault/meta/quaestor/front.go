package main

// The restricted YAML-subset frontmatter parser -- a LAW-EXACT port of
// lint.py's parse_front (vault/meta/schema.md section 8). The grammar is
// deliberately small: scalar values, single-line [a, b] flow lists, block
// "- item" lists, flow maps kept raw (presence-only). Multi-line flow
// lists are NOT parsed -- the unterminated-flow-list validate check fails
// them loudly. Do NOT widen this grammar and do NOT swap in a real YAML
// library: the subset is schema law, and a wider parser would silently
// accept notes the law rejects.

import (
	"regexp"
	"strings"
	"unicode"
)

// Value is a parsed frontmatter value: a scalar string, a list of
// strings, or a raw flow-map string (stored as a scalar, presence-only).
type Value struct {
	Str    string
	List   []string
	IsList bool
}

func scalar(s string) Value { return Value{Str: s} }
func list(l []string) Value { return Value{List: l, IsList: true} }
func (v Value) Vals() []string {
	if v.IsList {
		return v.List
	}
	return []string{v.Str}
}

func valueEqual(a, b Value) bool {
	if a.IsList != b.IsList {
		return false
	}
	if a.IsList {
		if len(a.List) != len(b.List) {
			return false
		}
		for i := range a.List {
			if a.List[i] != b.List[i] {
				return false
			}
		}
		return true
	}
	return a.Str == b.Str
}

// Front preserves key insertion order (the reference implementation's
// dict semantics): validation output is deterministic per note.
type Front struct {
	m    map[string]Value
	keys []string
}

func newFront() *Front { return &Front{m: map[string]Value{}} }

func (f *Front) Set(k string, v Value) {
	if _, ok := f.m[k]; !ok {
		f.keys = append(f.keys, k)
	}
	f.m[k] = v
}

func (f *Front) Get(k string) (Value, bool) { v, ok := f.m[k]; return v, ok }
func (f *Front) Has(k string) bool          { _, ok := f.m[k]; return ok }
func (f *Front) Keys() []string             { return f.keys }
func (f *Front) Len() int                   { return len(f.m) }

// Str returns the scalar value, or "" when absent or a list.
func (f *Front) Str(k string) string {
	v, ok := f.m[k]
	if !ok || v.IsList {
		return ""
	}
	return v.Str
}

// ListOr returns the list value, or nil when absent or scalar.
func (f *Front) ListOr(k string) []string {
	v, ok := f.m[k]
	if !ok || !v.IsList {
		return nil
	}
	return v.List
}

func stripQuotes(v string) string {
	v = strings.TrimSpace(v)
	if len(v) >= 2 && v[0] == v[len(v)-1] && (v[0] == '"' || v[0] == '\'') {
		return v[1 : len(v)-1]
	}
	return v
}

var commentRe = regexp.MustCompile(`\s+#\s.*$`)

func stripComment(v string) string {
	if strings.HasPrefix(v, "\"") || strings.HasPrefix(v, "'") ||
		strings.HasPrefix(v, "{") || strings.HasPrefix(v, "[") {
		return v
	}
	return commentRe.ReplaceAllString(v, "")
}

var kvRe = regexp.MustCompile(`^([A-Za-z0-9_-]+):\s*(.*)$`)
var flowSplitRe = regexp.MustCompile(`,\s*`)

// frontBounds finds the terminator of a "---\n...\n---" block: returns
// the index of the "\n---" terminator and whether it is the mid-file
// "\n---\n" form (true) or the EOF "\n---<ws>" form (false). -1 if none.
func frontBounds(text string) (end int, midFile bool) {
	if !strings.HasPrefix(text, "---\n") {
		return -1, false
	}
	end = indexFrom(text, "\n---", 4)
	for end != -1 {
		if end+5 <= len(text) && text[end:end+5] == "\n---\n" {
			return end, true
		}
		if strings.TrimRightFunc(text[end:], unicode.IsSpace) == "\n---" {
			return end, false
		}
		end = indexFrom(text, "\n---", end+1)
	}
	return -1, false
}

func indexFrom(s, sub string, from int) int {
	if from >= len(s) {
		return -1
	}
	i := strings.Index(s[from:], sub)
	if i == -1 {
		return -1
	}
	return from + i
}

// parseFront returns (front, body, ok). ok=false mirrors the reference
// implementation's (None, text) -- missing or unterminated frontmatter.
func parseFront(text string) (*Front, string, bool) {
	end, midFile := frontBounds(text)
	if end == -1 {
		return nil, text, false
	}
	block := text[4:end]
	body := ""
	if midFile {
		body = text[end+5:]
	}
	front := newFront()
	key := ""
	haveKey := false
	for _, raw := range strings.Split(block, "\n") {
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		if len(raw) > 0 && (raw[0] == ' ' || raw[0] == '\t') &&
			strings.HasPrefix(strings.TrimLeftFunc(raw, unicode.IsSpace), "- ") {
			if !haveKey {
				continue
			}
			cur, ok := front.Get(key)
			if !ok || !cur.IsList {
				cur = list(nil)
			}
			item := strings.TrimLeftFunc(raw, unicode.IsSpace)[2:]
			cur.List = append(cur.List, stripQuotes(stripComment(item)))
			front.Set(key, cur)
			continue
		}
		m := kvRe.FindStringSubmatch(raw)
		if m == nil {
			continue
		}
		key = m[1]
		haveKey = true
		val := strings.TrimSpace(stripComment(m[2]))
		switch {
		case val == "" || val == "[]":
			front.Set(key, list(nil))
		case strings.HasPrefix(val, "[") && strings.HasSuffix(val, "]"):
			inner := strings.TrimSpace(val[1 : len(val)-1])
			if inner == "" {
				front.Set(key, list(nil))
			} else {
				parts := flowSplitRe.Split(inner, -1)
				out := make([]string, len(parts))
				for i, p := range parts {
					out[i] = stripQuotes(p)
				}
				front.Set(key, list(out))
			}
		case strings.HasPrefix(val, "{"):
			front.Set(key, scalar(val)) // flow map kept raw; presence-only
		default:
			front.Set(key, scalar(stripQuotes(val)))
		}
	}
	return front, body, true
}
