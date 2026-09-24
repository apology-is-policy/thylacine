# Pointer

Halcyon displays a pointer over its workspace and fullscreen applications. The
pointer follows the mouse or tablet and changes shape over a divider so that
the direction of a resize is visible. Its size follows the display scale.

## In Practice

### Resize a tile

Move the pointer onto the divider between two tiles. A horizontal or vertical
resize pointer indicates the available direction. Press the primary mouse button,
drag the divider, then release it to keep the new layout. Escape cancels a drag.

Moving the pointer over a tile does not change keyboard focus. A click follows
Halcyon's existing focus and application input rules.

### Enter secure attention

Press Ctrl+Alt+F10 to enter the trusted scene. The normal workspace pointer is
removed during the episode. When Halcyon resumes, it restores the pointer at its
current position. The solid trusted backdrop remains independent of application
pixels.

## Technical Details

Tapestry owns pointer position and hit testing. An application can choose an
arrow, text, link, horizontal-resize or vertical-resize shape for its own surface.
The preference takes effect only over that surface. Divider dragging uses the
compositor's resize shape. Application cursor images are not accepted.

Lictor owns the VirtIO cursor plane and uses the same standard raster as the
software composition helper. This keeps the pointer separate from accelerated
application pixels. A failed cursor queue stops pointer updates and prevents a
trusted display acknowledgement; an uncertain hardware operation cannot establish
exclusive display ownership. Raspberry Pi display backends require separate
qualification before these guarantees can be claimed for bare metal.
