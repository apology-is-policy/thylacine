# Manual

`manual` reads the Operator's Manual installed at `/manual`. In Halcyon it
emits Beacon headings, emphasis, tables, and code blocks, using the pane's
current typography and theme. On a serial console or through a pipe it
produces plain text.

## In Practice

### Find and read a section

Run `manual` with no operand to list the installed sections. Open a section
by its name, for example:

```sh
manual view
manual gallery
manual remote-files
```

The output belongs to the shell transcript. Use Halcyon's normal scrollback
to revisit it. The reader does not take over the pane or open a separate
pager. A file path can be supplied instead of a section name:

```sh
manual /manual/15-view.md
```

### Select the output form

The default is `--beacon=auto`: rich output is enabled when the destination
and its advertised Beacon tier support it. To produce plain text explicitly:

```sh
manual --beacon=never view
```

`--beacon=always` explicitly selects Beacon output. Use it for a destination
that understands the frames; saving that output to a file preserves the
frames as well as the text.

### Check a section before installing it

```sh
manual --check /manual/15-view.md /manual/16-gallery.md
```

Each section is checked against the same format used by the build. An invalid
section is reported with its line and reason. The reader checks a whole
section before writing its rendered contents, so a later syntax error does
not leave a partially rendered document in the transcript.

## Technical Details

A section is a UTF-8 Markdown file of at most 1 MiB. The supported subset
includes a title, headings, paragraphs, flat lists, fenced code, bounded
tables, code spans, and emphasis. Links, images, raw HTML, nested lists,
character references, and bidirectional controls are rejected. References to
other sections are descriptive prose rather than clickable links.

The reader writes bounded chunks as it renders. Rich and plain output carry
the same text; Beacon frames supply semantic structure while Halcyon chooses
its appearance. Control characters from document content cannot create new
Beacon frames. Plain console output wraps when the console reports a usable
width; piped output keeps each paragraph on one logical line.
