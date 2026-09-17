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

Replace the path with a readable image. An image that fits the content column
keeps its native size. A wider image is reduced to fit without changing its
aspect ratio. The shell prompt returns after the raster has been delivered.
The pane can be scrolled and resized without reopening the file.

`view` takes one file operand. A PNG or JPEG is recognized by its contents,
not its filename extension. For a text file, `view /path/to/notes.txt` has the
same output as `cat /path/to/notes.txt`.

### Diagnose a missing image

A decoded-image report ending in `not displayed` means that decoding succeeded
but the Halcyon media channel was unavailable or refused the raster. A serial
shell has no graphical transcript. Run the command in a Halcyon pane, or use
`gallery /path/to/photo.jpg` when a compositor is available.

A read or decode failure reports the file and reason. Images exceeding the
compressed-input or decoded-pixel budget are refused before the expensive
allocation. A large photograph can be reduced on its source system before
viewing it inline.

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

The compressed file limit is 16 MiB. Inline decoding admits at most 3 Mi pixels
for both PNG and JPEG; the receiving compositor can impose a
smaller display-dependent limit. Gallery has a larger decode budget.
