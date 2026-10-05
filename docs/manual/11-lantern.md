# Presentations with Lantern

`lantern` presents a directory of slides one slide at a time. A slide is a
Markdown file written in the subset the Operator's Manual uses, or a PNG or
JPEG picture, and a manifest named `slides.toml` lists the slides in the order
they are shown. In a Halcyon tile a slide is drawn as a formatted document,
with its headings, lists and tables set in the tile's typography, and a picture
slide shows the picture; on a serial console the same text appears without
formatting; and when standard output or standard input is not a terminal,
lantern writes every slide once, in order, without reading the keyboard.

Use lantern to give a talk from a Thylacine display, to rehearse one, or to
check a deck before presenting it. Lantern controls which slide is shown; the
typeface, the theme and the display scale belong to Halcyon, and a deck cannot
change them. Because every slide is read again each time it
is shown, a deck can be edited during a rehearsal, including a deck read from
another machine over a Haul mount.

## In Practice

### Present the demo deck

Every image installs a four-slide deck at `/deck`, whose last slide is a
picture. In a Halcyon shell, run:

```sh
lantern /deck
```

Lantern clears the tile and shows the first slide at its top, followed by a
footer that gives the deck's title and the slide's position, such as
`Beacon slides  ·  1 / 4`. Space, the right arrow or `n` shows the next slide,
and the left arrow or `p` shows the previous one. The caret is hidden while a
slide is on the screen. `q` ends the presentation: the last slide stays on the
screen, and the shell prompt returns below it.

Advancing past the last slide keeps the last slide on the screen; lantern
neither wraps to the first slide nor exits, because the end of a deck is where
a talk stops for questions. A digit shows the slide with that number, and a
digit larger than the number of slides is ignored. Escape does not end a
presentation. Every key lantern understands is listed under Keys.

### Present to a room

The size of a slide's text follows the tile and the display scale, both of
which are set in Halcyon. Before starting the deck, zoom the tile to the whole
workspace with Super+F, then enlarge the display with Super+=, which adds 25
percent per press up to 200 percent:

```sh
lantern /deck
```

At 200 percent a slide's text is drawn at twice the size it has at 100
percent. The scale applies to the whole display, including every other tile.
Super+0 restores the scale measured from the display, and Super+F again
returns the tile to its place. See Halcyon.

### Write a deck

A deck is a directory that holds `slides.toml` and the slide files. The
manifest lists the slides in order and can give the deck a title:

```toml
title = "Quarterly review"
slides = ["01-title.md", "02-results.md", "03-plans.md"]
```

A text slide begins with a title line such as `# Results`. After the title it
can contain headings, paragraphs, bulleted and numbered lists, tables, code
blocks and block quotes, with code spans and emphasis in the text. Links,
images, raw HTML and nested lists are rejected, exactly as they are in a manual
section; a picture is a slide of its own, as Show a picture describes.
The manual's rule that a section's title does not repeat the number in its file
name does not apply to slides. See Manual.

A block quote sets a passage apart. Each of its lines begins with `>`, and it
holds paragraphs and lists. In a Halcyon tile the passage is framed, on a
console that reports its width it is drawn inside a box, and through a pipe it
is plain text. A slide whose content after its title is one block quote is
therefore shown as a boxed slide:

```md
# Results

> Revenue rose in every region.
>
> - Orders shipped within a day: 98 percent
> - Returns: down by a third
```

Check a deck before presenting it:

```sh
lantern --check /path/to/deck
```

When every slide can be shown, lantern prints a line such as
`lantern: 3 slides, all valid` and exits with status 0. Otherwise it reports
each problem with the file and line concerned, then a count such as
`lantern: 1 of 3 slides cannot be shown`, and exits with status 1. Presenting
performs the same check before the first slide appears, so a deck that starts
can be shown to its end.

Slides are read from their files each time they are drawn. An edit saved
during a rehearsal therefore appears the next time its slide is shown, or at
once after Ctrl-L. If an edit makes an open slide invalid, the slide shows the
reasons in place of its content and the presentation continues.

### Show a picture

A slide can be a PNG or JPEG picture instead of Markdown. Put the file in the
deck's directory and name it in the manifest like any other slide:

```toml
slides = ["01-title.md", "02-architecture.png", "03-plans.md"]
```

