# Halcyon

Halcyon is Thylacine's graphical environment. When a user logs in on a system
configured for it, a Halcyon session fills the display with tiles. Most tiles
are terminals running the user's shell; a graphical program such as `gallery`
shows its surface in a tile of its own. In a terminal tile, the shell's
prompts and the commands typed at them are set in proportional type, as is
output that a program marks up with Beacon, such as the tables of `ps` and
the file names of `ls`, which is drawn with its headings, tables and
emphasis. Other output keeps the fixed-width columns of an ordinary terminal
and is drawn as a block of its own, and a full-screen program such as the
`nora` editor is drawn as a character grid.

The session is driven mainly from the keyboard. Chords, which are key
combinations held with the Super key (the key with the Windows logo on most PC
keyboards), split the display, move the focus between tiles, zoom a tile,
switch workspaces, and open the key reference and the theme picker. The mouse
focuses tiles, resizes them by their dividers, opens menus on tile headers and
on the objects a program prints, and presses the controls on the rails, the
two bars along the top and bottom of the display.

Halcyon requires a graphical display. With the default configuration,
logging in at the console starts a Halcyon session on the display; an image
built with the `ci` configuration starts the textual shell instead. The look
of a session is set by its profile: Instrument, the default, draws the two
rails, and the legacy profile draws a single status bar and offers fewer
controls. The tasks below describe the Instrument profile unless they say
otherwise.

## In Practice

### Log in and out

Log in at the console with a user name and password. The display then changes
to the session: a tile on the left runs a short tour of the environment, and
the tile on the right, which has the focus, runs your shell. The tour shows a
few objects to try and the most common chords, then starts a shell in its own
tile. The top rail shows the workspace, the focused tile's working directory
and program, the rail's controls and the time; the bottom rail shows the state
of the focused tile's last command, the most common chords and the number of
panes, in which a stack of tiles counts once.

If the file `~/lib/halcyon.rc` exists, the session runs it as a Utopia
script instead of showing the tour; here and below, `~` is the user's home
directory, which the shell also accepts as a leading `~` in a command. The
script can arrange tiles and start programs in them with the `halcyon`
command; an empty file starts the session with the shell tile alone.

