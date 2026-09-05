package main

// Parser law. The restricted YAML subset's exact behaviors, pinned so a
// future "improvement" that widens or shifts the grammar fails here
// first (the grammar is schema law -- see front.go's header).

import "testing"

func parseOK(t *testing.T, text string) (*Front, string) {
	t.Helper()
	f, body, ok := parseFront(text)
	if !ok {
		t.Fatalf("parseFront refused valid input:\n%s", text)
	}
	return f, body
}

func TestParserScalarAndQuotes(t *testing.T) {
	f, body := parseOK(t, "---\ntitle: \"Quoted: text\"\nplain: hello\n---\nBODY\n")
	if got := f.Str("title"); got != "Quoted: text" {
		t.Errorf("quoted scalar: got %q", got)
	}
	if got := f.Str("plain"); got != "hello" {
		t.Errorf("plain scalar: got %q", got)
	}
	if body != "BODY\n" {
		t.Errorf("body: got %q", body)
	}
}

func TestParserEmptyValueIsEmptyList(t *testing.T) {
	// LAW: a bare "key:" and "key: []" both parse as the empty list.
	f, _ := parseOK(t, "---\na:\nb: []\n---\nx\n")
	for _, k := range []string{"a", "b"} {
		v, ok := f.Get(k)
		if !ok || !v.IsList || len(v.List) != 0 {
			t.Errorf("%s: want empty list, got %+v", k, v)
		}
	}
}

func TestParserFlowList(t *testing.T) {
	f, _ := parseOK(t, "---\nl: [a, \"b c\", d.e]\n---\nx\n")
	v, _ := f.Get("l")
	want := []string{"a", "b c", "d.e"}
	if !v.IsList || len(v.List) != len(want) {
		t.Fatalf("flow list: got %+v", v)
	}
	for i := range want {
		if v.List[i] != want[i] {
			t.Errorf("flow list[%d]: got %q want %q", i, v.List[i], want[i])
		}
	}
}

func TestParserBlockList(t *testing.T) {
	// LAW (verified against the reference implementation): an UNQUOTED
	// item's trailing comment is stripped; a QUOTED item is returned
	// verbatim -- strip_comment leaves quoted values untouched, so the
	// comment survives INSIDE the value and the quotes stay (the value
	// no longer ends in a quote). Don't "fix" this: quoted items must
	// not carry trailing comments.
	f, _ := parseOK(t, "---\nl:\n  - one\n  - \"two\"  # kept verbatim\n  - three  # stripped\n---\nx\n")
	v, _ := f.Get("l")
	want := []string{"one", "\"two\"  # kept verbatim", "three"}
	if !v.IsList || len(v.List) != len(want) {
		t.Fatalf("block list: got %+v", v)
	}
	for i := range want {
		if v.List[i] != want[i] {
			t.Errorf("block list[%d]: got %q want %q", i, v.List[i], want[i])
		}
	}
}

func TestParserFlowMapKeptRaw(t *testing.T) {
	// LAW: flow maps are presence-only; the raw text is kept as a scalar.
	f, _ := parseOK(t, "---\ncounts: {p0: 0, p1: 1}\n---\nx\n")
	v, _ := f.Get("counts")
	if v.IsList || v.Str != "{p0: 0, p1: 1}" {
		t.Errorf("flow map: got %+v", v)
	}
}

func TestParserCommentRules(t *testing.T) {
	// A comment needs whitespace-#-whitespace; a bare fragment inside a
	// value survives. Quoted/braced/bracketed values are never stripped.
	f, _ := parseOK(t, "---\na: value  # stripped\nb: not#stripped\nc: \"kept  # inside\"\n---\nx\n")
	if got := f.Str("a"); got != "value" {
		t.Errorf("a: got %q", got)
	}
	if got := f.Str("b"); got != "not#stripped" {
		t.Errorf("b: got %q", got)
	}
	if got := f.Str("c"); got != "kept  # inside" {
		t.Errorf("c: got %q", got)
	}
}

func TestParserDuplicateKeyLastWins(t *testing.T) {
	// LAW (the parity gate's dangling-edge probe found this live): a
	// duplicate key's LAST value wins -- an early insert is overwritten.
	f, _ := parseOK(t, "---\nk: first\nk: second\n---\nx\n")
	if got := f.Str("k"); got != "second" {
		t.Errorf("duplicate key: got %q", got)
	}
	if len(f.Keys()) != 1 {
		t.Errorf("duplicate key: %d keys", len(f.Keys()))
	}
}

func TestParserUnterminatedFlowListDegrades(t *testing.T) {
	// LAW: a multi-line flow list is NOT parsed -- the first line lands
	// as a scalar starting "[" (the validate check fails it loudly) and
	// the continuation line is silently dropped.
	f, _ := parseOK(t, "---\nl: [a,\n  b]\n---\nx\n")
	v, _ := f.Get("l")
	if v.IsList || v.Str != "[a," {
		t.Errorf("degraded flow list: got %+v", v)
	}
}

func TestParserTerminators(t *testing.T) {
	// Mid-file "---\n" terminator carries a body; an EOF "---" does not.
	if _, body, ok := parseFront("---\nk: v\n---\nbody"); !ok || body != "body" {
		t.Errorf("mid-file terminator: ok=%v body=%q", ok, body)
	}
	if _, body, ok := parseFront("---\nk: v\n---\n"); !ok || body != "" {
		t.Errorf("EOF terminator with newline: ok=%v body=%q", ok, body)
	}
	if _, _, ok := parseFront("---\nk: v\n"); ok {
		t.Error("unterminated frontmatter accepted")
	}
	if _, _, ok := parseFront("no front at all\n"); ok {
		t.Error("missing frontmatter accepted")
	}
	// "----" (a table rule) is not a terminator.
	if _, _, ok := parseFront("---\nk: v\n----\nnot the end\n"); ok {
		t.Error("'----' accepted as terminator")
	}
}

func TestParserColumnZeroDashIsNotAListItem(t *testing.T) {
	// LAW: a block-list item must be indented; a column-0 "- x" line is
	// neither an item nor a key and is silently skipped.
	f, _ := parseOK(t, "---\nl:\n- zero\n  - indented\n---\nx\n")
	v, _ := f.Get("l")
	if !v.IsList || len(v.List) != 1 || v.List[0] != "indented" {
		t.Errorf("column-0 dash: got %+v", v)
	}
}
