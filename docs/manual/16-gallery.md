# Gallery

`gallery` opens a PNG or JPEG on a separate graphical surface. The image is
centred and scaled to fit the available surface while preserving its aspect
ratio. Empty space is black; transparent image pixels blend over that black
background. Gallery uses Tapestry, the compositor beneath
Halcyon; it does not replace the shell transcript. Use View when an image
belongs among the output of a command.

## In Practice

### Open and dismiss an image

From a graphical shell, run:

```sh
gallery /path/to/photo.jpg
```

The path must name a readable PNG or JPEG, and the compositor must be running.
In Halcyon, the new pane is labelled with Gallery and the image path. Press
Super+F to zoom it across the workspace; press Super+F again to restore the
pane layout. The workspace rails remain available while the image is zoomed.

Press Escape or Q to close the image and return to the shell. Closing the
surface through the workspace also ends the viewer. The original shell
transcript, including any inline images, remains in place.

The viewer repaints when its surface is resized or uncovered. A small image
is enlarged to fit; a large image is reduced. Scaling preserves the aspect
ratio and uses nearest-neighbour sampling.

### Recognize a refused image

Gallery reports a read or decode error and exits if the file is unavailable,
malformed, or too large. Unlike View, it does not send non-image files to
`cat`. A compositor connection or presentation failure also ends the program
with an error.

The compressed file limit is 16 MiB, and the decoded image limit is
12 Mi pixels. Dimensions are checked before decoding, including for
progressive JPEG files whose intermediate storage exceeds their final raster.

## Technical Details

The viewer owns the decoder and the uncompressed image. It creates a Tapestry
surface, maps its pixel buffers, paints the complete frame, and presents it.
The compositor receives pixels rather than compressed file contents.

A still image declares a static frame intent: it needs no continuous animation
clock. Configuration changes trigger a complete repaint so that rotating
buffers cannot expose pixels left from an earlier size or frame. Exiting
releases the surface and its mappings.
