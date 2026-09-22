# How it works

The presenter writes one slide, then clears and writes the next. The clear is a
grid operation, so the tile keeps laying the document out richly.

## What it does not do

It never enters the alternate screen. That is the one mode in which a tile
paints its raw character grid instead of the rich document, which would throw
away the rendering the whole exercise is for.