In a Halcyon session tile the slide shows the picture, followed by the footer.
A picture larger than the tile accepts is reduced first, keeping its aspect
ratio, as View describes. On a serial console, through a pipe, and in any
other terminal, the slide shows the picture's file name in its place, set
apart as a block quote is:

```text
Picture: 02-architecture.png
```

When a tile cannot show the picture, the slide gives the reason below the
name, and the presentation continues. `lantern --check` decodes every picture
with `view --check`, so a picture that cannot be shown is reported before the
talk:

```text
lantern: 02-architecture.png: not a PNG or JPEG picture
```

Lantern never decodes a picture itself: `view` checks it and places it on the
slide, each time in a process of its own. See View.

### Present a deck from another machine

A deck kept on another computer can be presented without copying it. Run an
npxf server that exports the directory containing the deck, and copy its token
into Thylacine, as Remote Files with Haul describes. Then, in a Halcyon tile,
start Haul with a shell as its command, and run lantern in that shell:

```sh
mkdir -p /tmp/remote
haul -t /path/to/token 10.0.2.2!5640 /tmp/remote /bin/ut
lantern /tmp/remote/deck
```

Replace the token path, the server's address and the deck's directory with
your own. The mount at `/tmp/remote` exists only for Haul and the programs it
starts, so lantern must be started from the shell that Haul runs; leaving that
shell with `exit` removes the mount and ends Haul. Starting lantern as Haul's
command, as in `haul ... /tmp/remote lantern /tmp/remote/deck`, does not work:
the shell switches the terminal to key-at-a-time input only for a program it
starts itself and recognises by name, so lantern would receive its keys a line
at a time. On the serial console the recipe does not work either: the
console's shell gives an ordinary command no keyboard input, so the shell that
Haul starts there reads the end of its input at once and exits, removing the
mount.

A slide edited on the other computer is read again when it is next drawn;
Ctrl-L redraws the current slide.

### Print a deck

When standard output is a pipe or a file, lantern writes every slide once, in
the manifest's order and each with its footer, and exits without reading the
keyboard or clearing anything; the same happens when standard input is not a
terminal:

```sh
lantern /deck | cat
lantern /deck > /tmp/deck.txt
```

The output is plain text. `--beacon=always` writes the markup of the Beacon
tier that the `BEACON` environment variable names even into a pipe or a file,
for a destination that renders it, and `--no-footer` omits the footers.

### Keys

| Key | Action |
|---|---|
| Space, Right, Down, Page Down, Enter, `n`, `j`, `l` | Show the next slide |
| Left, Up, Page Up, Backspace, `p`, `k`, `h` | Show the previous slide |
| Home or `g` | Show the first slide |
| End or `G` | Show the last slide |
| `1` to `9` | Show that slide, if the deck has it |
| Ctrl-L | Read the current slide again and redraw it |
| `q`, Ctrl-C, Ctrl-D | End the presentation |

Every other key is ignored, so a key pressed by mistake cannot end a talk.

### Command reference

- `lantern [--beacon=auto|always|never] [--no-footer] DIR`: present the deck in
  the directory DIR, or print it when standard output is not a terminal.
- `lantern --check DIR`: check the deck and exit without presenting it.
- `lantern --help`: print the usage.

`--beacon` selects the output form as it does for `manual`: `auto`, the
default, writes formatted output only to a terminal that renders it. Exactly
one directory must be given.

| Exit status | Meaning |
|---|---|
| 0 | The deck was presented, printed or checked, or `--help` printed the usage. |
| 1 | The manifest or a slide could not be read or is invalid, output could not be written, or reading the keyboard failed. |
| 2 | The arguments are invalid. |

### The manifest

`slides.toml` is a flat list of `key = value` lines, and a `#` outside quotes
begins a comment. The file is at most 64 KiB and sets only two keys:

- `slides` (required): an array of one to 64 slide file names in double
  quotes, in presentation order.
- `title` (optional): a string in double quotes, shown in each slide's footer.

