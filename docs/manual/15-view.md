# View

`view` places an image in the transcript of the Halcyon pane that runs it.
The image stays with the surrounding command output and scrolls with it.
PNG and JPEG files are decoded by the viewer process; files with other
signatures are passed to `cat`. Use Gallery when the image should occupy a
separate graphical surface.

## In Practice

### Display an image beside command output

In a Halcyon shell, run:

```sh
view /path/to/photo.jpg
```

Replace the path with a readable image. Below the image `view` writes a line
such as `view: /path/to/photo.jpg placed inline (640x400)`, and the shell
prompt returns after the image has been delivered. An image that fits the
content column keeps its native size. A wider image is drawn smaller to fit
without changing its aspect ratio. The pane can be scrolled and resized
without reopening the file.

`view` takes one file operand, and `-` reads the image from standard input. A
PNG or JPEG is recognized by its contents, not its filename extension. For a
text file, `view /path/to/notes.txt` has the same output as
`cat /path/to/notes.txt`.

### View a large image

A pane accepts images up to a limited number of pixels. `view` reduces a
larger image to that limit before sending it, keeping its aspect ratio:

```sh
view /path/to/scan.png
```

The line below the image then gives both sizes, such as
`placed inline (1182x886, reduced from 2048x1536)`. The limit is 1 Mi pixels
(1,048,576) on a display of up to about 1920 by 1200 with four panes or fewer.
A larger display or more panes lower it, to no less than 64 Ki pixels. Each
pixel of the reduced image is the average of the pixels it replaces, so a fine
line fades instead of disappearing. An image wider or taller than 8192 pixels
is reduced to 8192 on that side in the same way. An image of more than 3 Mi
pixels, such as a 12-megapixel photograph, is refused before it is decoded;
reduce it on its source system first.

### Diagnose a missing image

A report that says `not displayed`, with the reason in parentheses after it,
means that the image decoded but was not shown, and `view` exits with
status 1. A serial shell has no graphical transcript. Run the command in a
Halcyon pane, or use `gallery /path/to/photo.jpg` when a compositor is
available.

A read or decode failure reports the file and the reason, such as
`view: /tmp/scan.png: png: malformed headers`, and `view` exits with status 1.
An image over the compressed-input or decoded-pixel budget is refused before
the memory to decode it is allocated.

### Check an image from a script

`--check` decodes the file and shows nothing. When the file is a PNG or JPEG
within the decode budget that decodes whole, `view` exits with status 0 and
prints nothing. Otherwise it writes one line giving the reason to standard
error and exits with status 1:

```sh
view --check /path/to/photo.jpg && echo usable
```

`--check` never passes a file to `cat`: a text file is refused with
`not a PNG or JPEG picture`. Lantern checks a deck's pictures this way before
a talk. See Presentations with Lantern.

### Command reference

- `view FILE`: show the image in FILE in this pane, or pass a file that is not
  a PNG or JPEG to `cat`.
- `view --check FILE`: decode the image and show nothing.
- `view --embed FILE`: place the image and write only its reference to
  standard output, for a program to write where the image belongs.

FILE may be `-`, which reads standard input. With `--check` or `--embed`, a
failure is one line on standard error that gives the reason alone, and `cat`
is never run. `--embed` works only in a Halcyon session pane.

| Exit status | Meaning |
|---|---|
| 0 | The image was displayed, checked or placed, or `cat` printed a file that is not an image. |
| 1 | The image could not be read, decoded or displayed. |
| 2 | The arguments are invalid. |

## Technical Details

Each Halcyon pane receives its own media-channel address in `HALCYON_PLACE`.
The address contains a routing token bound to the pane. The compositor checks
the connecting principal and the token, then accepts a bounded raster into
that pane's transcript. Closing the pane removes its route; restarting it
creates a fresh one.

The viewer decodes file bytes in its own process. Halcyon receives dimensions
and pixel data rather than compressed image syntax. The transcript retains
that raster within its scrollback budget. Evicting old transcript content can
therefore remove an old image just as it removes old text.

### Limits

The compressed file limit is 16 MiB. Inline decoding admits at most 3 Mi pixels
for both PNG and JPEG. Gallery has a larger decode budget.

A pane's own limit is smaller, and it changes because the compositor's memory
is fixed: images arriving at once share what the display's glyph cache leaves,
and no image may be larger than a pane's image cache, which the panes share.
Reading the pane's channel returns the current limit as a decimal number of
pixels. `view` reads it after decoding and reduces the image before sending
it. An image sent over the limit is refused, so a limit that falls between the
read and the send, as when a pane opens at that moment, shows as
`not displayed`.

### The reference

`--embed` writes a reference instead of the image: a short Beacon object that
names the placed image and carries the text `image WxH` for anything that
cannot draw it. The program that ran `view` writes the reference into its own
output where the image belongs, and the pane draws the image there. A reference
confers nothing: a pane draws only images placed through its own channel.