To log out, close every tile, on every workspace. Super+Shift+Q closes the
focused tile, and when the last tile closes, the session ends and the console
shows the login prompt again. The command running in a tile is ended when the tile closes; a job
started in the background keeps running (see When a Tile's Program Ends).

Typing `exit` in a shell ends the shell but keeps its tile on the screen, with
its output and a final line such as `Process ended · exit 0`. A kept tile can
be restarted in place from its header menu, which a right click on the tile's
header opens: Restart starts the tile's program again as a new process with an
empty history. Close removes the tile when its pane holds another tile; for a
pane's only tile the entry is unavailable, and Super+Shift+Q closes the tile
instead. The menu's Rename tile and Move to workspace entries are shown but
not available.

### Move between tiles and arrange them

Super and an arrow key moves the focus to the tile in the neighbouring pane in
that direction, and a click in a tile also focuses it. Super+Shift and an
arrow key moves the focused tile one place in that direction: the tile
exchanges places with its neighbour in the same row or column, steps out of a
nested split or stack to sit beside it, or, when moved across the direction of
the whole layout, takes a new row or column of its own. A tile already at the
end of its row or column, with no enclosing row or column to step into, stays
where it is. In a stack shown as a column of headers,
Super+Shift+Up and Down move the tile one place along the column, and in a
stack that shows a single header, Super+Shift+Left and Right move it one place
in the stack's order. The moved tile keeps the focus.

Each tile sits in a pane, a rectangle of the layout, and a pane can hold a
stack of several tiles, of which it shows one. Super+H starts a new shell in
a new pane beside the focused tile's pane, and Super+V in a new pane below
it; the new shell starts in the user's home directory. When the focused pane
already stands in a row of panes side by side, Super+H adds the new pane to
that row: the new pane is made as wide as the row's panes are on average, and
the others narrow in proportion, so a row of two equal panes becomes a row of
three. Otherwise Super+H halves the focused pane. Super+V does the same in a
column of panes one above the other. A tile in a stack is split as its whole
stack: the new pane goes beside or below the stack, never into it. Super+N
opens a new shell in the focused pane itself: the new tile
joins the pane's stack, and a pane that held a single tile becomes a stack of
two, shown as a column of headers above the open tile. In an empty pane,
Super+N starts the shell in the pane. Super+Tab and
Super+Shift+Tab open the next and the previous tile of the stack.

Super+S, Super+Shift+T and Super+E rearrange the split or stack that holds
the focused tile. Super+S makes it a stack shown as a column of headers.
Super+Shift+T makes it a stack that shows only its open tile, under a single
header carrying the tile's number in the stack; the other tiles stay in the
stack, Super+Tab and Super+Shift+Tab reach them, and Super+S shows their
headers again. A split that contains another split or stack cannot become a
stack, and the chord then has no effect.
Super+E lays the tiles one above the other when they are side by side, and
side by side otherwise, so it both turns a split and spreads out a stack.

Super+F zooms the focused tile to fill the workspace, and Super+F again
returns it to its place; a chord that changes the layout, such as a split,
returns it first. The other tiles keep running while it is zoomed.

A divider between two tiles can be dragged with the mouse to resize them, and
double-clicking it restores the even split. The RESET control on the top rail
asks for confirmation, then makes every tile on the workspace its even size
again and opens the first tile of each stack; it closes nothing.

A pane is never made narrower than 260 pixels, nor too short for its headers
and a body 54 pixels tall; these are the limits at 100 percent display scale,
and they grow with it. Dragging a divider stops at a limit, and a chord that
would take a pane past one has no effect: on a display 1280 pixels wide, a
row holds at most four panes side by side.

### Close a tile

Super+Shift+Q closes the focused tile. If a command is still running in it,
Halcyon first asks, in a dialog titled with the tile's name, whether to close
it: Cancel, the default, keeps the tile and its command, and Close tile closes
the tile and ends the command. Tab, Left and Right move between the two
buttons, Enter or Space chooses, and Escape cancels.

The `×` in a tile's header and Close in its header menu close the tile the same
way, asking the same question over a running command. The `×` refuses to close
a pane's only tile, which is reported as `FINAL TILE IS PROTECTED` on the
bottom rail, and the menu's Close is unavailable for such a tile. Super+Shift+Q
has no such restriction, which is how a pane's last tile, and finally the
session, is closed.

### Use workspaces

A workspace is a separate arrangement of tiles, of which the display shows one
at a time. Super+1 to Super+9 switches to the workspace with that number and
creates it if it does not exist; a new workspace starts with an empty pane, in
which Super+N or the pane's Open shell control starts a shell. Super+Shift+1 to
Super+Shift+9 moves the focused tile to the workspace with that number,
creating it if necessary; the display stays on the current workspace.

Tiles on other workspaces keep running while they are not displayed. When the
last tile on a workspace closes, or moves to another workspace, while another
workspace still holds tiles, the workspace keeps an empty pane in its place,
and the session continues; keys typed while that pane has the focus are
discarded. A workspace that holds no tiles is removed when you
switch away from it, and the remaining workspaces keep their numbers. The top rail shows a numbered chip
for each workspace once there is more than one; clicking a chip switches to
it, and clicking the mark at the left end of the rail lists the workspaces.

`halcyon workspace 3` switches to workspace 3 from a shell or from
`~/lib/halcyon.rc`, creating it if necessary.

### Scroll back through a tile

Output that has scrolled off the top of a tile is kept in the tile's history,
which is reached through Normal mode. Press Escape to enter Normal mode; a
marker appears on the row of the shell's prompt, and the keyboard now moves
the marker instead of typing into the shell. `k` or the up arrow moves up a
line, `u` or Page Up moves up half the tile's height, and `g` or Home moves to
the oldest line; `j`, `d`, `G` and their counterparts move down. The view
follows the marker. `i` returns to the prompt and to typing.

`v` starts a selection at the marker, which then extends the selection as it
moves; `v` again, or Escape, ends it. A selection cannot be copied in a
session tile: `y` only ends it, and `p` returns to typing without pasting.

Escape enters Normal mode only while the tile shows its ordinary screen. A
full-screen program receives Escape itself, and a tile that switches to such a
program leaves Normal mode. The mouse wheel does not scroll a tile's history.

### Open a menu on an object

Programs that write Beacon mark some of what they print as objects: a file
name printed by `ls` is a path object, for example. In Normal mode, `w` and `b`
move the marker to the next and the previous object, and Enter opens a menu of
commands for the selected object; a left click on an object opens the same
menu. Choosing a command types it into the tile's shell, in place of anything
partly typed there, and runs it on exactly the object the menu names.

| Object | Commands |
|---|---|
| A path | `ls`, `cat`, `view`, `gallery`, `stat`, `cd`, `edit` (in `nora`), `hexdump` |
| A process | `kill` |
| A URL | `fetch` (with `wget`) |
| A saved layout | `restore`, `save`, `delete` |

In a menu, the arrow keys or `j` and `k` move the selection, Enter chooses,
and Escape closes the menu without running anything. The commands come from
the file `/lib/beacon/verbs`.

### Clear the screen and delete a tile's history

`clear` empties the tile's view and starts the next output at its top. The
text it erases is not lost: it is moved into the tile's history, above the
view, where Normal mode reaches it. A program cannot delete the history: the
escape sequences that discard the scrollback in other terminals act in
Halcyon as `clear` does.

Super+K deletes the focused tile's history: every line above the tile's
current screen, including earlier output of a command that is still running.
It does not ask for confirmation and cannot be undone. The screen itself is
left as it is, so `clear` followed by Super+K leaves an empty tile. A running
program is not affected, and new output starts a new history.

The history is also limited in size. The session shares one budget among its
tiles, so a tile that produces a great deal of output, or the opening of more
tiles, can remove a tile's oldest lines.

### Look up the keys

Super+/ opens the key reference, a dialog that lists the chords for moving
the focus, arranging and closing tiles, the history, the theme, the scale and
the reference itself, each with the keys bound to it; the `?` control on the
top rail opens it as well. The workspace chords, Super+1 to Super+9 with and
without Shift, are not listed. The arrow keys, `j` and `k` scroll the
reference, Home and End jump to its ends, and Escape, Enter or Space closes
it. The keys it shows are read from the compositor's bindings each time it
opens, so a changed binding appears there.

### Change the theme and the size of the display

Super+T opens the theme picker, which the theme control on the top rail also
opens. The arrow keys or `j` and `k` move through the installed themes, and
Enter or Space applies the selected theme at once to the whole session. The
choice is saved in `~/lib/halcyon/theme`, so later sessions start with
it. Escape closes the picker without changing the theme.

Super+= enlarges everything on the display by 25 percent, up to 200 percent,
Super+- reduces it, down to 100 percent, and Super+0 restores the scale
measured from the display. The scale applies to the whole display.

### Save and restore layouts

A layout is a saved arrangement of tiles together with the command each tile
runs. Save the current arrangement under a name, and rebuild it later:

```sh
halcyon layout save work
halcyon layout restore work
```

`halcyon layout save` writes `~/lib/halcyon/layouts/work`. `halcyon
layout restore` looks for the name in that directory first, then in
`/lib/halcyon/layouts`, which holds the layouts supplied with the image;
restoring builds the saved tiles beside the existing ones and starts each
saved command in its tile. `halcyon layout list` lists both sets, with each
name as a layout object whose menu offers restore, save and delete, and
`halcyon layout delete work` removes a saved layout. A name is a single file
name made of letters, digits, `.`, `_` and `-`, and does not begin with `.`
or `-`. Restoring a name found in neither directory reports that no layout of
that name exists, and deleting a layout supplied with the image is refused,
because `halcyon layout delete` removes only layouts saved in
`~/lib/halcyon/layouts`; both exit with status 1.

To arrange tiles at every login, put the commands in `~/lib/halcyon.rc`.

### Default chords

| Chord | Action |
|---|---|
| Super+Left, Right, Up, Down | Focus the neighbouring pane |
| Super+Shift+Left, Right, Up, Down | Move the focused tile |
| Super+H | Start a shell in a new pane beside the focused one |
| Super+V | Start a shell in a new pane below the focused one |
| Super+E | Turn the split, or lay a stack's tiles side by side |
| Super+N | Open a new shell in the focused pane's stack |
| Super+F | Zoom the focused tile, or return it |
| Super+S | Make the split or stack a stack with a column of headers |
| Super+Shift+T | Make the split or stack a stack that shows one tile |
| Super+Tab, Super+Shift+Tab | Open the next or the previous tile of the stack |
| Super+Shift+Q | Close the focused tile |
| Super+K | Delete the focused tile's history |
| Super+T | Open the theme picker |
| Super+/ | Open the key reference |
| Super+=, Super+-, Super+0 | Enlarge, reduce or restore the display scale |
| Super+1 to Super+9 | Switch to that workspace |
| Super+Shift+1 to Super+Shift+9 | Move the focused tile to that workspace |

### Normal-mode keys

| Key | Action |
|---|---|
| Escape (while typing) | Enter Normal mode |
| `k` or Up, `j` or Down | Move up or down a line |
| `u`, Page Up, Ctrl-U | Move up half the tile's height |
| `d`, Page Down, Ctrl-D | Move down half the tile's height |
| `g` or Home, `G` or End | Move to the oldest or the newest line |
| `v` | Start or end a selection |
| Escape (in Normal mode) | End the selection |
| `w`, `b` | Select the next or the previous object |
| Enter | Open the menu for the selected object |
| `i` | Return to the prompt |

### Files

| File | Purpose |
|---|---|
| `~/lib/halcyon.rc` | A Utopia script run at session start in place of the tour |
| `~/lib/halcyon/theme` | The theme chosen in the picker |
| `~/lib/halcyon/profile` | `legacy` or `instrument`, overriding the image's profile |
| `~/lib/halcyon/layouts/` | Layouts saved with `halcyon layout save` |
| `/lib/halcyon/layouts/` | The layouts supplied with the image, including the tour's |
| `/lib/halcyon/themes/` | The installed themes |
| `/lib/beacon/verbs` | The commands offered on objects |

## Technical Details

### The session and its processes

After a successful login, `login` starts the session compositor, `halcyond`,
running as the user who logged in. The compositor connects to `tapestryd`, the
system's display server, which owns the display, the keyboard and the mouse,
and asks it for the whole screen. The console renderer that drew the login
prompt stays behind the session until the session ends.

Each tile is backed by a terminal process, `kaua-term`, which the compositor
starts as the same user. The terminal owns a pseudo-terminal, runs the tile's
program on it, and forwards the program's screen to the compositor as a stream
of records. A tile's program therefore runs as the user, with the user's
authority.

The compositor lays out and draws every tile from those records. Because each
tile's output is parsed in the compositor, a malformed stream from one tile is
contained there: the tile is frozen with its last frame and marked as crashed,
and the other tiles and the session continue. When the session's last tile
closes, on whichever workspace it was, the compositor exits, and `login` treats
that as the end of the session.

### Keys, chords and the focus

`tapestryd` routes every key. While Super is held, every other key belongs to
the compositor and none reaches a tile, whether or not the key is bound to an
action. A program in a tile therefore never sees a chord, and nothing it
writes to its terminal can act as one. Other keys go to the focused tile.

The bindings are a table in `tapestryd`, which publishes the bindings in force
as a file; the key reference and the hints on the bottom rail are built from
that file, so they change with the bindings. The theme picker, the key
reference, Super+K and Super+Shift+Q are carried out by the session
compositor: `tapestryd` delivers those chords to it through the top rail. The
legacy profile has no rails, so there the first three chords have no effect,
and `tapestryd` closes a tile on Super+Shift+Q itself, without asking.

### Rich text and the character grid

A session tile's terminal tells the programs it runs, through the `BEACON`
environment variable, that it renders the rich tier. Programs that support
Beacon, such as `ls`, `ps` and `manual`, then emit its markup alongside their
text, and the compositor draws headings, tables, emphasis, framed passages
and objects from it. Output without markup is drawn as a terminal view, a
block set in a fixed-width typeface on the theme's terminal background, in
which columns line up as the program laid them out. The shell marks its
prompts and the commands typed at them, and the compositor sets those in
proportional type.

A program that switches the terminal to its alternate screen, as full-screen
editors and monitors do, is drawn as a character grid in a fixed-width
typeface until it switches back.

### History and the view

A tile's history, its transcript, is held by the compositor. Lines enter it
when they scroll off the top of the terminal's screen, and when a program
erases the whole screen, as `clear` does, the erased rows are moved into it
first. An erase of the whole screen also pins the view, so that the next
output starts at the top of the tile, clear of the history above it; the pin
is released when output next scrolls the screen. The escape sequences that
other terminals use to discard their scrollback, erase-scrollback and the full
reset, act in Halcyon as an erase of the whole screen and delete nothing from
the transcript.

The screen below the history belongs to the running program, which can
overwrite or erase it at any time, as in any terminal; only what leaves the
screen by scrolling or by a whole-screen erase is kept. Super+K is read from
the keyboard by the compositor and has no equivalent a program could send,
which is why a program cannot delete the transcript.

The session's transcripts share one memory budget. When the budget is
exhausted, the oldest lines are removed first.

### When a tile's program ends

Under the Instrument profile a tile whose program exits is kept, with its
last output, until the user restarts or closes it. A restart starts the
tile's command again as a new process in the same place, with an empty
history. Under the legacy profile a tile whose program exits with status 0
closes, and one whose program fails or whose stream breaks is frozen until it
is closed.

Closing a tile closes the compositor's channel to its terminal. The terminal
then ends the tile's program, usually the shell, with a kill that cannot be
caught, and exits once that program has exited; a terminal still running two
seconds later, because a command the shell started holds it open, is killed
by the compositor. The end of the terminal is a hangup: the command in the
foreground receives the `tty:hup` note and ends unless it handles the note. A
job running in the background receives no note and keeps running, although
its terminal is gone: a read from the terminal returns the end of the input,
and a write to it transfers nothing.

### Workspaces

`tapestryd` keeps each workspace as a separate tree of tiles. Only the active
workspace is displayed; the tiles of the others stay alive with their last
frame and receive no redraws until their workspace is shown again. A
workspace is identified by its number, from 1 to 9, and an empty workspace
other than the active one is removed at the next rearrangement.

The console's own renderer keeps a hidden place in the top row of workspace
1, where every session starts, and the session's tiles share that row with it.
`tapestryd` marks an empty pane it makes on its own account -- a new
workspace's pane, or the pane a workspace's last tile leaves when it closes or
moves away -- and the session compositor starts no shell in a marked pane
until it is asked, by Open shell or by Super+N. A pane made by a split, or
built by `halcyon layout restore`, is unmarked, and its program starts at
once. A tile that is the last on its workspace leaves a marked pane
behind only while a tile on another workspace keeps the session going; the
session's last tile of all closes outright. Without the kept pane, the
workspace would hold only the console's hidden place, and the keys typed there
would reach the console's renderer instead of the session.

When the session ends, `tapestryd` closes the marked panes and displays
workspace 1, and the other workspaces, now empty, are removed. The
login prompt is therefore shown, and receives the keys, whichever workspace the
session ended on.

### Profiles

The profile is one word, `instrument` or `legacy`, read at session start from
`~/lib/halcyon/profile` or, when that file is absent or holds neither word,
from `/lib/halcyon/profile`. It selects the layout of the session's chrome, its
typefaces and its sizes. The Instrument profile adds the rails, the key
reference, the theme picker, the confirmation dialogs, the empty pane's Open
shell control and kept tiles; the
legacy profile keeps the earlier status bar and closes a tile whose program
exits cleanly.