Every other key is refused, including `scale`, `theme` and `font`, so a deck
cannot change the display it is shown on. A `[table]` header, a key set twice,
a slide named twice and a slide name that contains `/` or `\`, begins with `.`
or `-`, or does not end in `.md`, `.png`, `.jpg` or `.jpeg` are refused as
well. The ending is matched exactly, so `photo.PNG` is refused. A refusal
names the manifest and the line:

```text
lantern: /tmp/talk/slides.toml:2: a manifest sets only 'slides' and, optionally, 'title'
```

The manifest and every slide must be regular files in the deck's directory. A
symbolic link is refused, even one that names another file of the same deck,
so that a deck written by someone else cannot show a file from outside its
directory, such as one of the presenter's own:

```text
lantern: /tmp/talk/notes.md: a link; a deck's files are files in its directory
```

The deck's directory, and each directory above it, is used as named, links
included. On a Haul mount, a link in that path is set by the machine that
serves it and can lead to another directory on this one.

## Technical Details

### Output forms

Lantern asks the kernel for the device class of its standard output and of its
standard input separately, because the two can differ: in
`lantern deck | tee log` standard input is a terminal and standard output is a
pipe.

| Standard output | Standard input | Behaviour |
|---|---|---|
| A terminal that renders Beacon | A terminal | Formatted slides, one at a time, read from the keyboard |
| A terminal that does not render Beacon | A terminal | Plain slides, one at a time, read from the keyboard |
| A terminal | A pipe or a file | Every slide once, formatted as that terminal renders, with no clearing and no keyboard |
| A pipe or a file | Anything | Every slide once, plain unless `--beacon=always` is given, with no clearing and no keyboard |

A terminal renders Beacon when it is a Halcyon tile or the console under a
renderer that advertises the rich tier. The decision uses the same rules as
`manual`: the `BEACON` environment variable, the device class of standard
output, and the `--beacon` option. A picture slide shows its picture only in a
Halcyon session tile; in every other case it shows its file name.

### Clearing the tile

Before each slide lantern erases the whole screen. In a Halcyon tile the
erased screen is moved into the tile's history, and the view is pinned so that
the next slide starts at the top of the tile, clear of the history above it.
After a talk, the tile's Normal mode scrolls back through the slides in the
order they were shown. Lantern never switches to the alternate screen that
full-screen programs use, because Halcyon draws that screen as a plain
character grid and the slide's formatting would be lost.

### One slide per frame

A slide is read and formatted in memory before anything is written, so a slow
read, such as one over a network mount, delays the change instead of showing a
blank screen. Lantern then writes the clear, the slide and its footer in one
write, enclosed in the synchronized-output marks of DEC private mode 2026. A
Halcyon tile holds any paint that falls due inside the frame until the closing
mark, for at most 150 milliseconds from the first paint it holds, so neither
the blank screen nor a partly drawn slide is shown when the slide arrives
within that time. The limit keeps a tile from
waiting indefinitely for a closing mark that never comes. A terminal that does
not recognise the mode ignores the marks.

### Picture slides

A picture slide is placed before its frame is written. Lantern runs
`view --embed` with the picture's file as its standard input and reads back
the reference that `view` writes, then writes the clear, the reference and the
footer in one write, as it does a text slide. The picture is therefore decoded
and sent while the previous slide is still on the screen, and a slow decode
delays the change instead of showing a blank slide. If `view` writes nothing
and does not exit for 30 seconds, lantern stops it and shows the file name
with that reason.

Lantern runs `view` while presenting only when the terminal renders Beacon
richly. In a Halcyon session tile the picture is shown. On a console that
Halcyon renders, which has no channel for placed pictures, `view` refuses at
once, and the slide shows the file name with that reason. In every other
terminal lantern shows the file name without running `view`.

### Keyboard input

When the shell starts lantern, it recognises the program by name and switches
the terminal to raw input, which delivers each key as it is pressed. In this
mode the terminal also stops converting line endings, so lantern converts
them itself. Escape begins the byte sequence of every arrow and function key,
and a lone Escape could only be told apart from those sequences by waiting;
this is why Escape does not end a presentation.

### Checking and memory

Lantern reads the manifest, then reads and checks every slide before it shows
the first, and keeps no slide in memory once it has been checked. A picture is
checked by `view --check`, which decodes it in its own process. While a deck
is open, lantern holds only the slide being shown, whatever the number of
slides, and reads it again from its file each time it is drawn. This is what
makes an edit, local or remote, visible at the next redraw.
