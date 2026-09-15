// The pane tree (Tapestry G-6; TAPESTRY.md sections 13-15 + 18.5) -- the
// i3 container model: ONE structural primitive (a container whose layout
// mode is split-h | split-v | tabbed | stacked), nestable, with leaves
// hosting surfaces. The screen is the root container; "tabs" and "splits"
// are modes of the same primitive, not levels.
//
// This module is the PURE tree: slots, structure, geometry, focus, and the
// text rendering. It knows surface INDICES but never touches the surface
// table, the GPU, or the 9P layer -- Comp (server.rs) orchestrates both
// sides and keeps them coherent (host/unhost return the affected surface
// so the caller can fix its side).
//
// Placement policy encoded here:
//   - split FLATTENS into a same-mode parent (sibling insert) and NESTS
//     under a different-mode one -- the tiling-standard shallow tree.
//   - a split focuses the NEW empty leaf: that is the auto-host targeting
//     mechanism (Comp hosts the next created surface into the focused
//     empty leaf).
//   - closing a leaf collapses single-child containers; the root pane is
//     never removed (an empty root leaf is the blank screen).
//   - pane PUBLIC ids are monotonic and never reused (the net-3d
//     discipline for free: a stale pane fid resolves to nothing).
//   - geometry, under the LEGACY profile: equal division (remainder to the
//     last child); the Daylight chrome ring per pane iff more than one pane
//     is visible (the single-fullscreen root leaf keeps the stage-0
//     borderless look). Under the INSTRUMENT profile (HALCYON-INSTRUMENT 5;
//     I-2): a weighted N-ary division with 7 px tracks between siblings,
//     every leaf a stack of one inside a 1 px frame with a 32 px header, a
//     stack's collapsed tiles given their header rects, the workspace
//     padded 3 inside the two rails the compositor carves off the display.
//     The two carves share nothing but the tree: the legacy one is
//     byte-identical to what it was before the profile existed.

use alloc::string::String;
use alloc::vec::Vec;

use libhalcyon::carve::{self, DEFAULT_WEIGHT};
use libhalcyon::instrument::Profile;
use libhalcyon::theme;

pub const MAX_PANES: usize = 32;

/// HALCYON-WORKSPACES 4 (the ratified bound; I-32): nine workspaces, because
/// Super+1..9 is the whole keyboard's worth and a client verb must not be
/// able to mint more. Workspaces PARTITION the `MAX_PANES` pool -- they never
/// enlarge it, so the resource floor is unchanged by this feature.
pub const MAX_WORKSPACES: usize = 9;

// The blank/empty-pane fill moved to `Theme.blank` at HALCYON-THEME TH-2: it
// was the last chrome colour outside the token source, and a near-black hole
// is exactly what a light theme must be able to retint. Every chrome colour
// now reaches a painter through `Comp.theme`.

// The tab/stack indicator strip height (G-6c; glyph-free per D7 -- the
// compositor paints colored segments, never titles) is carved from the TOP
// of a tabbed/stacked container's rect: tabbed = ONE row divided into
// per-child segments; stacked = one full-width row PER child. The value is
// `Layout.metrics.tab_strip_h` -- `Metrics::at(scale)`, the single
// chrome-token source at the display's scale (HALCYON-SCALE 5).

#[derive(Clone, Copy, PartialEq)]
pub enum Dir {
    Left,
    Right,
    Up,
    Down,
}

impl Dir {
    pub fn parse(s: &str) -> Option<Dir> {
        match s {
            "left" => Some(Dir::Left),
            "right" => Some(Dir::Right),
            "up" => Some(Dir::Up),
            "down" => Some(Dir::Down),
            _ => None,
        }
    }
    fn horizontal(self) -> bool {
        matches!(self, Dir::Left | Dir::Right)
    }
    fn before(self) -> bool {
        matches!(self, Dir::Left | Dir::Up)
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Mode {
    SplitH,
    SplitV,
    Tabbed,
    Stacked,
}

impl Mode {
    pub fn name(self) -> &'static str {
        match self {
            Mode::SplitH => "splith",
            Mode::SplitV => "splitv",
            Mode::Tabbed => "tabbed",
            Mode::Stacked => "stacked",
        }
    }
    pub fn parse(s: &str) -> Option<Mode> {
        match s {
            "splith" => Some(Mode::SplitH),
            "splitv" => Some(Mode::SplitV),
            "tabbed" => Some(Mode::Tabbed),
            "stacked" => Some(Mode::Stacked),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Role {
    Content,
    Chrome,
    PinTarget,
    /// H-3c: the ephemeral verb menu (a SURFACE role only; never a pane's).
    Menu,
    /// H-3d: the screen-bottom status bar (a SURFACE role only): the one
    /// piece of chrome bound to the DISPLAY, not to a pane.
    Status,
    /// HALCYON-INSTRUMENT 8: the display-top rail (a SURFACE role only),
    /// the second piece of display-bound chrome; exists only under the
    /// Instrument profile, whose carve always reserves it.
    Rail,
}

impl Role {
    pub fn name(self) -> &'static str {
        match self {
            Role::Content => "content",
            Role::Chrome => "chrome",
            Role::PinTarget => "pin-target",
            Role::Menu => "menu",
            Role::Status => "status",
            Role::Rail => "rail",
        }
    }
    pub fn parse(s: &str) -> Option<Role> {
        match s {
            "content" => Some(Role::Content),
            "chrome" => Some(Role::Chrome),
            "pin-target" => Some(Role::PinTarget),
            "menu" => Some(Role::Menu),
            "status" => Some(Role::Status),
            "rail" => Some(Role::Rail),
            _ => None,
        }
    }
}

/// H-3d: what a `role=status` registration asks for. A struct rather than a
/// positional argument list because the two `bool`s and the four `u32`s are
/// mutually transposable and the compiler would not catch a swap: at the call
/// site each field is named, so `disp_w`/`disp_h` and the two principal bools
/// cannot be silently exchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StatusReq {
    /// A bar is already registered on this display.
    pub bar_registered: bool,
    pub w: u32,
    pub h: u32,
    pub disp_w: u32,
    pub disp_h: u32,
    /// The one vertical unit the strip occupies (`Metrics::status_h`).
    pub status_h: u32,
    /// At least one session connection is declared on this display.
    pub session_declared: bool,
    /// The requesting surface's owner is a session principal (i.e. not
    /// SYSTEM / NONE / INVALID).
    pub requester_is_session: bool,
}

/// The verdict on a status-bar registration.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StatusAdmit {
    Admit,
    /// A bar already exists, or the geometry is not exactly the strip.
    Malformed,
    /// The display belongs to a declared session; a SYSTEM renderer may not
    /// take the slot.
    NotYours,
}

/// H-3d: whether a `role=status` surface may become THE display's status bar.
///
/// The second arm is the one worth having as a seam. Retiring the console's
/// bar when a session declares is necessary and NOT sufficient on its own:
/// the console observes the CLOSE, re-arms on the very relayout the retire
/// causes, and races the session for the slot it was just relieved of.
/// Whoever wins owns it, which makes "does the user have a status bar" a coin
/// toss. Refused here, the console stays bar-less while it is invisible and
/// re-mints from the relayout that foregrounds it at logout.
///
/// Pure over scalars, so the rule is testable without a compositor -- which
/// is the point: the fix this encodes landed at `9d5f38ee` with no witness of
/// any kind, host or guest.
pub fn admit_status_bar(r: &StatusReq) -> StatusAdmit {
    if r.bar_registered || r.w != r.disp_w || r.h != r.status_h || r.disp_h <= r.status_h {
        return StatusAdmit::Malformed;
    }
    if r.session_declared && !r.requester_is_session {
        return StatusAdmit::NotYours;
    }
    StatusAdmit::Admit
}

/// HALCYON-INSTRUMENT 8: what a `role=rail` registration asks for -- the
/// status bar's request with the profile beside it, since the top rail
/// exists only under Instrument (the legacy carve has no such strip and
/// `rail_h` is 0 there).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RailReq {
    /// A rail is already registered on this display.
    pub rail_registered: bool,
    pub w: u32,
    pub h: u32,
    pub disp_w: u32,
    pub disp_h: u32,
    /// The strip the carve reserves at the top (`Metrics::rail_h`; 0 under
    /// legacy, where no rail exists).
    pub rail_h: u32,
    /// The Instrument profile is in force.
    pub instrument: bool,
    pub session_declared: bool,
    pub requester_is_session: bool,
}

/// HALCYON-INSTRUMENT 8: whether a `role=rail` surface may become THE
/// display's top rail. The status bar's rule (`admit_status_bar`) with one
/// more malformed case: no rail under legacy -- refused as malformed (there
/// is no strip to be exactly), never as an authority question. The order
/// is the status bar's: geometry before ownership.
pub fn admit_rail(r: &RailReq) -> StatusAdmit {
    if !r.instrument
        || r.rail_h == 0
        || r.rail_registered
        || r.w != r.disp_w
        || r.h != r.rail_h
        || r.disp_h <= r.rail_h
    {
        return StatusAdmit::Malformed;
    }
    if r.session_declared && !r.requester_is_session {
        return StatusAdmit::NotYours;
    }
    StatusAdmit::Admit
}

/// A tile's recorded status (HALCYON.md 13.6; HALCYON-VISUAL 1.4): the exit
/// of the last command completed in it. `Resting` = none recorded (a fresh
/// tile; "nothing has run yet"). The DISPLAY key is derived, never stored:
/// the LIVE (focused) tile shows sage unless `Err` (cinnabar); a tile that
/// is not live shows no key at all -- so a stale status can never mark a
/// tile that does not hold input. Set only through the renderer-gated
/// `tag <id> status` global verb; reset here whenever the tile's program
/// changes (alloc / host / the root collapse).
#[derive(Clone, Copy, PartialEq)]
pub enum Status {
    Resting,
    Ok,
    Err,
}

impl Status {
    pub fn name(self) -> &'static str {
        match self {
            Status::Resting => "resting",
            Status::Ok => "ok",
            Status::Err => "err",
        }
    }
    pub fn parse(s: &str) -> Option<Status> {
        match s {
            "resting" => Some(Status::Resting),
            "ok" => Some(Status::Ok),
            "err" => Some(Status::Err),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

impl Rect {
    pub const ZERO: Rect = Rect {
        x: 0,
        y: 0,
        w: 0,
        h: 0,
    };

    /// Intersection (empty rects collapse to ZERO).
    pub fn intersect(self, o: Rect) -> Rect {
        let x1 = self.x.max(o.x);
        let y1 = self.y.max(o.y);
        let x2 = (self.x + self.w).min(o.x + o.w);
        let y2 = (self.y + self.h).min(o.y + o.h);
        if x2 <= x1 || y2 <= y1 {
            return Rect::ZERO;
        }
        Rect {
            x: x1,
            y: y1,
            w: x2 - x1,
            h: y2 - y1,
        }
    }
    pub fn is_empty(self) -> bool {
        self.w == 0 || self.h == 0
    }
    /// Does the half-open rect contain display point (x, y)?
    pub fn contains(self, x: u32, y: u32) -> bool {
        x >= self.x
            && x < self.x.saturating_add(self.w)
            && y >= self.y
            && y < self.y.saturating_add(self.h)
    }
}

/// HALCYON.md 13.6 / HALCYON-INSTRUMENT 9.1: may a SESSION principal
/// `chrome_owner` bind chrome to a pane whose hosted surface belongs to
/// `occupant` (None: the leaf is empty), the leaf's recorded owner being
/// `pane_owner`? An occupied leaf's owner is its surface's; an empty leaf's
/// the recorded one (H-4b-2). Judged at the mint AND at every reconcile (r1
/// A-F6), with one function so the two can never disagree -- the
/// renderer's unconditional admission is the caller's, not this.
pub fn chrome_bind_admitted(chrome_owner: u32, occupant: Option<u32>, pane_owner: u32) -> bool {
    match occupant {
        Some(o) => o == chrome_owner,
        None => pane_owner == chrome_owner,
    }
}

/// The outcome of a divider drag or double-click (9.2; `Layout::drag_track`,
/// `Layout::equalise_track`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DragVerdict {
    /// The weights moved; a reconcile is owed.
    Changed,
    /// The clamps hold the boundary where it is.
    Unchanged,
    /// The pair is in the carve's overflow: nothing to drag.
    Refused,
}

/// `r` shrunk by `by` on every side; ZERO when it cannot hold that (the
/// Instrument carve's frame and pad insets, 5.1).
fn inset(r: Rect, by: u32) -> Rect {
    if r.w > 2 * by && r.h > 2 * by {
        Rect {
            x: r.x + by,
            y: r.y + by,
            w: r.w - 2 * by,
            h: r.h - 2 * by,
        }
    } else {
        Rect::ZERO
    }
}

#[derive(Clone)]
pub enum Kind {
    Leaf {
        surface: Option<usize>,
    },
    Container {
        mode: Mode,
        children: Vec<usize>,
        active: usize,
    },
}

#[derive(Clone)]
pub struct Pane {
    pub id: u32,
    pub parent: Option<usize>,
    pub kind: Kind,
    pub role: Role,
    pub focusable: bool,
    pub tag: String,
    /// The pane's outer rect (computed; ZERO when hidden).
    pub rect: Rect,
    /// The content rect (outer minus the border inset AND the tag bar; ZERO
    /// when hidden). This is the client's usable area.
    pub content: Rect,
    /// The Daylight tag bar (HALCYON-VISUAL 3.2/4): the `header_h` strip at
    /// the TOP of the leaf's inner rect, inside the ring, above `content`.
    /// ZERO when none -- a single fullscreen leaf, a container, or a leaf too
    /// small to carve. A Role::Chrome surface binds here (H-3b); the
    /// compositor paints it `header`-bg as the resting fallback.
    /// Under Instrument (HALCYON-INSTRUMENT 5.4 / 6.3) it is the tile's
    /// HEADER rect -- set for a collapsed tile too, whose `content` is ZERO
    /// and which is not `visible`.
    pub tagbar: Rect,
    /// The pane's weight in its parent's division (HALCYON-INSTRUMENT 5.2:
    /// `u16`, sum-normalised, `DEFAULT_WEIGHT` when equal). Carried by the
    /// pane, so a swap moves it with the pane; a newcomer to a container
    /// takes the mean of its siblings (an equal share, the siblings' ratios
    /// untouched); a dissolved container's survivor takes the container's.
    /// Read only by the Instrument carve; the legacy division ignores it.
    pub weight: u16,
    /// A split container's divider tracks after the last recompute, in
    /// child order (the per-container `dividers` file, 5.5). Empty for a
    /// leaf, a stack, and under the legacy profile.
    pub dividers: Vec<Rect>,
    /// HALCYON-INSTRUMENT 6.4 (as measured on the golden, I-3): the 1 px
    /// `separator` row after an OPEN body when a header follows it -- the
    /// row `carve::stack_alloc` reserves between the body and the next
    /// header. Set on the open tile (a leaf, or a container tile carved
    /// into the body), ZERO when the open tile is the stack's last, for a
    /// collapsed tile, and under legacy. The compositor paints it; the
    /// separator INSIDE a collapsed header (its last row, unless last in
    /// the stack) is halcyond's, painted with the header.
    pub separator: Rect,
    /// The tile's recorded last-command status (see `Status`).
    pub status: Status,
    /// Visible under the current layout (tab-inactive subtrees are not).
    pub visible: bool,
    /// H-4b: a one-shot placement claim (`pane/<id>/claim` mints it, a
    /// `create ... claim=<tok>` consumes it). Set only on an EMPTY leaf and
    /// cleared the instant the leaf is hosted or freed -- so a claim can
    /// never steer a surface into a leaf that already holds one.
    pub claim_token: Option<u128>,
    /// H-4b-2: the principal that owns this pane when it is an EMPTY leaf --
    /// recorded at SPLIT from the splitting actor (0 = the renderer's / the
    /// environment; a user session stamps its own principal). Load-bearing
    /// on EXACTLY two paths: the claim mint (a session mints a placement
    /// token only on an empty leaf it owns) and the session reap (an empty
    /// leaf is closed when its owning principal's last conn goes). It is
    /// NOT consulted for structural authority (`actor_owns_subtree` keys on
    /// the hosted SURFACES, and an all-empty subtree is vacuously anyone's
    /// -- HALCYON.md 13.6); an occupied leaf's ownership is its surface's,
    /// so this field is meaningful only while the leaf is empty.
    pub owner_principal: u32,
    /// H-4d: the CONN that created this leaf by a ctl `split` (0 = none: a
    /// chord split, the environment, or a creator that has since gone).
    /// While the creator conn lives, an EMPTY leaf it split is RESERVED to
    /// it: the claim mint refuses every other same-principal conn (E_AGAIN),
    /// so a restore tool building a skeleton is never raced by its own
    /// session compositor filling the leaves before they are tagged (rio: a
    /// window a program creates is that program's, not the menu's). Cleared
    /// for every pane of a conn at its retire (`release_creator`), which is
    /// the moment the leaves become the principal's to host. Meaningful only
    /// while the leaf is empty (a hosted leaf's owner is its surface's).
    pub creator_conn: u64,
    /// The creator conn's PROCESS (its kernel stripes): a claim-less create
    /// from another conn of the SAME process (a driver's control conn
    /// splits, its surface conn hosts -- the battery's idiom) is not held
    /// off by the reservation; another process's is.
    pub creator_peer: u64,
    /// F2 (d-1b tiling completion): this leaf hosts a BACKGROUNDED surface (a
    /// non-session renderer while a session holds the display). Set by
    /// reconcile BEFORE recompute from the tree (owner-based, visibility-
    /// independent). Consulted ONLY by `layout_pane`'s Split arm, which
    /// excludes a backgrounded leaf from its parent's division (zero rect) so
    /// the foreground siblings fill the space. Orthogonal to
    /// `Surface.backgrounded` (visibility-based, post-recompute, drives
    /// compose/scanout): a Tab/Stack ACTIVE child is shown via the One path
    /// regardless of this flag, so a backgrounded-but-active leaf is never
    /// blanked.
    pub backgrounded: bool,
}

/// One workspace (HALCYON-WORKSPACES 4, mechanism (A)): its LIVE root and
/// the leaf to restore focus to on return -- focus is per workspace and
/// remembered across a switch (the i3 rule).
///
/// `focused` is a pane ID, never a slot. Slots are REUSED -- `alloc` takes
/// the first free one -- so a remembered slot can be resurrected by an
/// unrelated pane allocated in ANOTHER workspace while this one was away,
/// and an `is_leaf` restore guard cannot tell the difference, because the
/// reused slot genuinely is a live leaf. Ids are monotonic and never reused
/// (the file header's rule), so a dead id resolves to nothing -- which is
/// exactly the fallback the restore wants.
///
/// `number` is the workspace's IDENTITY (S4, operator-ratified 2026-09-15),
/// not its position: the set is SPARSE and kept sorted ascending, so 1, 3, 4
/// is an ordinary state. Identity used to be the vector index, and a vanish
/// therefore renumbered every higher workspace -- the user's tiles stayed
/// alive but Super+3 stopped reaching them. i3 treats numbers as names and
/// tmux keeps stable numbers with gaps; both refuse the renumbering.
#[derive(Clone, Copy)]
struct Workspace {
    root: usize,
    focused: u32,
    number: u8,
}

#[derive(Clone)]
pub struct Layout {
    panes: Vec<Option<Pane>>,
    /// One live root per workspace, in order. There is deliberately NO
    /// second copy of the active root: `root()` reads it through this Vec,
    /// so a switch cannot leave a stale mirror behind.
    workspaces: Vec<Workspace>,
    /// Index into `workspaces`. Invariant: `active < workspaces.len()` and
    /// `workspaces` is never empty (`new` seeds one, the vanish rule never
    /// drops the active one).
    active: usize,
    /// The focused LEAF slot -- the ACTIVE workspace's. An inactive
    /// workspace's focus lives in its `Workspace` entry until it returns.
    pub focused: usize,
    id_seq: u32,
    /// Bumped on every structural / geometry / focus mutation; Comp
    /// reconciles scanout + chrome when it observes a change.
    pub epoch: u64,
    /// The zoomed pane's PUBLIC id (tmux-`zoom`: the leaf temporarily
    /// fills the display; the tree is untouched). Held by id, not slot --
    /// slots are reused, ids never are (a freed target self-clears at the
    /// next recompute).
    zoomed_id: Option<u32>,
    /// The chrome metrics the last recompute carved with (HALCYON-SCALE 5:
    /// `Metrics::at(scale)`, handed in by Comp -- the one table both the
    /// carve and the paint read).
    pub metrics: theme::Metrics,
    /// The profile the last recompute carved for (HALCYON-INSTRUMENT 4):
    /// which of the two carves ran, and which minima a split is held to.
    pub profile: Profile,
    /// The workspace the last recompute carved (the display under legacy,
    /// less a registered status bar; the space between the rails under
    /// Instrument). The minima are judged against it.
    pub area: Rect,
}

impl Layout {
    pub fn new() -> Layout {
        let mut l = Layout {
            panes: Vec::new(),
            workspaces: Vec::new(),
            active: 0,
            focused: 0,
            id_seq: 0,
            epoch: 1,
            zoomed_id: None,
            metrics: theme::builtin().metrics,
            profile: Profile::Legacy,
            area: Rect::ZERO,
        };
        let root = l
            .alloc(None, Kind::Leaf { surface: None })
            .expect("root pane");
        let root_id = l.id_of(root).expect("root id");
        l.workspaces.push(Workspace {
            root,
            focused: root_id,
            number: 1,
        });
        l.focused = root;
        l
    }

    /// The ACTIVE workspace's root (HALCYON-WORKSPACES 4). Deliberately an
    /// accessor rather than a stored field: the one thing a workspace switch
    /// must not be able to do is leave a stale root behind, and a value that
    /// is never copied cannot go stale.
    pub fn root(&self) -> usize {
        self.workspaces[self.active].root
    }

    /// Re-seat the root of the workspace that OWNS `old` -- a dissolve, a
    /// split of a root, or a collapse moved it.
    ///
    /// NEVER the active workspace by assumption. This read `self.active`
    /// until 2026-09-15, so dissolving an INACTIVE workspace's root re-seated
    /// the ACTIVE workspace onto a pane in another tree and then freed the
    /// slot the inactive workspace still named -- both corrupted, from one
    /// `close` in a workspace nobody was looking at. `slot_of_id` is global,
    /// so that close is reachable from any conn that owns the pane.
    fn reseat_root(&mut self, old: usize, new: usize) {
        if let Some(i) = self.workspace_of_root(old) {
            self.workspaces[i].root = new;
        }
    }

    /// Which workspace has `slot` as its root, if any.
    ///
    /// `self.root()` answers only for the ACTIVE one, and every caller that
    /// used it as "is this a root" was silently asking a narrower question.
    /// That confusion is what left `close_inner` unable to see an inactive
    /// workspace's root (round 1 F1, a P0: the close became a no-op that
    /// still reported the surface unhosted) and `reap_session_empties`
    /// unable to hand one back (F7).
    fn workspace_of_root(&self, slot: usize) -> Option<usize> {
        self.workspaces.iter().position(|w| w.root == slot)
    }

    /// Is `slot` the root of SOME workspace (not merely the active one)?
    pub fn is_workspace_root(&self, slot: usize) -> bool {
        self.workspace_of_root(slot).is_some()
    }

    /// Does any pane in this subtree carry a live PLACEMENT RESERVATION -- a
    /// one-shot claim token, or a creator conn that has not gone? An empty
    /// leaf under reservation is spoken for even though it hosts nothing.
    fn subtree_reserved(&self, slot: usize) -> bool {
        let p = match self.get(slot) {
            Some(p) => p,
            None => return false,
        };
        if p.claim_token.is_some() || p.creator_conn != 0 {
            return true;
        }
        let kids: Vec<usize> = match &p.kind {
            Kind::Container { children, .. } => children.clone(),
            _ => Vec::new(),
        };
        kids.iter().any(|&c| self.subtree_reserved(c))
    }

    /// How many workspaces exist, and which is active (the `layout` header's
    /// two numbers, HALCYON-WORKSPACES 4).
    pub fn workspace_count(&self) -> usize {
        self.workspaces.len()
    }

    pub fn active_workspace(&self) -> usize {
        self.active
    }

    /// The ACTIVE workspace's number -- its identity, and what the `layout`
    /// header's `active` token carries (S4). Distinct from
    /// `active_workspace`, which is the internal index into a sparse set.
    pub fn active_number(&self) -> u8 {
        self.workspaces[self.active].number
    }

    /// The live workspace numbers, ascending -- the `layout` header's list.
    /// A COUNT cannot stand in for this once the set is sparse: a bar told
    /// "3" cannot know whether that means 1,2,3 or 1,3,4.
    pub fn workspace_numbers(&self) -> Vec<u8> {
        self.workspaces.iter().map(|w| w.number).collect()
    }

    /// Find workspace `n`, CREATING it if absent; returns its index, or None
    /// when the number is out of range or the pane table is full.
    ///
    /// One place, because the create is where this gets subtle. The set is
    /// sorted by number, so a create INSERTS rather than pushes -- and an
    /// insert at or below `active` shifts the active index, which must move
    /// with it or the seat silently changes workspace under the user.
    ///
    /// Super+N creates N DIRECTLY: the old "only the next free number" rule
    /// existed to keep a dense vector hole-free (a property of the
    /// representation) and mis-attributed i3, which creates workspace 5 on
    /// Super+5 whether or not 2, 3 and 4 exist.
    fn ensure_workspace(&mut self, n: u8) -> Option<usize> {
        if n == 0 || n as usize > MAX_WORKSPACES {
            return None;
        }
        if let Some(i) = self.workspaces.iter().position(|w| w.number == n) {
            return Some(i);
        }
        // I-32: the count is bounded and creation fails CLEAN. Unique numbers
        // in 1..=MAX_WORKSPACES bound the length on their own; the explicit
        // check keeps that a stated invariant rather than an inference.
        if self.workspaces.len() >= MAX_WORKSPACES {
            return None;
        }
        let root = self.alloc(None, Kind::Leaf { surface: None })?;
        let root_id = match self.id_of(root) {
            Some(i) => i,
            None => {
                self.panes[root] = None; // roll back: no half-made workspace
                return None;
            }
        };
        let at = self
            .workspaces
            .iter()
            .position(|w| w.number > n)
            .unwrap_or(self.workspaces.len());
        self.workspaces.insert(
            at,
            Workspace {
                root,
                focused: root_id,
                number: n,
            },
        );
        if at <= self.active {
            self.active += 1;
        }
        Some(at)
    }

    /// Every root EXCEPT the active one -- the subtrees `recompute` leaves
    /// dark and `apply_backgrounded` stamps dormant.
    fn inactive_roots(&self) -> Vec<usize> {
        self.workspaces
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != self.active)
            .map(|(_, w)| w.root)
            .collect()
    }

    /// Is `slot` inside the ACTIVE workspace's tree? Walks to its top, so it
    /// answers for a container as well as a leaf. Used where a global,
    /// id-addressed lookup (`slot_of_id`) could otherwise reach across
    /// workspaces -- the zoom target being the case that bites.
    pub fn in_active_root(&self, slot: usize) -> bool {
        let mut cur = slot;
        loop {
            if cur == self.root() {
                return true;
            }
            match self.get(cur).and_then(|p| p.parent) {
                Some(p) => cur = p,
                None => return false,
            }
        }
    }

    /// Switch to workspace NUMBER `n` (1..=`MAX_WORKSPACES`), creating it if
    /// it does not exist -- the actual i3 rule (S4).
    ///
    /// Focus is per workspace: the outgoing one's is saved and the incoming
    /// one's restored. A remembered leaf that died while the workspace was
    /// away falls back to the arriving root's first leaf, so a switch can
    /// never land focus on a freed slot.
    pub fn switch_workspace(&mut self, n: u8) -> bool {
        if self.workspaces.get(self.active).map(|w| w.number) == Some(n) {
            return false; // already there
        }
        // `ensure_workspace` may INSERT below `active` and bump it, so the
        // outgoing index is read AFTER the call, never before.
        let k = match self.ensure_workspace(n) {
            Some(k) => k,
            None => return false,
        };
        let a = self.active;
        self.workspaces[a].focused = self.id_of(self.focused).unwrap_or(0);
        self.active = k;
        // Round 1 F3: resolve the remembered ID, and accept it only if it is
        // a live leaf INSIDE the workspace just entered. Storing a SLOT let a
        // reused slot resurrect a stale focus -- `alloc` hands out the first
        // free one, so a pane created in another workspace could land on the
        // remembered slot and pass an `is_leaf` guard as a genuinely live
        // leaf. A dead id resolves to nothing, which is the fallback we want.
        let want = self.workspaces[k].focused;
        let r = self.root();
        self.focused = match self.slot_of_id(want) {
            Some(s) if self.is_leaf(s) && self.in_active_root(s) => s,
            _ => self.first_leaf(r).unwrap_or(r),
        };
        // A zoom belongs to the tree it was made in, and `zoomed_id` is one
        // global field: carrying it across would put an id in the `layout`
        // header that the carve (guarded by `in_active_root`) refuses to
        // honour -- a file saying something the screen does not.
        self.zoomed_id = None;
        self.epoch += 1;
        true
    }

    /// The i3 vanish rule (HALCYON-WORKSPACES 4): an INACTIVE workspace with
    /// no hosted leaf is dropped; the active one never is, however empty.
    /// Returns how many went. Walks backwards so a removal cannot shift an
    /// index still to be visited.
    pub fn reap_empty_workspaces(&mut self) -> usize {
        let mut dropped = 0usize;
        let mut i = self.workspaces.len();
        while i > 0 {
            i -= 1;
            if i == self.active {
                continue;
            }
            let r = self.workspaces[i].root;
            // A workspace holding a RESERVED empty leaf is not empty. H-4d
            // stamps `creator_conn` (and the claim mint a one-shot token) on
            // the skeleton a restore tool builds, precisely so its own session
            // compositor cannot fill it mid-build -- so testing only for
            // HOSTED surfaces let the vanish rule destroy exactly what that
            // reservation exists to protect.
            if self.subtree_hosted(r).is_empty() && !self.subtree_reserved(r) {
                self.free_subtree(r);
                self.workspaces.remove(i);
                if self.active > i {
                    self.active -= 1;
                }
                dropped += 1;
            }
        }
        if dropped > 0 {
            self.epoch += 1;
        }
        dropped
    }

    /// Move the FOCUSED leaf to workspace NUMBER `n` -- i3's Super+Shift+N.
    /// OWNERSHIP-PRESERVING: the leaf keeps its id, its surface and its
    /// status, and nothing is saved, restored or respawned
    /// (HALCYON-WORKSPACES 4). The target is CREATED if absent (S4).
    ///
    /// The subtle case is a focused leaf that IS this workspace's root:
    /// `detach_leaf` no-ops on a parentless pane, so moving it without
    /// re-seating would leave the SAME SLOT rooted in two workspaces at
    /// once. That branch mints a fresh empty root to leave behind.
    pub fn move_focused_to_workspace(&mut self, n: u8) -> bool {
        if self.workspaces.get(self.active).map(|w| w.number) == Some(n) {
            return false; // already here
        }
        let leaf = self.focused;
        // An empty tile is not worth moving: it would trade one placeholder
        // for another and could strand the workspace it left.
        //
        // Judged BEFORE the target is ensured, or a refused move would leave
        // a freshly-minted empty workspace behind it.
        if !self.is_leaf(leaf) || self.leaf_surface(leaf).is_none() {
            return false;
        }
        // r2 F4: allocate the replacement root BEFORE ensuring the target.
        // `ensure_workspace` mints a workspace whose root is an empty
        // placeholder, so `pre_container` below is only ever needed for a
        // workspace that ALREADY existed -- making "ensure created it, then a
        // later alloc failed" the one way a refused move could strand a
        // freshly-minted empty workspace. Allocating first closes that window
        // by construction instead of unwinding it, and whether the leaf needs
        // replacing is knowable here: it does iff it is its workspace's root.
        let pre_fresh = if self.get(leaf).and_then(|p| p.parent).is_some() {
            None
        } else {
            match self.alloc(None, Kind::Leaf { surface: None }) {
                Some(f) => Some(f),
                None => return false, // pane table full: untouched
            }
        };
        // May INSERT below `active` and bump it, so every index used below is
        // read AFTER this point.
        let k = match self.ensure_workspace(n) {
            Some(k) => k,
            None => {
                if let Some(f) = pre_fresh {
                    self.panes[f] = None; // nothing was made: leave nothing behind
                }
                return false;
            }
        };
        // Round 1 F4: allocate EVERY pane this move needs BEFORE detaching
        // the leaf. `detach_leaf`'s contract is that a leaf is never exposed
        // un-reinserted, and the old order broke it -- on an exhausted pool
        // the container alloc failed AFTER the detach and returned false with
        // the leaf parentless, in no tree, still hosting its surface:
        // invisible, un-reapable, and addressable by id through the global
        // `pane/` readdir. `move_dir` already had this ordering right
        // ("pane table full: untouched"); this path had inverted it.
        let tr_now = self.workspaces[k].root;
        // r2 F3: `reap_empty_workspaces` was taught to respect a placement
        // reservation (S5); this path judged "placeholder" on EMPTINESS alone
        // and freed a restore tool's reserved skeleton root out from under it.
        let target_is_placeholder = self.is_leaf(tr_now)
            && self.leaf_surface(tr_now).is_none()
            && !self.subtree_reserved(tr_now);
        let pre_container = if target_is_placeholder {
            None
        } else {
            match self.alloc(
                None,
                Kind::Container {
                    mode: Mode::SplitH,
                    children: Vec::new(),
                    active: 0,
                },
            ) {
                Some(c) => Some(c),
                None => {
                    if let Some(f) = pre_fresh {
                        self.panes[f] = None; // roll back the hoisted leaf
                    }
                    return false; // pane table full: untouched
                }
            }
        };
        // Past this line nothing can fail, so the tree is mutated only once
        // every pane the move needs is in hand.
        match pre_fresh {
            None => self.detach_leaf(leaf), // may dissolve, and may re-seat the root
            Some(fresh) => {
                let a = self.active;
                self.workspaces[a].root = fresh;
            }
        }
        let tr = self.workspaces[k].root;
        match pre_container {
            // The target is a bare placeholder: the arriving leaf becomes
            // its root outright rather than nesting under an empty tile.
            None => {
                self.workspaces[k].root = leaf;
                self.get_mut(leaf).unwrap().parent = None;
                self.free_subtree(tr);
            }
            Some(c) => {
                if let Some(Kind::Container { children, .. }) = self.get_mut(c).map(|p| &mut p.kind)
                {
                    children.push(tr);
                    children.push(leaf);
                }
                for s in [tr, leaf] {
                    let p = self.get_mut(s).unwrap();
                    p.parent = Some(c);
                    p.weight = DEFAULT_WEIGHT;
                }
                self.workspaces[k].root = c;
            }
        }
        self.workspaces[k].focused = self.id_of(leaf).unwrap_or(0);
        // Focus stays HERE, on what is left behind -- the tile went away,
        // the eye did not follow it (i3's move, not its move-and-follow).
        let r = self.root();
        self.focused = self.first_leaf(r).unwrap_or(r);
        self.zoomed_id = None;
        self.epoch += 1;
        true
    }

    fn alloc(&mut self, parent: Option<usize>, kind: Kind) -> Option<usize> {
        let live = self.panes.iter().filter(|p| p.is_some()).count();
        if live >= MAX_PANES {
            return None;
        }
        // Ids are never reused, so exhaustion REFUSES rather than wraps (a
        // wrap would alias a live id); 2^32 allocations away, but the
        // counter is driven by a client verb, so it fails clean.
        let id = self.id_seq.checked_add(1)?;
        self.id_seq = id;
        let p = Pane {
            id,
            parent,
            kind,
            role: Role::Content,
            focusable: true,
            tag: String::new(),
            rect: Rect::ZERO,
            content: Rect::ZERO,
            tagbar: Rect::ZERO,
            status: Status::Resting,
            visible: false,
            claim_token: None,
            owner_principal: 0,
            creator_conn: 0,
            creator_peer: 0,
            backgrounded: false,
            weight: DEFAULT_WEIGHT,
            dividers: Vec::new(),
            separator: Rect::ZERO,
        };
        let slot = match self.panes.iter().position(|s| s.is_none()) {
            Some(i) => {
                self.panes[i] = Some(p);
                i
            }
            None => {
                self.panes.push(Some(p));
                self.panes.len() - 1
            }
        };
        Some(slot)
    }

    pub fn get(&self, slot: usize) -> Option<&Pane> {
        self.panes.get(slot).and_then(|p| p.as_ref())
    }
    pub fn get_mut(&mut self, slot: usize) -> Option<&mut Pane> {
        self.panes.get_mut(slot).and_then(|p| p.as_mut())
    }

    pub fn slot_of_id(&self, id: u32) -> Option<usize> {
        self.panes
            .iter()
            .position(|p| p.as_ref().map_or(false, |p| p.id == id))
    }

    pub fn id_of(&self, slot: usize) -> Option<u32> {
        self.get(slot).map(|p| p.id)
    }

    /// All live (slot, id) pairs, slot-ordered (readdir).
    pub fn live_ids(&self) -> Vec<(usize, u32)> {
        self.panes
            .iter()
            .enumerate()
            .filter_map(|(i, p)| p.as_ref().map(|p| (i, p.id)))
            .collect()
    }

    pub fn is_leaf(&self, slot: usize) -> bool {
        matches!(self.get(slot).map(|p| &p.kind), Some(Kind::Leaf { .. }))
    }

    pub fn leaf_surface(&self, slot: usize) -> Option<usize> {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Leaf { surface }) => *surface,
            _ => None,
        }
    }

    /// H-4b-2: is `slot` an EMPTY leaf (a leaf hosting no surface)? The
    /// unit the claim mint and the session reap both key on -- ownership
    /// (`owner_principal`) is meaningful only for these.
    pub fn is_empty_leaf(&self, slot: usize) -> bool {
        matches!(
            self.get(slot).map(|p| &p.kind),
            Some(Kind::Leaf { surface: None })
        )
    }

    /// H-4b-2: the recorded owner principal of `slot` (0 = the renderer's /
    /// the environment, and the default for a missing slot). Meaningful for
    /// an empty leaf; an occupied leaf's real owner is its surface's.
    pub fn pane_owner_principal(&self, slot: usize) -> u32 {
        self.get(slot).map_or(0, |p| p.owner_principal)
    }

    /// H-4b-2: stamp `slot`'s owner principal (called after `split` with
    /// the splitting actor's principal, and by the reap to hand a reaped
    /// root-leaf back to the environment).
    pub fn set_owner_principal(&mut self, slot: usize, principal: u32) {
        if let Some(p) = self.get_mut(slot) {
            p.owner_principal = principal;
        }
    }

    /// H-4d: the conn that split `slot` into being (0 = none / released).
    pub fn pane_creator(&self, slot: usize) -> u64 {
        self.get(slot).map_or(0, |p| p.creator_conn)
    }

    /// H-4d: stamp `slot`'s creator conn (after a ctl `split`, on both
    /// resulting empties -- the splitter's, like the owner principal).
    pub fn set_creator(&mut self, slot: usize, conn: u64, peer: u64) {
        if let Some(p) = self.get_mut(slot) {
            p.creator_conn = conn;
            p.creator_peer = peer;
        }
    }

    /// H-4d: release every reservation `conn` holds (its retire). Returns
    /// how many EMPTY leaves were reserved to it -- the count the session
    /// compositor must be told about (a released leaf is now claimable by
    /// the principal's other conns, and no geometry changes to announce it).
    pub fn release_creator(&mut self, conn: u64) -> usize {
        if conn == 0 {
            return 0;
        }
        let mut released = 0;
        for p in self.panes.iter_mut().flatten() {
            if p.creator_conn == conn {
                p.creator_conn = 0;
                p.creator_peer = 0;
                if matches!(p.kind, Kind::Leaf { surface: None }) {
                    released += 1;
                }
            }
        }
        released
    }

    /// The leaf hosting surface `n` (linear scan; the table is small).
    pub fn find_hosting(&self, n: usize) -> Option<usize> {
        self.panes.iter().enumerate().find_map(|(i, p)| match p {
            Some(Pane {
                kind: Kind::Leaf { surface: Some(s) },
                ..
            }) if *s == n => Some(i),
            _ => None,
        })
    }

    /// The VISIBLE hosted surface whose pane content rect contains display
    /// point (x, y), with that content rect (the caller translates to
    /// surface-relative coords -- G-7c pointer routing: events go to the
    /// surface UNDER the pointer). Visible content rects never overlap
    /// (the geometry pass tiles them; zoom hides every other pane), so
    /// the first hit is the only hit.
    pub fn surface_at(&self, x: u32, y: u32) -> Option<(usize, Rect)> {
        self.panes.iter().flatten().find_map(|p| match p.kind {
            Kind::Leaf { surface: Some(n) }
                if p.visible
                    && x >= p.content.x
                    && x < p.content.x.saturating_add(p.content.w)
                    && y >= p.content.y
                    && y < p.content.y.saturating_add(p.content.h) =>
            {
                Some((n, p.content))
            }
            _ => None,
        })
    }

    /// Split leaf `slot`: same-mode parents FLATTEN (sibling insert),
    /// different-mode ones NEST. Returns the NEW empty leaf's slot; focus
    /// moves to it (the auto-host target).
    pub fn split(&mut self, slot: usize, mode: Mode) -> Option<usize> {
        if !self.is_leaf(slot) {
            return None;
        }
        let parent = self.get(slot)?.parent;
        if let Some(pi) = parent {
            let same = matches!(self.get(pi)?.kind,
                Kind::Container { mode: m, .. } if m == mode);
            if same {
                // The newcomer takes an equal share of the container: the
                // mean of its siblings' weights, computed BEFORE it joins,
                // so the siblings' ratios among themselves stand.
                let share = self.sibling_mean(pi);
                let new_leaf = self.alloc(Some(pi), Kind::Leaf { surface: None })?;
                if let Some(Kind::Container {
                    children, active, ..
                }) = self.get_mut(pi).map(|p| &mut p.kind)
                {
                    let at = children.iter().position(|&c| c == slot).unwrap_or(0);
                    children.insert(at + 1, new_leaf);
                    *active = at + 1;
                }
                self.get_mut(new_leaf).unwrap().weight = share;
                // Round 1 F2, the FLATTEN branch. `split` moves focus in TWO
                // places, and a guard on one of them is not a property of the
                // function -- the same shape as W-1a F2, where the zoom guard
                // went on one carve of two and the other was the one that
                // shipped. Same reason as the nest branch: `host_for` places
                // the next surface at `self.focused`.
                if self.in_active_root(new_leaf) {
                    self.focused = new_leaf;
                }
                self.epoch += 1;
                return Some(new_leaf);
            }
        }
        // Nest: the leaf's position becomes a container [leaf, new-leaf].
        // The container stands where the leaf stood, so it takes the leaf's
        // weight in the parent; inside it the two halve (equal defaults).
        let leaf_weight = self.get(slot)?.weight;
        let container = self.alloc(
            parent,
            Kind::Container {
                mode,
                children: Vec::new(),
                active: 1,
            },
        )?;
        let new_leaf = match self.alloc(Some(container), Kind::Leaf { surface: None }) {
            Some(l) => l,
            None => {
                self.panes[container] = None; // roll back the container
                return None;
            }
        };
        match parent {
            Some(pi) => {
                if let Some(Kind::Container { children, .. }) =
                    self.get_mut(pi).map(|p| &mut p.kind)
                {
                    if let Some(at) = children.iter().position(|&c| c == slot) {
                        children[at] = container;
                    }
                }
            }
            None => self.reseat_root(slot, container),
        }
        self.get_mut(slot).unwrap().parent = Some(container);
        self.get_mut(slot).unwrap().weight = DEFAULT_WEIGHT;
        self.get_mut(container).unwrap().weight = leaf_weight;
        if let Some(Kind::Container { children, .. }) = self.get_mut(container).map(|p| &mut p.kind)
        {
            children.push(slot);
            children.push(new_leaf);
        }
        // Round 1 F2: a split of a DORMANT pane must not drag focus out of
        // the active tree. Not cosmetic -- `host_for` places the next surface
        // at `self.focused`, so the next client would be hosted into an
        // invisible workspace and never seen.
        if self.in_active_root(new_leaf) {
            self.focused = new_leaf;
        }
        self.epoch += 1;
        Some(new_leaf)
    }

    /// The mean of `container`'s children's weights, round half up, at
    /// least 1 -- the equal share a newcomer takes (5.2). `DEFAULT_WEIGHT`
    /// for a container with no children or a non-container.
    fn sibling_mean(&self, container: usize) -> u16 {
        let kids: Vec<usize> = match self.get(container).map(|p| &p.kind) {
            Some(Kind::Container { children, .. }) if !children.is_empty() => children.clone(),
            _ => return DEFAULT_WEIGHT,
        };
        let sum: u64 = kids.iter().map(|&c| self.get(c).map_or(1, |p| p.weight) as u64).sum();
        let n = kids.len() as u64;
        (((sum + n / 2) / n).clamp(1, u16::MAX as u64)) as u16
    }

    /// HALCYON-INSTRUMENT 5.2: set `slot`'s weight in its parent's division
    /// (`1..=65535`; a root has no division). False = refused; the epoch
    /// moves only when the value did.
    pub fn set_weight(&mut self, slot: usize, w: u16) -> bool {
        if w == 0 {
            return false;
        }
        let p = match self.get_mut(slot) {
            Some(p) if p.parent.is_some() => p,
            _ => return false,
        };
        if p.weight != w {
            p.weight = w;
            self.epoch += 1;
        }
        true
    }

    /// Host surface `n` into the focused leaf if empty, else split the
    /// focused leaf (orientation by aspect) and host into the new leaf.
    /// Returns the hosting slot (None: pane table exhausted).
    pub fn host(&mut self, n: usize) -> Option<usize> {
        self.host_for(n, 0, 0)
    }

    /// `host`, for a surface conn `conn` of process `peer` creates: a
    /// focused EMPTY leaf that another live PROCESS split (`creator_conn`
    /// + `creator_peer`, H-4d) is that process's until its conn goes --
    /// treated as occupied here, so the surface splits beside it instead of
    /// taking a restore tool's leaf out from under its tag (the claim mint
    /// already refused such a leaf; a claim-less create fell through to this
    /// focused-leaf fallback). Keyed on the PROCESS, not the conn: a program
    /// that splits on one conn and hosts on another (the battery's control
    /// conn + its per-surface conns) fills its own leaf as before. `conn` 0
    /// = the environment, never held off.
    pub fn host_for(&mut self, n: usize, conn: u64, peer: u64) -> Option<usize> {
        let f = self.focused;
        let reserved_elsewhere = conn != 0
            && self.get(f).map_or(false, |p| {
                p.creator_conn != 0 && p.creator_conn != conn && p.creator_peer != peer
            });
        if !reserved_elsewhere {
            if let Some(p) = self.get_mut(f) {
                if let Kind::Leaf { surface: s @ None } = &mut p.kind {
                    *s = Some(n);
                    // A new program takes the tile: its status starts fresh.
                    p.status = Status::Resting;
                    p.claim_token = None;
                    // r2 F2 (P0): the H-4d reservation has SERVED ITS PURPOSE
                    // the moment the leaf is FILLED. Holding it past that made
                    // `subtree_reserved` true for every leaf halcyond ever
                    // split -- its session conn outlives the session's tiles --
                    // which silently disabled the ratified vanish rule.
                    p.creator_conn = 0;
                    p.creator_peer = 0;
                    self.epoch += 1;
                    return Some(f);
                }
            }
        }
        let r = self.get(f)?.content;
        let mut mode = if r.w >= r.h {
            Mode::SplitH
        } else {
            Mode::SplitV
        };
        // HALCYON-INSTRUMENT 5.2: the minima hold on every growth. When the
        // aspect split would leave a pane below them, the new tile joins the
        // focused leaf's STACK instead (a same-mode split flattens into an
        // existing stack; a fresh one nests) -- the mockup's own answer to a
        // full pane; and when even that will not fit, the host is refused
        // like a full pane table.
        if self.profile == Profile::Instrument && !self.split_fits(f, mode) {
            if self.split_fits(f, Mode::Stacked) {
                mode = Mode::Stacked;
            } else {
                return None;
            }
        }
        let leaf = self.split(f, mode)?;
        if let Some(Kind::Leaf { surface }) = self.get_mut(leaf).map(|p| &mut p.kind) {
            *surface = Some(n);
        }
        self.epoch += 1;
        Some(leaf)
    }

    /// H-4b: host surface `n` into the SPECIFIC empty leaf `slot` (the
    /// claim-token placement path) -- never splits, never moves focus (the
    /// tool arranges the whole skeleton, then sets focus once). Returns
    /// `slot` on success; None if `slot` is not an empty leaf (a container,
    /// a bad slot, or a leaf hosted since the claim was minted). Clears the
    /// leaf's claim regardless of the surface it now holds.
    pub fn host_into(&mut self, n: usize, slot: usize) -> Option<usize> {
        let p = self.get_mut(slot)?;
        if let Kind::Leaf { surface: s @ None } = &mut p.kind {
            *s = Some(n);
            p.status = Status::Resting;
            p.claim_token = None;
            // r2 F2 (P0): filled means the reservation is spent. See `host_for`.
            p.creator_conn = 0;
            p.creator_peer = 0;
            self.epoch += 1;
            Some(slot)
        } else {
            None
        }
    }

    /// H-4b: mint a placement claim on the empty leaf `slot`. Returns false
    /// (nothing stored) unless `slot` is an EMPTY leaf -- an occupied leaf
    /// or a container is not claimable. Last mint wins (a fresh read
    /// re-tokens the leaf); a matching `create claim=` consumes it.
    pub fn mint_claim(&mut self, slot: usize, token: u128) -> bool {
        match self.get_mut(slot) {
            Some(Pane {
                kind: Kind::Leaf { surface: None },
                claim_token,
                ..
            }) => {
                *claim_token = Some(token);
                true
            }
            _ => false,
        }
    }

    /// H-4b: the empty leaf whose live claim matches `token`, if any
    /// (non-consuming; `consume_claim` spends it).
    fn find_claim(&self, token: u128) -> Option<usize> {
        self.panes.iter().position(|p| {
            matches!(
                p,
                Some(Pane {
                    kind: Kind::Leaf { surface: None },
                    claim_token: Some(t),
                    ..
                }) if *t == token
            )
        })
    }

    /// H-4b: spend the claim `token` -- clear it (one-shot) and return the
    /// empty leaf it named. None if no empty leaf carries it (a bad/stale
    /// token, or the leaf was hosted since the mint). The caller hosts into
    /// the returned slot; a replay of the same token lands nothing.
    pub fn consume_claim(&mut self, token: u128) -> Option<usize> {
        let slot = self.find_claim(token)?;
        if let Some(p) = self.get_mut(slot) {
            p.claim_token = None;
        }
        Some(slot)
    }

    /// Close a pane. A leaf is removed (root: stays as an empty leaf); a
    /// container closes its whole subtree. Single-child containers
    /// dissolve. Returns the surfaces unhosted by the close.
    pub fn close(&mut self, slot: usize) -> Vec<usize> {
        let mut unhosted = Vec::new();
        self.close_inner(slot, &mut unhosted);
        self.epoch += 1;
        unhosted
    }

    fn close_inner(&mut self, slot: usize, unhosted: &mut Vec<usize>) {
        // Collect the subtree's hosted surfaces first.
        self.collect_surfaces(slot, unhosted);
        if let Some(wi) = self.workspace_of_root(slot) {
            // The root never leaves; it collapses back to an empty leaf.
            let kids: Vec<usize> = match self.get(slot).map(|p| &p.kind) {
                Some(Kind::Container { children, .. }) => children.clone(),
                _ => Vec::new(),
            };
            if let Some(p) = self.get_mut(slot) {
                p.kind = Kind::Leaf { surface: None };
                p.status = Status::Resting;
                p.claim_token = None;
                // r2 F2 (P0): a collapsed root is a PRISTINE root -- the rest
                // of this block already says so. A reservation that outlived
                // the tile it was stamped beside pinned the workspace forever.
                p.creator_conn = 0;
                p.creator_peer = 0;
                p.weight = DEFAULT_WEIGHT;
                p.dividers.clear();
                p.separator = Rect::ZERO;
            }
            // HALCYON-WORKSPACES 4: free only THIS root's descendants.
            // Before workspaces this freed the WHOLE POOL -- "the subtree was
            // the whole tree" was true with one root and is false with nine:
            // it would annihilate every other workspace's tree and leave
            // `workspaces` pointing at freed slots. The children must be read
            // BEFORE the kind above is replaced, or there is nothing to walk.
            for c in kids {
                self.free_subtree(c);
            }
            // Round 1 F1 (P0): this arm tested `slot == self.root()` -- the
            // ACTIVE root -- so an INACTIVE workspace's root fell through to
            // the parentless early return below and freed nothing, unhosted
            // nothing, and still handed the caller every surface
            // `collect_surfaces` had collected. The leaf went on naming a
            // surface slot the caller then freed, and surface slots ARE
            // reused (first-free), so the next client's surface surfaced
            // inside another principal's pane.
            if wi == self.active {
                self.focused = slot;
            } else if let Some(id) = self.id_of(slot) {
                self.workspaces[wi].focused = id;
            }
            return;
        }
        let parent = match self.get(slot).and_then(|p| p.parent) {
            Some(p) => p,
            None => return,
        };
        self.free_subtree(slot);
        if let Some(Kind::Container {
            children, active, ..
        }) = self.get_mut(parent).map(|p| &mut p.kind)
        {
            if let Some(at) = children.iter().position(|&c| c == slot) {
                children.remove(at);
                // HALCYON-INSTRUMENT 6.5, the successor rule: the tile at the
                // removed index, else the previous one. Removing a child
                // BEFORE the active one shifts the active one down by a
                // slot, so the index follows it -- without this the open
                // tile of [A, B, C*, D] became D when A closed (the index
                // stayed 2 and now named D), reachable from the UI the
                // moment a collapsed header carries its own close.
                if at < *active {
                    *active -= 1;
                } else if *active >= children.len() && !children.is_empty() {
                    *active = children.len() - 1;
                }
            }
        }
        // Fix focus if it pointed into the closed subtree.
        if self.get(self.focused).is_none() {
            let f = self.first_leaf(parent).unwrap_or(self.root());
            self.focused = f;
        }
        self.dissolve_if_single(parent);
    }

    /// Every hosted surface in the subtree at `slot` (the pane-tree trust
    /// model's ownership question: a client owns a subtree iff every one of
    /// these is its own).
    pub fn subtree_surfaces(&self, slot: usize) -> Vec<usize> {
        let mut out = Vec::new();
        self.collect_surfaces(slot, &mut out);
        out
    }

    /// F2: every hosted leaf in `slot`'s subtree as (leaf slot, surface) --
    /// the ownership walk's input, so a caller can consult the LEAF's stable
    /// tree `backgrounded` flag rather than the surface's visibility-derived
    /// one (which clears the moment a tab hides the leaf).
    pub fn subtree_hosted(&self, slot: usize) -> Vec<(usize, usize)> {
        let mut out = Vec::new();
        self.collect_hosted(slot, &mut out);
        out
    }

    fn collect_hosted(&self, slot: usize, out: &mut Vec<(usize, usize)>) {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Leaf { surface: Some(n) }) => out.push((slot, *n)),
            Some(Kind::Container { children, .. }) => {
                for &c in children.clone().iter() {
                    self.collect_hosted(c, out);
                }
            }
            _ => {}
        }
    }

    fn collect_surfaces(&self, slot: usize, out: &mut Vec<usize>) {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Leaf { surface: Some(n) }) => out.push(*n),
            Some(Kind::Container { children, .. }) => {
                for &c in children.clone().iter() {
                    self.collect_surfaces(c, out);
                }
            }
            _ => {}
        }
    }

    fn free_subtree(&mut self, slot: usize) {
        let kids: Vec<usize> = match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container { children, .. }) => children.clone(),
            _ => Vec::new(),
        };
        for c in kids {
            self.free_subtree(c);
        }
        self.panes[slot] = None;
    }

    /// A container left with one child dissolves: the child takes its
    /// place (in the grandparent, or as root).
    fn dissolve_if_single(&mut self, slot: usize) {
        let (only, gp, weight) = match self.get(slot) {
            Some(Pane {
                kind: Kind::Container { children, .. },
                parent,
                weight,
                ..
            }) if children.len() == 1 => (children[0], *parent, *weight),
            _ => return,
        };
        // The survivor stands where the container stood, and takes its
        // share of the grandparent's division (5.2).
        self.get_mut(only).unwrap().weight = weight;
        match gp {
            Some(g) => {
                if let Some(Kind::Container { children, .. }) = self.get_mut(g).map(|p| &mut p.kind)
                {
                    if let Some(at) = children.iter().position(|&c| c == slot) {
                        children[at] = only;
                    }
                }
                self.get_mut(only).unwrap().parent = Some(g);
            }
            None => {
                self.reseat_root(slot, only);
                self.get_mut(only).unwrap().parent = None;
            }
        }
        self.panes[slot] = None;
        if self.focused == slot {
            self.focused = self.first_leaf(only).unwrap_or(self.root());
        }
    }

    /// The first focusable leaf under `slot` (depth-first, active-first
    /// for tab/stack containers).
    pub fn first_leaf(&self, slot: usize) -> Option<usize> {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Leaf { .. }) => Some(slot),
            Some(Kind::Container {
                children, active, ..
            }) => {
                let mut order: Vec<usize> = Vec::new();
                if *active < children.len() {
                    order.push(children[*active]);
                }
                for &c in children.iter() {
                    if !order.contains(&c) {
                        order.push(c);
                    }
                }
                for c in order {
                    if let Some(l) = self.first_leaf(c) {
                        return Some(l);
                    }
                }
                None
            }
            None => None,
        }
    }

    /// Focus a leaf (containers focus their first leaf). False = no
    /// focusable leaf there.
    pub fn focus(&mut self, slot: usize) -> bool {
        // Round 1 F2: `slot_of_id` is global BY DESIGN, so an id names a pane
        // in any workspace -- and every focus-moving verb funnels through
        // here. A dormant target routed keys to an invisible tile, pulled
        // `host_for` (which places the next surface at `self.focused`) into
        // the wrong tree, and was then PERSISTED as the workspace's
        // remembered focus by the next switch. W-1a's guard fixed what got
        // DRAWN; this fixes what can be REACHED.
        if !self.in_active_root(slot) {
            return false;
        }
        match self.first_leaf(slot) {
            Some(l) => {
                if self.focused != l {
                    self.focused = l;
                    self.epoch += 1;
                }
                // Walking up, make the path the active child of each
                // tab/stack ancestor (revealing the focused leaf).
                let mut cur = l;
                while let Some(pi) = self.get(cur).and_then(|p| p.parent) {
                    if let Some(Kind::Container {
                        children, active, ..
                    }) = self.get_mut(pi).map(|p| &mut p.kind)
                    {
                        if let Some(at) = children.iter().position(|&c| c == cur) {
                            if *active != at {
                                *active = at;
                                self.epoch += 1;
                            }
                        }
                    }
                    cur = pi;
                }
                true
            }
            None => false,
        }
    }

    /// The container a `mode` on `slot` acts on: the pane itself, or a
    /// leaf's parent (the i3 shape). None = nothing to act on.
    pub fn mode_target(&self, slot: usize) -> Option<usize> {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container { .. }) => Some(slot),
            Some(Kind::Leaf { .. }) => self.get(slot).and_then(|p| p.parent),
            None => None,
        }
    }

    /// Set a container's mode (a leaf targets its parent container --
    /// the i3 shape). False = no container to act on.
    pub fn set_mode(&mut self, slot: usize, mode: Mode) -> bool {
        let target = self.mode_target(slot);
        match target {
            Some(t) => {
                if let Some(Kind::Container { mode: m, .. }) = self.get_mut(t).map(|p| &mut p.kind)
                {
                    if *m != mode {
                        *m = mode;
                        self.epoch += 1;
                    }
                    true
                } else {
                    false
                }
            }
            None => false,
        }
    }

    /// The zoomed pane's public id (None = not zoomed).
    pub fn zoom_id(&self) -> Option<u32> {
        self.zoomed_id
    }

    /// Toggle zoom on a LEAF: zoomed fills the display alone (recompute
    /// hides everything else; the tree is untouched). Zooming focuses the
    /// leaf; re-zooming the zoomed pane restores the layout.
    pub fn zoom_toggle(&mut self, slot: usize) -> bool {
        // Refuse a target outside the active tree at the SETTER. Both carves
        // already decline to honour such a zoom, but relying on two
        // independent carves to ignore a bad value is precisely the shape
        // that produced W-1a F2, where one of the two forgot -- and the
        // shipped one was the one that forgot.
        if !self.is_leaf(slot) || !self.in_active_root(slot) {
            return false;
        }
        let id = match self.id_of(slot) {
            Some(id) => id,
            None => return false,
        };
        if self.zoomed_id == Some(id) {
            self.zoomed_id = None;
        } else {
            self.zoomed_id = Some(id);
            self.focus(slot);
        }
        self.epoch += 1;
        true
    }

    /// Drop the zoom (structural mutations restore the layout first --
    /// the tmux rule). No-op when not zoomed.
    pub fn unzoom(&mut self) {
        if self.zoomed_id.take().is_some() {
            self.epoch += 1;
        }
    }

    /// Move focus SPATIALLY (G-6c; the Super+arrow walk): among visible
    /// focusable leaves, pick the nearest one strictly in `dir` with
    /// orthogonal overlap. False = no candidate (edge of the screen, or
    /// zoomed -- only one leaf is visible then).
    pub fn focus_dir(&mut self, dir: Dir) -> bool {
        match self.neighbor_dir(dir) {
            Some(s) => self.focus(s),
            None => false,
        }
    }

    /// The leaf `focus_dir` would move to, without moving (the pane-tree
    /// gate asks before it lets a client walk focus onto a tile).
    pub fn neighbor_dir(&self, dir: Dir) -> Option<usize> {
        let fr = match self.get(self.focused) {
            Some(p) => p.rect,
            None => return None,
        };
        let overlap = |a1: u32, l1: u32, a2: u32, l2: u32| -> u32 {
            let lo = a1.max(a2);
            let hi = (a1 + l1).min(a2 + l2);
            hi.saturating_sub(lo)
        };
        let mut best: Option<(usize, u32, u32)> = None; // (slot, dist, overlap)
        for (slot, _) in self.live_ids() {
            if slot == self.focused || !self.is_leaf(slot) {
                continue;
            }
            let p = self.get(slot).unwrap();
            if !p.visible || !p.focusable {
                continue;
            }
            let r = p.rect;
            let (ok, dist, ov) = match dir {
                Dir::Left => (
                    r.x + r.w <= fr.x,
                    fr.x - (r.x + r.w).min(fr.x),
                    overlap(r.y, r.h, fr.y, fr.h),
                ),
                Dir::Right => (
                    fr.x + fr.w <= r.x,
                    r.x.saturating_sub(fr.x + fr.w),
                    overlap(r.y, r.h, fr.y, fr.h),
                ),
                Dir::Up => (
                    r.y + r.h <= fr.y,
                    fr.y - (r.y + r.h).min(fr.y),
                    overlap(r.x, r.w, fr.x, fr.w),
                ),
                Dir::Down => (
                    fr.y + fr.h <= r.y,
                    r.y.saturating_sub(fr.y + fr.h),
                    overlap(r.x, r.w, fr.x, fr.w),
                ),
            };
            if !ok || ov == 0 {
                continue;
            }
            let better = match best {
                None => true,
                Some((_, bd, bo)) => dist < bd || (dist == bd && ov > bo),
            };
            if better {
                best = Some((slot, dist, ov));
            }
        }
        best.map(|(s, _, _)| s)
    }

    /// The nearest tab/stack ancestor of `slot` (the container `tab_cycle`
    /// acts on from there). None: no tabbed or stacked ancestor.
    pub fn tab_ancestor(&self, slot: usize) -> Option<usize> {
        let mut cur = slot;
        loop {
            let p = self.get(cur).and_then(|p| p.parent)?;
            if matches!(
                self.get(p).map(|q| &q.kind),
                Some(Kind::Container {
                    mode: Mode::Tabbed | Mode::Stacked,
                    ..
                })
            ) {
                return Some(p);
            }
            cur = p;
        }
    }

    /// Detach a leaf from its parent (the parent dissolves if left
    /// single). The leaf stays allocated, parentless; the caller
    /// re-inserts it (move) -- never exposed un-reinserted.
    fn detach_leaf(&mut self, slot: usize) {
        let parent = match self.get(slot).and_then(|p| p.parent) {
            Some(p) => p,
            None => return,
        };
        if let Some(Kind::Container {
            children, active, ..
        }) = self.get_mut(parent).map(|p| &mut p.kind)
        {
            if let Some(at) = children.iter().position(|&c| c == slot) {
                children.remove(at);
                if *active >= children.len() && !children.is_empty() {
                    *active = children.len() - 1;
                }
            }
        }
        self.get_mut(slot).unwrap().parent = None;
        self.dissolve_if_single(parent);
    }

    /// Move a leaf directionally (G-6c; the D6 live-reparent verb, the i3
    /// shape). Within a matching-axis ancestor: swap with the adjacent
    /// sibling; nested deeper: pull the leaf out beside its subtree; at
    /// the far edge with no outer matching level: no-op (false). A pure
    /// cross-axis move at the root wraps the root in a fresh axis
    /// container. Horizontal moves also walk tab order (Tabbed matches
    /// the h axis, Stacked the v axis). The moved leaf keeps focus.
    pub fn move_dir(&mut self, slot: usize, dir: Dir) -> bool {
        if !self.is_leaf(slot) {
            return false;
        }
        // r2 F1: round 1 closed the cross-workspace class at the FOCUS
        // chokepoint, and this is a STRUCTURAL verb taking a caller-supplied
        // slot -- `slot_of_id` is global by design, so a dormant pane's id is
        // reachable by verb. Its root-wrap branch below reads `self.root()`
        // (the ACTIVE root) unconditionally, so it would graft the pane out of
        // its own workspace and re-parent a root the caller never named.
        // Refusing matches `focus`'s precedent; teaching the wrap to re-seat
        // the pane's OWN workspace root would be a new feature, not a fix.
        if !self.in_active_root(slot) {
            return false;
        }
        let horiz = dir.horizontal();
        let before = dir.before();
        let axis_match = |m: Mode| {
            if horiz {
                matches!(m, Mode::SplitH | Mode::Tabbed)
            } else {
                matches!(m, Mode::SplitV | Mode::Stacked)
            }
        };
        let mut sub = slot;
        let mut saw_axis_edge = false;
        loop {
            let anc = match self.get(sub).and_then(|p| p.parent) {
                Some(a) => a,
                None => {
                    // Ran out of ancestors. Past a matching container's far
                    // edge this is the screen edge -- no-op; with NO
                    // matching level anywhere it is a cross-axis move --
                    // wrap the root in a fresh axis container.
                    if sub == slot || saw_axis_edge {
                        return false;
                    }
                    let mode = if horiz { Mode::SplitH } else { Mode::SplitV };
                    let c = match self.alloc(
                        None,
                        Kind::Container {
                            mode,
                            children: Vec::new(),
                            active: 0,
                        },
                    ) {
                        Some(c) => c,
                        None => return false, // pane table full: untouched
                    };
                    self.detach_leaf(slot);
                    let oldroot = self.root(); // re-read: detach may dissolve
                    if let Some(Kind::Container { children, .. }) =
                        self.get_mut(c).map(|p| &mut p.kind)
                    {
                        if before {
                            children.push(slot);
                            children.push(oldroot);
                        } else {
                            children.push(oldroot);
                            children.push(slot);
                        }
                    }
                    self.get_mut(oldroot).unwrap().parent = Some(c);
                    self.get_mut(slot).unwrap().parent = Some(c);
                    // A fresh two-way division: equal halves.
                    self.get_mut(oldroot).unwrap().weight = DEFAULT_WEIGHT;
                    self.get_mut(slot).unwrap().weight = DEFAULT_WEIGHT;
                    self.reseat_root(oldroot, c);
                    self.epoch += 1;
                    self.focus(slot);
                    return true;
                }
            };
            let (mode, kids) = match self.get(anc).map(|p| &p.kind) {
                Some(Kind::Container { mode, children, .. }) => (*mode, children.clone()),
                _ => return false,
            };
            if !axis_match(mode) {
                sub = anc;
                continue;
            }
            let i = match kids.iter().position(|&c| c == sub) {
                Some(i) => i,
                None => return false,
            };
            if sub == slot {
                // Direct child: swap with the neighbor, or escalate past
                // the edge to the next matching level.
                let j = if before {
                    i.checked_sub(1)
                } else if i + 1 < kids.len() {
                    Some(i + 1)
                } else {
                    None
                };
                match j {
                    Some(j) => {
                        if let Some(Kind::Container { children, .. }) =
                            self.get_mut(anc).map(|p| &mut p.kind)
                        {
                            children.swap(i, j);
                        }
                        self.epoch += 1;
                        self.focus(slot);
                        return true;
                    }
                    None => {
                        saw_axis_edge = true;
                        sub = anc;
                        continue;
                    }
                }
            }
            // Nested deeper inside child `sub`: pull the leaf out and
            // insert it beside that subtree. The index stays valid across
            // detach_leaf's dissolution (a dissolving container is
            // REPLACED in place at its own index).
            let at = if before { i } else { i + 1 };
            self.detach_leaf(slot);
            // The leaf joins `anc`'s division as a newcomer: an equal share.
            let share = self.sibling_mean(anc);
            if let Some(Kind::Container { children, .. }) = self.get_mut(anc).map(|p| &mut p.kind) {
                let at = at.min(children.len());
                children.insert(at, slot);
            }
            self.get_mut(slot).unwrap().parent = Some(anc);
            self.get_mut(slot).unwrap().weight = share;
            self.epoch += 1;
            self.focus(slot);
            return true;
        }
    }

    /// Cycle the ACTIVE child of the nearest tabbed/stacked ancestor of
    /// the focused leaf (G-6c; Super+Tab). Focus follows into the newly
    /// revealed child. False = no tab/stack ancestor.
    pub fn tab_cycle(&mut self, forward: bool) -> bool {
        let mut cur = self.focused;
        loop {
            let p = match self.get(cur).and_then(|p| p.parent) {
                Some(p) => p,
                None => return false,
            };
            let is_tab = matches!(
                self.get(p).map(|q| &q.kind),
                Some(Kind::Container {
                    mode: Mode::Tabbed | Mode::Stacked,
                    ..
                })
            );
            if is_tab {
                // F2 structural transparency: cycle over the NON-backgrounded
                // children, so `tab next/prev` never lands on a stepped-back
                // console leaf. Snapshot children first (releasing the borrow),
                // flag bg-ness, then advance under the mut borrow.
                let kids: Vec<usize> = match self.get(p).map(|q| &q.kind) {
                    Some(Kind::Container { children, .. }) => children.clone(),
                    _ => return false,
                };
                let bg: Vec<bool> = kids.iter().map(|&c| self.is_bg_subtree(c)).collect();
                let target = match self.get_mut(p).map(|q| &mut q.kind) {
                    Some(Kind::Container {
                        children, active, ..
                    }) => {
                        let n = children.len();
                        if n == 0 {
                            return false;
                        }
                        let mut next = *active;
                        for _ in 0..n {
                            next = if forward {
                                (next + 1) % n
                            } else {
                                (next + n - 1) % n
                            };
                            if !bg.get(next).copied().unwrap_or(false) {
                                break;
                            }
                        }
                        *active = next;
                        children[*active]
                    }
                    _ => return false,
                };
                self.epoch += 1;
                return self.focus(target);
            }
            cur = p;
        }
    }

    /// The direct child of `container` on the focused path (None: focus
    /// is not inside it). Drives the strip focus-highlight.
    pub fn focus_child_of(&self, container: usize) -> Option<usize> {
        let mut cur = self.focused;
        loop {
            let p = self.get(cur)?.parent?;
            if p == container {
                return Some(cur);
            }
            cur = p;
        }
    }

    /// The strip rows a tabbed/stacked container carves (0 = too small
    /// to carve; children then get the full rect and no strip paints).
    fn strip_h(mode: Mode, n: u32, rect: Rect, unit: u32) -> u32 {
        let total = match mode {
            Mode::Tabbed => unit,
            Mode::Stacked => unit * n.max(1),
            _ => 0,
        };
        if total == 0 || rect.h < total + 8 || rect.w < 8 {
            0
        } else {
            total
        }
    }

    /// Every visible tabbed/stacked container with a carved strip:
    /// (container slot, strip area, mode, children, active index). The
    /// chrome painter's input (D7 glyph-free segments).
    pub fn visible_strips(&self) -> Vec<(usize, Rect, Mode, Vec<usize>, usize)> {
        self.panes
            .iter()
            .enumerate()
            .filter_map(|(i, p)| match p {
                Some(Pane {
                    kind:
                        Kind::Container {
                            mode: m @ (Mode::Tabbed | Mode::Stacked),
                            children,
                            active,
                        },
                    visible: true,
                    rect,
                    ..
                }) => {
                    // F2 structural transparency: a BACKGROUNDED leaf is not a
                    // strip segment. Filter to the effective children and remap
                    // the active into that list (clamped in range).
                    let eff: Vec<usize> = children
                        .iter()
                        .copied()
                        .filter(|&c| !self.is_bg_subtree(c))
                        .collect();
                    if eff.is_empty() {
                        return None;
                    }
                    let strip = Self::strip_h(*m, eff.len() as u32, *rect, self.metrics.tab_strip_h as u32);
                    if strip == 0 {
                        return None;
                    }
                    let eff_active = children
                        .get(*active)
                        .and_then(|&ac| eff.iter().position(|&c| c == ac))
                        .unwrap_or(0);
                    Some((
                        i,
                        Rect {
                            x: rect.x,
                            y: rect.y,
                            w: rect.w,
                            h: strip,
                        },
                        *m,
                        eff,
                        eff_active,
                    ))
                }
                _ => None,
            })
            .collect()
    }

    /// The number of visible leaves after the last `recompute` -- the
    /// border-inset decision input + the scanout-mode predicate.
    pub fn visible_leaf_count(&self) -> usize {
        self.panes
            .iter()
            .filter(|p| {
                matches!(
                    p,
                    Some(Pane {
                        kind: Kind::Leaf { .. },
                        visible: true,
                        ..
                    })
                )
            })
            .count()
    }

    /// Visible leaves that are not backgrounded: the count the chrome inset
    /// keys on. A backgrounded leaf is visible with a zero rect, so counting
    /// it would put a ring + tag bar on a lone foreground leaf the Direct
    /// scanout then shows at the display origin.
    pub fn foreground_leaf_count(&self) -> usize {
        self.panes
            .iter()
            .filter(|p| {
                matches!(
                    p,
                    Some(Pane {
                        kind: Kind::Leaf { .. },
                        visible: true,
                        backgrounded: false,
                        ..
                    })
                )
            })
            .count()
    }

    /// Recompute geometry + visibility for the whole tree inside `area`
    /// (the display under legacy, less a registered status bar -- always at
    /// the origin; the workspace between the rails under Instrument), for
    /// `profile`. The two carves are separate functions on purpose: the
    /// legacy one is the pre-profile code, byte for byte (the compose gate
    /// at 1.0 and 2.0 and ls-halcyon witness it).
    pub fn recompute(&mut self, area: Rect, gaps: u32, metrics: theme::Metrics, profile: Profile) {
        self.metrics = metrics;
        self.profile = profile;
        self.area = area;
        // Pass 1: mark everything hidden, then walk the visible tree.
        for p in self.panes.iter_mut().flatten() {
            p.visible = false;
            p.rect = Rect::ZERO;
            p.content = Rect::ZERO;
            p.tagbar = Rect::ZERO;
            p.dividers.clear();
            p.separator = Rect::ZERO;
        }
        match profile {
            Profile::Legacy => self.recompute_legacy(area.w, area.h, gaps),
            Profile::Instrument => self.recompute_instrument(area),
        }
    }

    /// The legacy carve (Tapestry G-6; HALCYON-VISUAL 2-4), on the display
    /// (`disp_w` x `disp_h` at the origin) -- unchanged by the profile.
    fn recompute_legacy(&mut self, disp_w: u32, disp_h: u32, gaps: u32) {
        // A zoomed leaf preempts the walk: it alone fills the display
        // (one visible leaf -> no inset -> borderless, the stage-0 look).
        // A stale zoom target (closed/retired) self-clears here.
        if let Some(zid) = self.zoomed_id {
            match self.slot_of_id(zid) {
                // HALCYON-WORKSPACES 4: the SAME guard the Instrument carve
                // carries, and it has to be on BOTH. `slot_of_id` is global,
                // so without it a zoom made in a dormant workspace resolves
                // here and paints that pane over the active tree. W-1a added
                // it to one carve and then claimed, in the commit body and in
                // the AUDIT-TRIGGERS row, that "the zoom" was guarded -- true
                // of the carve I was reading, false of the system, and Legacy
                // is the profile that actually ships.
                Some(z) if self.is_leaf(z) && self.in_active_root(z) => {
                    let full = Rect {
                        x: 0,
                        y: 0,
                        w: disp_w,
                        h: disp_h,
                    };
                    let p = self.get_mut(z).unwrap();
                    p.visible = true;
                    p.rect = full;
                    p.content = full;
                    return;
                }
                _ => self.zoomed_id = None,
            }
        }
        let root = self.root();
        self.layout_pane(
            root,
            Rect {
                x: 0,
                y: 0,
                w: disp_w,
                h: disp_h,
            },
        );
        // Pass 2: the content inset -- the Daylight chrome ring per leaf iff
        // >1 FOREGROUND leaf visible (a backgrounded zero-rect leaf does not
        // count). A single fullscreen leaf stays borderless (the stage-0
        // look). The ring = floor(`gaps`, the tunable gap) +
        // bevel(2) + hairline(1); the bevel+hairline is fixed structural
        // chrome (HALCYON-VISUAL section 2/2.4), the floor is the tunable
        // inter-pane gap (section 2.3 -- at gaps=1 the two abutting floors
        // give the 2px inter-pane floor).
        let chrome = (self.metrics.bevel + self.metrics.hairline) as u32;
        let inset = if self.foreground_leaf_count() > 1 {
            gaps + chrome
        } else {
            0
        };
        // The Daylight tag bar (HALCYON-VISUAL 3.2/4): every inset leaf carves a
        // `header_h` strip off the TOP of its inner rect (inside the ring),
        // above the client content. Gated with the ring (>1 leaf) -- a single
        // fullscreen leaf stays borderless AND bar-free (stage-0). A leaf too
        // short to spare the strip stays bar-free (the `+ tag_h` client floor,
        // mirroring strip_h's `+ 8`).
        let tag_h = self.metrics.header_h as u32;
        for p in self.panes.iter_mut().flatten() {
            if !p.visible {
                continue;
            }
            let r = p.rect;
            if inset > 0
                && matches!(p.kind, Kind::Leaf { .. })
                && r.w > 2 * inset
                && r.h > 2 * inset
            {
                let mut c = Rect {
                    x: r.x + inset,
                    y: r.y + inset,
                    w: r.w - 2 * inset,
                    h: r.h - 2 * inset,
                };
                if c.h > tag_h + tag_h {
                    p.tagbar = Rect {
                        x: c.x,
                        y: c.y,
                        w: c.w,
                        h: tag_h,
                    };
                    c.y += tag_h;
                    c.h -= tag_h;
                }
                p.content = c;
            } else {
                p.content = r;
            }
        }
    }


    /// The Instrument carve (HALCYON-INSTRUMENT 5): the workspace `area`
    /// (between the rails) padded `outer_pad`, then the tree -- a split
    /// container divides its rect among its foreground children by weight
    /// with a track between each pair (5.2); a stack, and every lone leaf,
    /// which renders as a stack of one (6.1), is a 1 px frame holding its
    /// tiles' headers and the open tile's body (5.4); a tabbed container is
    /// the legacy mode, its active child shown as a stack of one (6.6).
    /// Every rect is clipped to its parent's, so nothing published leaves
    /// the display however small the area gets. A zoomed leaf fills the
    /// workspace alone, frame-less (5.6).
    fn recompute_instrument(&mut self, area: Rect) {
        if let Some(zid) = self.zoomed_id {
            match self.slot_of_id(zid) {
                // HALCYON-WORKSPACES 4: `slot_of_id` is GLOBAL, so without
                // this the zoom set in one workspace would still resolve
                // after a switch and zoom another workspace's pane over the
                // active tree. A zoom belongs to the tree it was made in.
                Some(z) if self.is_leaf(z) && self.in_active_root(z) => {
                    let p = self.get_mut(z).unwrap();
                    p.visible = true;
                    p.rect = area;
                    p.content = area;
                    return;
                }
                _ => self.zoomed_id = None,
            }
        }
        let root = self.root();
        let pad = self.metrics.outer_pad.max(0) as u32;
        self.carve(root, inset(area, pad));
    }

    fn carve(&mut self, slot: usize, rect: Rect) {
        enum Next {
            Leaf,
            Split(Mode, Vec<usize>),
            Tab(Vec<usize>, usize),
            Stack(Vec<usize>, usize),
        }
        let next = match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Leaf { .. }) => Next::Leaf,
            Some(Kind::Container {
                mode,
                children,
                active,
            }) => match mode {
                Mode::SplitH | Mode::SplitV => Next::Split(*mode, children.clone()),
                Mode::Tabbed => Next::Tab(children.clone(), *active),
                Mode::Stacked => Next::Stack(children.clone(), *active),
            },
            None => return,
        };
        match next {
            Next::Leaf => self.place_frame(rect, &[slot], 0),
            Next::Split(mode, children) => {
                self.show_container(slot, rect);
                if children.is_empty() {
                    return;
                }
                // F2 structural transparency, exactly as the legacy division:
                // a backgrounded subtree is out of the division (a zero rect,
                // kept visible) unless every child is.
                let divide = Self::divide_list(&children, |c| self.is_bg_subtree(c));
                for &c in children.iter() {
                    if !divide.contains(&c) {
                        self.carve(c, Rect::ZERO);
                    }
                }
                let horizontal = mode == Mode::SplitH;
                let t = self.metrics.track.max(0) as u32;
                let weights: Vec<u16> = divide
                    .iter()
                    .map(|&c| self.get(c).map_or(DEFAULT_WEIGHT, |p| p.weight))
                    .collect();
                let minima: Vec<u32> = divide
                    .iter()
                    .map(|&c| {
                        let (w, h) = self.min_size_hyp(c, None);
                        if horizontal {
                            w
                        } else {
                            h
                        }
                    })
                    .collect();
                let (origin, extent) = if horizontal {
                    (rect.x, rect.w)
                } else {
                    (rect.y, rect.h)
                };
                let spans = carve::split_spans(origin, extent, t, &weights, &minima);
                let mut dividers: Vec<Rect> = Vec::new();
                for (i, &c) in divide.iter().enumerate() {
                    let sp = spans[i];
                    let child = if horizontal {
                        Rect {
                            x: sp.start,
                            y: rect.y,
                            w: sp.len(),
                            h: rect.h,
                        }
                    } else {
                        Rect {
                            x: rect.x,
                            y: sp.start,
                            w: rect.w,
                            h: sp.len(),
                        }
                    };
                    self.carve(c, child.intersect(rect));
                    if i + 1 < divide.len() {
                        let track = if horizontal {
                            Rect {
                                x: sp.end,
                                y: rect.y,
                                w: t,
                                h: rect.h,
                            }
                        } else {
                            Rect {
                                x: rect.x,
                                y: sp.end,
                                w: rect.w,
                                h: t,
                            }
                        };
                        dividers.push(track.intersect(rect));
                    }
                }
                self.get_mut(slot).unwrap().dividers = dividers;
            }
            Next::Tab(children, active) => {
                self.show_container(slot, rect);
                let eff: Vec<usize> = children
                    .iter()
                    .copied()
                    .filter(|&c| !self.is_bg_subtree(c))
                    .collect();
                let shown = children
                    .get(active)
                    .copied()
                    .filter(|&a| !self.is_bg_subtree(a))
                    .or_else(|| eff.first().copied());
                if let Some(a) = shown {
                    self.place_frame(rect, &[a], 0);
                }
            }
            Next::Stack(children, active) => {
                self.show_container(slot, rect);
                let eff: Vec<usize> = children
                    .iter()
                    .copied()
                    .filter(|&c| !self.is_bg_subtree(c))
                    .collect();
                if eff.is_empty() {
                    return;
                }
                let open = children
                    .get(active)
                    .and_then(|&a| eff.iter().position(|&c| c == a))
                    .unwrap_or(0);
                self.place_frame(rect, &eff, open);
            }
        }
    }

    fn show_container(&mut self, slot: usize, rect: Rect) {
        if let Some(p) = self.get_mut(slot) {
            p.visible = true;
            p.rect = rect;
            p.content = rect;
        }
    }

    /// A frame at `rect` holding `tiles` (a stack's effective children, or
    /// one lone leaf) with `tiles[open]` expanded: the 1 px frame, each
    /// tile's header rect (5.4: the collapsed ones stacked before and after
    /// the open one, the open one directly above its body), the open body.
    /// A collapsed leaf is hidden with a ZERO body and its header; a tile
    /// that is itself a container takes the header slot and, when open, has
    /// its subtree carved into the body.
    fn place_frame(&mut self, rect: Rect, tiles: &[usize], open: usize) {
        let m = self.metrics;
        let f = m.frame.max(0) as u32;
        let inner = inset(rect, f);
        let (headers, body) = carve::stack_alloc(
            inner.y,
            inner.h,
            tiles.len(),
            open,
            m.header_h.max(0) as u32,
            m.hairline.max(0) as u32,
        );
        let body_rect = Rect {
            x: inner.x,
            y: body.start,
            w: inner.w,
            h: body.len(),
        }
        .intersect(inner);
        // HALCYON-INSTRUMENT 14.6, the empty pane: a lone EMPTY leaf is the
        // N = 0 exception -- no 32 px header (no tile exists); its `tagbar`
        // is the whole interior, where its chrome surface paints the
        // placard, and its body is ZERO. An empty leaf inside a stack of
        // several keeps a header row like any tile.
        if tiles.len() == 1 && self.is_empty_leaf(tiles[0]) {
            let p = self.get_mut(tiles[0]).unwrap();
            // Dormant when the clip left it no interior (r1 A-F1; r2 C-F1:
            // judged on the carved placard, not the frame rect -- a rect of
            // 2 px has a frame and nothing inside it).
            p.visible = !inner.is_empty();
            p.rect = rect;
            p.tagbar = inner;
            p.content = Rect::ZERO;
            return;
        }
        // The 1 px separator after the open body: reserved by stack_alloc
        // between the body and the header that follows it (none when the
        // open tile is the last).
        let sep = if open + 1 < tiles.len() {
            Rect {
                x: inner.x,
                y: body.end,
                w: inner.w,
                h: m.hairline.max(0) as u32,
            }
            .intersect(inner)
        } else {
            Rect::ZERO
        };
        for (i, &t) in tiles.iter().enumerate() {
            let header = Rect {
                x: inner.x,
                y: headers[i].start,
                w: inner.w,
                h: headers[i].len(),
            }
            .intersect(inner);
            let is_open = i == open;
            if self.is_leaf(t) {
                let p = self.get_mut(t).unwrap();
                // A tile the clip left no BODY (the tree outgrew the minima
                // through a path the fits-check does not guard: a scale step,
                // a display resize, a restore onto a smaller display) is
                // dormant exactly like a collapsed one: nothing hosted
                // composes there and it may not keep focus (r1 A-F1). Judged
                // on the carved body, not the frame rect: a rect of 34 rows
                // holds a frame and a header and no body (r2 C-F1).
                p.visible = is_open && !body_rect.is_empty();
                p.rect = rect;
                p.tagbar = header;
                p.content = if is_open { body_rect } else { Rect::ZERO };
                p.separator = if is_open { sep } else { Rect::ZERO };
            } else {
                if is_open {
                    self.carve(t, body_rect);
                }
                let p = self.get_mut(t).unwrap();
                p.tagbar = header;
                p.separator = if is_open { sep } else { Rect::ZERO };
                if !is_open {
                    p.rect = rect;
                }
            }
        }
    }

    /// A subtree's minimum outer size under Instrument (5.2): a pane is
    /// `min_pane_w` wide and, as a stack of N tiles, `2 + 32 N + 1 + 54`
    /// tall (the separator only when N > 1); a split sums its foreground
    /// children's minima along its axis plus the tracks and takes the max
    /// across; a container tile inside a stack or tab adds its own minimum
    /// to the body's. With `hyp = Some((leaf, mode))` the leaf is judged
    /// as if already split in `mode` -- flattened into a same-mode parent
    /// as one more sibling, nested as a fresh two-way container otherwise
    /// -- which is how a split is refused before it is made.
    fn min_size_hyp(&self, slot: usize, hyp: Option<(usize, Mode)>) -> (u32, u32) {
        let m = self.metrics;
        let f = m.frame.max(0) as u32;
        let t = m.track.max(0) as u32;
        let hdr = m.header_h.max(0) as u32;
        let sep = m.hairline.max(0) as u32;
        let pane_w = m.min_pane_w.max(0) as u32;
        let body_h = m.min_body_h.max(0) as u32;
        let leaf_min = (pane_w, carve::stack_min_h(1, hdr, sep, body_h, f));
        let nested = |mode: Mode| -> (u32, u32) {
            match mode {
                Mode::SplitH => (2 * leaf_min.0 + t, leaf_min.1),
                Mode::SplitV => (leaf_min.0, 2 * leaf_min.1 + t),
                Mode::Tabbed => leaf_min,
                Mode::Stacked => (pane_w, carve::stack_min_h(2, hdr, sep, body_h, f)),
            }
        };
        let is_hyp = |c: usize| matches!(hyp, Some((l, _)) if l == c);
        match self.get(slot).map(|p| &p.kind) {
            None => (0, 0),
            Some(Kind::Leaf { .. }) => match hyp {
                Some((l, mode)) if l == slot => nested(mode),
                _ => leaf_min,
            },
            Some(Kind::Container { mode, children, .. }) => {
                let eff: Vec<usize> = {
                    let fg: Vec<usize> = children
                        .iter()
                        .copied()
                        .filter(|&c| !self.is_bg_subtree(c))
                        .collect();
                    if fg.is_empty() {
                        children.clone()
                    } else {
                        fg
                    }
                };
                match mode {
                    Mode::SplitH | Mode::SplitV => {
                        let horizontal = *mode == Mode::SplitH;
                        let mut parts: Vec<(u32, u32)> = Vec::with_capacity(eff.len() + 1);
                        for &c in &eff {
                            // The hypothetical leaf under a same-mode split
                            // flattens: one more leaf beside it.
                            let flat = matches!(hyp, Some((l, hm)) if l == c && hm == *mode);
                            if flat {
                                parts.push(leaf_min);
                                parts.push(leaf_min);
                            } else {
                                parts.push(self.min_size_hyp(c, hyp));
                            }
                        }
                        let n = parts.len() as u32;
                        let (along, across) = parts.iter().fold((0u32, 0u32), |(a, x), &(w, h)| {
                            let (pa, px) = if horizontal { (w, h) } else { (h, w) };
                            (a.saturating_add(pa), x.max(px))
                        });
                        let along = along.saturating_add(n.saturating_sub(1).saturating_mul(t));
                        if horizontal {
                            (along, across)
                        } else {
                            (across, along)
                        }
                    }
                    Mode::Stacked => {
                        let mut n = eff.len();
                        let mut w = pane_w;
                        let mut body = body_h;
                        for &c in &eff {
                            if matches!(hyp, Some((l, Mode::Stacked)) if l == c) {
                                n += 1; // one more tile in this stack
                                continue;
                            }
                            if self.is_leaf(c) && !is_hyp(c) {
                                continue;
                            }
                            // A container tile (or a leaf about to become
                            // one): its subtree must fit the body.
                            let (cw, ch) = self.min_size_hyp(c, hyp);
                            w = w.max(cw.saturating_add(2 * f));
                            body = body.max(ch);
                        }
                        (w, carve::stack_min_h(n, hdr, sep, body, f))
                    }
                    Mode::Tabbed => {
                        let mut w = pane_w;
                        let mut body = body_h;
                        for &c in &eff {
                            if matches!(hyp, Some((l, Mode::Tabbed)) if l == c) {
                                continue; // one more tab: nothing grows
                            }
                            if self.is_leaf(c) && !is_hyp(c) {
                                continue;
                            }
                            let (cw, ch) = self.min_size_hyp(c, hyp);
                            w = w.max(cw.saturating_add(2 * f));
                            body = body.max(ch);
                        }
                        (w, carve::stack_min_h(1, hdr, sep, body, f))
                    }
                }
            }
        }
    }

    /// The tree's minimum outer size under the current carve (5.2); (0, 0)
    /// under legacy, which has no minima.
    pub fn min_size(&self) -> (u32, u32) {
        if self.profile != Profile::Instrument {
            return (0, 0);
        }
        self.min_size_hyp(self.root(), None)
    }

    /// HALCYON-INSTRUMENT 5.2: would splitting leaf `slot` in `mode` keep
    /// every minimum inside the workspace? Judged on the tree as it WOULD
    /// be, before anything changes, so a refusal leaves the tree untouched.
    /// Always true under legacy.
    pub fn split_fits(&self, slot: usize, mode: Mode) -> bool {
        if self.profile != Profile::Instrument || !self.is_leaf(slot) {
            return true;
        }
        let pad = self.metrics.outer_pad.max(0) as u32;
        let root = inset(self.area, pad);
        let (w, h) = self.min_size_hyp(self.root(), Some((slot, mode)));
        w <= root.w && h <= root.h
    }

    /// The children a split container divides (the list its tracks index,
    /// 5.5): the foreground ones, or all of them when every child is
    /// backgrounded (the F2 rule `carve` applies).
    fn divide_list(children: &[usize], is_bg: impl Fn(usize) -> bool) -> Vec<usize> {
        let fg: Vec<usize> = children.iter().copied().filter(|&c| !is_bg(c)).collect();
        if fg.is_empty() {
            children.to_vec()
        } else {
            fg
        }
    }

    /// HALCYON-INSTRUMENT 9.2 (I-6): the split container's divided children
    /// in track order -- track `i` separates `divide_of(slot)[i]` from
    /// `[i + 1]`. Empty for a leaf, a stack, a tab.
    pub fn divide_of(&self, slot: usize) -> Vec<usize> {
        match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container {
                mode: Mode::SplitH | Mode::SplitV,
                children,
                ..
            }) => Self::divide_list(children, |c| self.is_bg_subtree(c)),
            _ => Vec::new(),
        }
    }

    /// The divider track under display point (x, y): the split container's
    /// slot and the track's index in its `dividers` (9.1: a track routes to
    /// the compositor itself). Only a carved container publishes tracks
    /// (`recompute` clears them first), so a hidden or zoomed-over track is
    /// never hit.
    pub fn track_at(&self, x: u32, y: u32) -> Option<(usize, usize)> {
        // A per-motion path: no allocation (as `surface_at`).
        self.panes.iter().enumerate().find_map(|(slot, p)| {
            let p = p.as_ref()?;
            if !p.visible {
                return None;
            }
            p.dividers.iter().position(|d| d.contains(x, y)).map(|i| (slot, i))
        })
    }

    /// The pair of children track `idx` of `slot` separates, with the
    /// facts a drag needs along the container's axis: (first child, second
    /// child, origin, first extent, second extent, first minimum, second
    /// minimum, horizontal). None when the track does not exist.
    fn track_pair(&self, slot: usize, idx: usize) -> Option<(usize, usize, u32, u32, u32, u32, u32, bool)> {
        let horizontal = match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container { mode: Mode::SplitH, .. }) => true,
            Some(Kind::Container { mode: Mode::SplitV, .. }) => false,
            _ => return None,
        };
        if self.get(slot).map_or(true, |p| idx >= p.dividers.len()) {
            return None;
        }
        let divide = self.divide_of(slot);
        let (&a, &b) = (divide.get(idx)?, divide.get(idx + 1)?);
        let (ra, rb) = (self.get(a)?.rect, self.get(b)?.rect);
        let along = |r: Rect| if horizontal { (r.x, r.w) } else { (r.y, r.h) };
        let (origin, ea) = along(ra);
        let (_, eb) = along(rb);
        let min_along = |c: usize| {
            let (w, h) = self.min_size_hyp(c, None);
            if horizontal {
                w
            } else {
                h
            }
        };
        Some((a, b, origin, ea, eb, min_along(a), min_along(b), horizontal))
    }

    /// Re-weight a split container so its children's weights ARE their
    /// pixel extents along the axis, with the pair `(a, b)` at `(ea, eb)`:
    /// the extents are a fixed point of the flex rule (they sum to the
    /// usable extent and each clears its minimum), so the next carve lays
    /// the boundary exactly where the pair says, and the neighbours keep
    /// their extents to the pixel. Weights are `1..=65535` (a display is
    /// narrower than that; a clipped child of 0 counts as 1). True when
    /// some weight changed.
    fn set_pair_extents(&mut self, slot: usize, a: usize, b: usize, ea: u32, eb: u32, horizontal: bool) -> bool {
        let mut changed = false;
        for c in self.divide_of(slot) {
            let e = if c == a {
                ea
            } else if c == b {
                eb
            } else {
                let r = self.get(c).map_or(Rect::ZERO, |p| p.rect);
                if horizontal {
                    r.w
                } else {
                    r.h
                }
            };
            let w = e.clamp(1, u16::MAX as u32) as u16;
            let before = self.get(c).map(|p| p.weight);
            if self.set_weight(c, w) && before != Some(w) {
                changed = true;
            }
        }
        changed
    }

    /// HALCYON-INSTRUMENT 9.2 (I-6): a divider drag -- track `idx` of split
    /// container `slot` follows display point `pos` along the axis. The
    /// two adjacent children take `carve::drag_pair`'s extents (the
    /// mockup's ratio, the 22..78 band, the minima); the rest of the
    /// container keeps its extents. `Refused` when the pair is in the
    /// carve's overflow (nothing changes); `Unchanged` when the clamps put
    /// the boundary where it already is.
    pub fn drag_track(&mut self, slot: usize, idx: usize, pos: (u32, u32)) -> DragVerdict {
        let Some((a, b, origin, ea, eb, ma, mb, horizontal)) = self.track_pair(slot, idx) else {
            return DragVerdict::Refused;
        };
        let t = self.metrics.track.max(0) as u32;
        let p = if horizontal { pos.0 } else { pos.1 } as i64;
        match carve::drag_pair(origin, ea, eb, t, ma, mb, p) {
            Some((na, nb)) if self.set_pair_extents(slot, a, b, na, nb, horizontal) => DragVerdict::Changed,
            Some(_) => DragVerdict::Unchanged,
            None => DragVerdict::Refused,
        }
    }

    /// Double-click on a track (9.2): the two adjacent children equalised
    /// (`carve::equalise_pair`, the same clamps); the neighbours untouched.
    pub fn equalise_track(&mut self, slot: usize, idx: usize) -> DragVerdict {
        let Some((a, b, _, ea, eb, ma, mb, horizontal)) = self.track_pair(slot, idx) else {
            return DragVerdict::Refused;
        };
        match carve::equalise_pair(ea, eb, ma, mb) {
            Some((na, nb)) if self.set_pair_extents(slot, a, b, na, nb, horizontal) => DragVerdict::Changed,
            Some(_) => DragVerdict::Unchanged,
            None => DragVerdict::Refused,
        }
    }

    /// Does the tree's minimum fit the padded workspace (5.2)? Always under
    /// legacy.
    fn min_fits(&self) -> bool {
        if self.profile != Profile::Instrument {
            return true;
        }
        let pad = self.metrics.outer_pad.max(0) as u32;
        let root = inset(self.area, pad);
        let (w, h) = self.min_size();
        w <= root.w && h <= root.h
    }

    /// HALCYON-INSTRUMENT 5.2 (r1 A-F1's owed half, I-6): would `mutate`
    /// leave the tree past the minima that it clears today? Judged on a
    /// COPY, before anything changes, so a refusal leaves the tree
    /// untouched. A tree already past its minima (a layout restored onto a
    /// smaller display) stays mutable: only a mutation that CREATES the
    /// overflow is refused, since the moves that would cure one are also
    /// mutations. True when the mutation itself is a no-op (the real call
    /// answers with its own refusal). Always true under legacy.
    pub fn fits_after(&self, mutate: impl FnOnce(&mut Layout) -> bool) -> bool {
        if self.profile != Profile::Instrument || !self.min_fits() {
            return true;
        }
        let mut trial = self.clone();
        if !mutate(&mut trial) {
            return true;
        }
        trial.min_fits()
    }

    fn layout_pane(&mut self, slot: usize, rect: Rect) {
        enum Next {
            Done,
            Split(Mode, Vec<usize>),
            Tab(Mode, Vec<usize>, usize),
        }
        let next = {
            let p = match self.get_mut(slot) {
                Some(p) => p,
                None => return,
            };
            p.visible = true;
            p.rect = rect;
            match &p.kind {
                Kind::Leaf { .. } => Next::Done,
                Kind::Container {
                    mode,
                    children,
                    active,
                } => match mode {
                    // Tab/stack: only the active child is visible, in the
                    // rect below the indicator strip (G-6c; strip_h = 0
                    // when the rect is too small to carve).
                    // The effective children (backgrounded leaves skipped) and
                    // the strip carve are resolved OUTSIDE this `&mut p` borrow
                    // -- `is_bg_leaf` needs `&self` -- so hand them to the
                    // `Next::Tab` arm below.
                    m @ (Mode::Tabbed | Mode::Stacked) => Next::Tab(*m, children.clone(), *active),
                    m => Next::Split(*m, children.clone()),
                },
            }
        };
        match next {
            Next::Done => {}
            Next::Tab(mode, children, active) => {
                // F2 structural transparency: a BACKGROUNDED leaf is not a
                // tab/stack segment and never the shown child. Carve the strip
                // for the EFFECTIVE count and show the effective active child
                // (the raw active never lands on a backgrounded leaf after
                // `tab_cycle`, but fall back to the first effective child).
                let eff: Vec<usize> = children
                    .iter()
                    .copied()
                    .filter(|&c| !self.is_bg_subtree(c))
                    .collect();
                let strip = Self::strip_h(mode, eff.len() as u32, rect, self.metrics.tab_strip_h as u32);
                let shown = children
                    .get(active)
                    .copied()
                    .filter(|&a| !self.is_bg_subtree(a))
                    .or_else(|| eff.first().copied());
                if let Some(a) = shown {
                    self.layout_pane(
                        a,
                        Rect {
                            x: rect.x,
                            y: rect.y + strip,
                            w: rect.w,
                            h: rect.h - strip,
                        },
                    );
                }
            }
            Next::Split(mode, children) => {
                if children.is_empty() {
                    return;
                }
                // F2 (d-1b tiling completion): exclude a BACKGROUNDED leaf (a
                // non-session renderer while a session holds the display) from
                // the division -- it gets a ZERO rect (kept visible, so the
                // post-recompute vis/bg accounting is untouched) and the
                // foreground siblings divide the FULL rect. Guard: if EVERY
                // child is backgrounded, divide among all (never blank a
                // container). Only the Split arm consults `backgrounded`; a
                // Tab/Stack ACTIVE child rides the One path above regardless,
                // so a backgrounded-but-active leaf is shown, never blanked.
                let divide: Vec<usize> = {
                    let fg: Vec<usize> = children
                        .iter()
                        .copied()
                        .filter(|&c| !self.is_bg_subtree(c))
                        .collect();
                    if fg.is_empty() {
                        children.clone()
                    } else {
                        fg
                    }
                };
                for &c in children.iter() {
                    if !divide.contains(&c) {
                        self.layout_pane(c, Rect::ZERO);
                    }
                }
                let n = divide.len() as u32;
                if mode == Mode::SplitH {
                    let each = rect.w / n;
                    let mut x = rect.x;
                    for (i, &c) in divide.iter().enumerate() {
                        let w = if i as u32 == n - 1 {
                            rect.x + rect.w - x
                        } else {
                            each
                        };
                        self.layout_pane(
                            c,
                            Rect {
                                x,
                                y: rect.y,
                                w,
                                h: rect.h,
                            },
                        );
                        x += w;
                    }
                } else {
                    let each = rect.h / n;
                    let mut y = rect.y;
                    for (i, &c) in divide.iter().enumerate() {
                        let h = if i as u32 == n - 1 {
                            rect.y + rect.h - y
                        } else {
                            each
                        };
                        self.layout_pane(
                            c,
                            Rect {
                                x: rect.x,
                                y,
                                w: rect.w,
                                h,
                            },
                        );
                        y += h;
                    }
                }
            }
        }
    }

    /// Visible hosted surfaces: (leaf slot, surface index, content rect).
    pub fn visible_hosted(&self) -> Vec<(usize, usize, Rect)> {
        self.panes
            .iter()
            .enumerate()
            .filter_map(|(i, p)| match p {
                Some(Pane {
                    kind: Kind::Leaf { surface: Some(n) },
                    visible: true,
                    content,
                    ..
                }) => Some((i, *n, *content)),
                _ => None,
            })
            .collect()
    }

    /// F2: every hosted leaf (leaf slot, surface index), regardless of
    /// visibility -- the pre-recompute input for the backgrounding decision.
    /// It must NOT depend on visibility (recompute has not run yet); the
    /// visibility-filtered twin is `visible_hosted`.
    pub fn hosted_leaves(&self) -> Vec<(usize, usize)> {
        self.panes
            .iter()
            .enumerate()
            .filter_map(|(i, p)| match p {
                Some(Pane {
                    kind: Kind::Leaf { surface: Some(n) },
                    ..
                }) => Some((i, *n)),
                _ => None,
            })
            .collect()
    }

    /// F2: stamp the backgrounded set on every pane (true iff its slot is in
    /// `bg`), called by reconcile BEFORE recompute. One pass clears stale
    /// flags and sets the current set, so a leaf that stops being backgrounded
    /// (the session logs out) is un-stamped the same reconcile.
    pub fn apply_backgrounded(&mut self, bg: &[usize]) {
        for (i, p) in self.panes.iter_mut().enumerate() {
            if let Some(p) = p {
                p.backgrounded = bg.contains(&i);
            }
        }
        // HALCYON-WORKSPACES 4: the d-1b predicate, ONE WORKSPACE WIDER --
        // every pane of an inactive root is dormant too. Stamped here rather
        // than by the caller because only the tree knows its own roots, and a
        // caller-supplied set would be a second copy able to fall out of step.
        for r in self.inactive_roots() {
            self.stamp_bg_subtree(r);
        }
    }

    /// Stamp `backgrounded` on a whole subtree (an inactive workspace's).
    fn stamp_bg_subtree(&mut self, slot: usize) {
        let kids: Vec<usize> = match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container { children, .. }) => children.clone(),
            _ => Vec::new(),
        };
        if let Some(p) = self.get_mut(slot) {
            p.backgrounded = true;
        }
        for c in kids {
            self.stamp_bg_subtree(c);
        }
    }

    /// F2: is `slot` a backgrounded LEAF (the structural-transparency
    /// predicate)? Restricted to leaves: a container's backgrounding is its
    /// leaves'. This is the TREE flag (owner-based, set before recompute),
    /// stable across a tab hiding the leaf -- unlike `Surface.backgrounded`,
    /// which is visibility-derived and clears once a tab hides the leaf.
    pub fn is_bg_leaf(&self, slot: usize) -> bool {
        self.get(slot)
            .is_some_and(|p| p.backgrounded && matches!(p.kind, Kind::Leaf { .. }))
    }

    /// Is `slot` backgrounded as a whole: a backgrounded leaf, or a container
    /// whose every child is. The transparency predicate for a CHILD of a
    /// division/tab/cycle -- a system-only container beside a session must
    /// vanish exactly like a lone system leaf does.
    pub fn is_bg_subtree(&self, slot: usize) -> bool {
        match self.get(slot) {
            Some(Pane {
                kind: Kind::Leaf { .. },
                backgrounded,
                ..
            }) => *backgrounded,
            Some(Pane {
                kind: Kind::Container { children, .. },
                ..
            }) => !children.is_empty() && children.iter().all(|&c| self.is_bg_subtree(c)),
            None => false,
        }
    }

    /// The focused leaf's hosted surface (input routing).
    pub fn focused_surface(&self) -> Option<usize> {
        self.leaf_surface(self.focused)
    }

    /// The layout text (the `layout` file read): one pane per line,
    /// depth-indented; `*` marks the focused leaf.
    pub fn render_text(&self) -> String {
        let mut s = String::new();
        // S4: the ascending list of LIVE NUMBERS, never a count. The set is
        // sparse, so a reader told "3" cannot know whether that means 1,2,3
        // or 1,3,4 -- and the rail has to label its chips from this.
        let mut nums = String::new();
        for (i, w) in self.workspaces.iter().enumerate() {
            if i > 0 {
                nums.push(',');
            }
            let _ = core::fmt::write(&mut nums, format_args!("{}", w.number));
        }
        let _ = core::fmt::write(
            &mut s,
            format_args!(
                // HALCYON-WORKSPACES 4: the ratified channel for the bar and
                // the tool -- one header line, no `workspace/` subtree.
                // `workspaces` is the ascending comma-separated list of live
                // workspace NUMBERS, and `active` is the active workspace's
                // NUMBER (S4, 2026-09-15): both are identities, never
                // positions. The rows below stay the ACTIVE root's.
                "epoch {} focused {} workspaces {} active {}",
                self.epoch,
                self.id_of(self.focused).unwrap_or(0),
                nums,
                self.active_number()
            ),
        );
        if let Some(z) = self.zoomed_id {
            let _ = core::fmt::write(&mut s, format_args!(" zoomed {}", z));
        }
        s.push('\n');
        self.render_pane(&mut s, self.root(), 0);
        s
    }

    fn render_pane(&self, s: &mut String, slot: usize, depth: usize) {
        let p = match self.get(slot) {
            Some(p) => p,
            None => return,
        };
        for _ in 0..depth {
            s.push_str("  ");
        }
        let star = if slot == self.focused { "*" } else { "" };
        match &p.kind {
            Kind::Leaf { surface } => {
                let _ = core::fmt::write(s, format_args!("{}{} leaf", p.id, star));
                match surface {
                    Some(n) => {
                        let _ = core::fmt::write(s, format_args!(" surface={}", n));
                    }
                    None => s.push_str(" empty"),
                }
            }
            Kind::Container {
                mode,
                children,
                active,
            } => {
                let _ = core::fmt::write(
                    s,
                    format_args!(
                        "{}{} {} n={} active={}",
                        p.id,
                        star,
                        mode.name(),
                        children.len(),
                        active
                    ),
                );
            }
        }
        let c = p.content;
        let _ = core::fmt::write(s, format_args!(" [{},{},{},{}]", c.x, c.y, c.w, c.h));
        // HALCYON-INSTRUMENT 5.3: a non-default weight rides after the rect
        // (`layout save` reads it; every older reader reads past it), so
        // the equal-weight dump is byte-identical to the pre-I-2 one.
        if p.weight != DEFAULT_WEIGHT {
            let _ = core::fmt::write(s, format_args!(" w={}", p.weight));
        }
        let _ = core::fmt::write(s, format_args!("{}\n", if p.visible { "" } else { " hidden" }));
        if let Kind::Container { children, .. } = &p.kind {
            for &c in children {
                self.render_pane(s, c, depth + 1);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    /// A well-formed status-bar registration on an undeclared display: the
    /// console renderer's own case. Every test below moves exactly ONE field
    /// off this base, so a verdict change names its cause.
    fn base() -> StatusReq {
        StatusReq {
            bar_registered: false,
            w: 1280,
            h: 20,
            disp_w: 1280,
            disp_h: 800,
            status_h: 20,
            session_declared: false,
            requester_is_session: false,
        }
    }

    #[test]
    fn the_console_may_take_the_bar_while_no_session_is_declared() {
        assert_eq!(admit_status_bar(&base()), StatusAdmit::Admit);
    }

    #[test]
    fn a_second_bar_is_refused() {
        let r = StatusReq {
            bar_registered: true,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::Malformed);
    }

    #[test]
    fn the_bar_is_exactly_the_strip() {
        // Not the display width -- neither narrower nor wider, since the bar
        // is never cropped or letterboxed (HALCYON.md 13.6).
        for w in [1279u32, 1281] {
            let r = StatusReq { w, ..base() };
            assert_eq!(admit_status_bar(&r), StatusAdmit::Malformed, "w={}", w);
        }
        // Not the one vertical unit.
        for h in [19u32, 21] {
            let r = StatusReq { h, ..base() };
            assert_eq!(admit_status_bar(&r), StatusAdmit::Malformed, "h={}", h);
        }
    }

    #[test]
    fn a_display_no_taller_than_its_own_strip_has_no_room_for_one() {
        // The carve would leave zero rows for content, so the request is
        // refused rather than producing a bar-only display.
        for disp_h in [19u32, 20] {
            let r = StatusReq {
                disp_h,
                h: 20,
                ..base()
            };
            assert_eq!(admit_status_bar(&r), StatusAdmit::Malformed, "disp_h={}", disp_h);
        }
        // One row of content is enough.
        let r = StatusReq {
            disp_h: 21,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::Admit);
    }

    /// THE HANDOVER RULE (@9d5f38ee), which had no witness of any kind until
    /// this test: while a session is declared, the backgrounded SYSTEM
    /// console renderer may not take the display's status-bar slot. Retiring
    /// its bar at the declare does not close this on its own -- the console
    /// re-arms on the relayout the retire causes and races for the slot.
    #[test]
    fn a_system_renderer_may_not_take_a_declared_sessions_bar() {
        let r = StatusReq {
            session_declared: true,
            requester_is_session: false,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::NotYours);
    }

    /// The positive control one variable away: the SAME declared display
    /// admits the SESSION's own bar. Without this, the test above is
    /// satisfied by a rule that refuses every request once a session exists.
    #[test]
    fn the_declared_session_may_take_its_own_bar() {
        let r = StatusReq {
            session_declared: true,
            requester_is_session: true,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::Admit);
    }

    /// The other control: a session-principal requester is not what admits
    /// the base case, so `requester_is_session` alone changes nothing while
    /// no session is declared.
    #[test]
    fn the_principal_axis_is_inert_while_no_session_is_declared() {
        let r = StatusReq {
            requester_is_session: true,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::Admit);
    }

    /// Order matters and is load-bearing at the call site: the malformed
    /// arms answer E_INVAL and the ownership arm answers E_PERM, so a
    /// refactor that judged ownership first would change the errno a client
    /// sees for a malformed request. Pinned here because nothing else looks.
    #[test]
    fn malformed_is_judged_before_ownership() {
        let r = StatusReq {
            bar_registered: true,
            session_declared: true,
            requester_is_session: false,
            ..base()
        };
        assert_eq!(admit_status_bar(&r), StatusAdmit::Malformed);
    }

    // ---- HALCYON-INSTRUMENT 8: the top rail's admission (I-4) --------------

    /// A well-formed rail registration on an undeclared Instrument display:
    /// the console renderer's own case. Every test below moves exactly ONE
    /// field off this base.
    fn rail_base() -> RailReq {
        RailReq {
            rail_registered: false,
            w: 1280,
            h: 34,
            disp_w: 1280,
            disp_h: 800,
            rail_h: 34,
            instrument: true,
            session_declared: false,
            requester_is_session: false,
        }
    }

    #[test]
    fn the_console_may_take_the_rail_while_no_session_is_declared() {
        assert_eq!(admit_rail(&rail_base()), StatusAdmit::Admit);
    }

    #[test]
    fn no_rail_exists_under_legacy() {
        // The legacy carve reserves no top strip: refused as MALFORMED (there
        // is no rect to be exactly), whoever asks -- the renderer included.
        let r = RailReq { instrument: false, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::Malformed);
        // And a zero `rail_h` (the legacy table's value) is the same refusal
        // even if the profile word said otherwise.
        let r = RailReq { rail_h: 0, h: 0, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::Malformed);
    }

    #[test]
    fn the_rail_is_exactly_the_top_strip_and_one_per_display() {
        for w in [1279u32, 1281] {
            assert_eq!(admit_rail(&RailReq { w, ..rail_base() }), StatusAdmit::Malformed, "w={}", w);
        }
        for h in [33u32, 35] {
            assert_eq!(admit_rail(&RailReq { h, ..rail_base() }), StatusAdmit::Malformed, "h={}", h);
        }
        assert_eq!(
            admit_rail(&RailReq { rail_registered: true, ..rail_base() }),
            StatusAdmit::Malformed
        );
        for disp_h in [33u32, 34] {
            assert_eq!(admit_rail(&RailReq { disp_h, ..rail_base() }), StatusAdmit::Malformed, "disp_h={}", disp_h);
        }
        assert_eq!(admit_rail(&RailReq { disp_h: 35, ..rail_base() }), StatusAdmit::Admit);
    }

    #[test]
    fn a_system_renderer_may_not_take_a_declared_sessions_rail() {
        let r = RailReq { session_declared: true, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::NotYours);
        // The declared session takes its own; the principal axis is inert
        // with no session declared.
        let r = RailReq { session_declared: true, requester_is_session: true, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::Admit);
        let r = RailReq { requester_is_session: true, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::Admit);
        // Geometry before ownership, as for the bar: a malformed request
        // from the wrong principal reads Malformed.
        let r = RailReq { session_declared: true, h: 20, ..rail_base() };
        assert_eq!(admit_rail(&r), StatusAdmit::Malformed);
    }

    // ---------------------------------------------------------------------
    // HALCYON-INSTRUMENT 5 (I-2): the Instrument carve, the weights, the
    // minima -- and the legacy carve pinned where it was.
    // ---------------------------------------------------------------------

    fn r(x: u32, y: u32, w: u32, h: u32) -> Rect {
        Rect { x, y, w, h }
    }
    fn inst100() -> theme::Metrics {
        libhalcyon::instrument::INSTRUMENT_BASE.at(100)
    }
    fn weight(l: &Layout, slot: usize) -> u16 {
        l.get(slot).unwrap().weight
    }
    fn parent(l: &Layout, slot: usize) -> usize {
        l.get(slot).unwrap().parent.unwrap()
    }

    /// The reference layout (`fixtures.json`, the 1440 x 900 golden): the
    /// root divides [p1 | [p2 / p3]] at 515:485 and 49:51; p1 a stack of
    /// four with the second open (focused), p2 of three with the first
    /// open, p3 of three with the second open. Built through the same verbs
    /// a session would use, so the weight rules are exercised on the way.
    fn reference() -> (Layout, [usize; 3], [Vec<usize>; 3]) {
        let mut l = Layout::new();
        let p1 = l.root();
        let p2 = l.split(p1, Mode::SplitH).unwrap();
        let p3 = l.split(p2, Mode::SplitV).unwrap();
        let splitv = parent(&l, p2);
        assert!(l.set_weight(p1, 515) && l.set_weight(splitv, 485));
        assert!(l.set_weight(p2, 49) && l.set_weight(p3, 51));
        let t2 = l.split(p1, Mode::Stacked).unwrap();
        let t3 = l.split(t2, Mode::Stacked).unwrap();
        let t4 = l.split(t3, Mode::Stacked).unwrap();
        let u2 = l.split(p2, Mode::Stacked).unwrap();
        let u3 = l.split(u2, Mode::Stacked).unwrap();
        let v2 = l.split(p3, Mode::Stacked).unwrap();
        let v3 = l.split(v2, Mode::Stacked).unwrap();
        assert!(l.focus(p2) && l.focus(v2) && l.focus(t2));
        (l, [p1, p2, p3], [vec![p1, t2, t3, t4], vec![p2, u2, u3], vec![p3, v2, v3]])
    }

    /// Every rect the browser laid out for the reference (the geometry
    /// dump + the PNG, JOURNAL run 46o "I-2"), reproduced: the workspace
    /// root, both dividers, the three frames, every header and both open
    /// bodies -- at 1440 x 900 with the rails carved off (34 + 25).
    #[test]
    fn the_instrument_carve_reproduces_the_reference_layout() {
        let (mut l, [p1, p2, p3], [s1, s2, s3]) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        assert_eq!(l.get(root).unwrap().rect, r(3, 37, 1434, 835), "the root: the workspace padded 3");
        assert_eq!(l.get(root).unwrap().dividers, vec![r(738, 37, 7, 835)], "the root track on columns 738..744");
        // p1: the focused stack of four, the second open.
        let f1 = parent(&l, p1);
        assert_eq!(l.get(f1).unwrap().rect, r(3, 37, 735, 835));
        assert_eq!(l.get(f1).unwrap().dividers, Vec::<Rect>::new());
        let heads: Vec<Rect> = s1.iter().map(|&t| l.get(t).unwrap().tagbar).collect();
        assert_eq!(heads, vec![r(4, 38, 733, 32), r(4, 70, 733, 32), r(4, 807, 733, 32), r(4, 839, 733, 32)]);
        let vis: Vec<bool> = s1.iter().map(|&t| l.get(t).unwrap().visible).collect();
        assert_eq!(vis, vec![false, true, false, false], "one open tile");
        assert_eq!(l.get(s1[1]).unwrap().content, r(4, 102, 733, 704), "the open body: 737 - 32 - 1");
        for &t in [s1[0], s1[2], s1[3]].iter() {
            assert_eq!(l.get(t).unwrap().content, Rect::ZERO, "a collapsed body is ZERO");
            assert_eq!(l.get(t).unwrap().rect, r(3, 37, 735, 835), "a stacked leaf's frame is its stack's");
        }
        // The right column and its track.
        let splitv = parent(&l, parent(&l, p2));
        assert_eq!(l.get(splitv).unwrap().rect, r(745, 37, 692, 835));
        assert_eq!(l.get(splitv).unwrap().dividers, vec![r(745, 443, 692, 7)], "the column's track on rows 443..449");
        // p2: three tiles, the first open.
        let f2 = parent(&l, p2);
        assert_eq!(l.get(f2).unwrap().rect, r(745, 37, 692, 406));
        let heads: Vec<Rect> = s2.iter().map(|&t| l.get(t).unwrap().tagbar).collect();
        assert_eq!(heads, vec![r(746, 38, 690, 32), r(746, 378, 690, 32), r(746, 410, 690, 32)]);
        assert_eq!(l.get(p2).unwrap().content, r(746, 70, 690, 307));
        assert!(l.get(p2).unwrap().visible && !l.get(s2[1]).unwrap().visible);
        // p3: three tiles, the second open.
        let f3 = parent(&l, p3);
        assert_eq!(l.get(f3).unwrap().rect, r(745, 450, 692, 422));
        let heads: Vec<Rect> = s3.iter().map(|&t| l.get(t).unwrap().tagbar).collect();
        assert_eq!(heads, vec![r(746, 451, 690, 32), r(746, 483, 690, 32), r(746, 839, 690, 32)]);
        assert_eq!(l.get(s3[1]).unwrap().content, r(746, 515, 690, 323));
        // Focus: p1's open tile; its stack is the focused pane.
        assert_eq!(l.focused, s1[1]);
        // The dump carries the weights after the rects, nothing else moved.
        let t = l.render_text();
        assert!(t.contains(" w=515\n") && t.contains(" w=485\n") && t.contains(" w=49\n") && t.contains(" w=51\n"), "{t}");
        assert!(t.contains("[4,102,733,704]\n"), "{t}");
        assert!(t.contains("[0,0,0,0] hidden\n"), "{t}");
        // At 200 % in a 2880 x 1800 framebuffer the same tree doubles: the
        // root track at 1476, the frames 2 px, the headers 64.
        l.recompute(r(0, 68, 2880, 1682), 1, libhalcyon::instrument::INSTRUMENT_BASE.at(200), Profile::Instrument);
        assert_eq!(l.get(root).unwrap().rect, r(6, 74, 2868, 1670));
        assert_eq!(l.get(root).unwrap().dividers, vec![r(1476, 74, 14, 1670)]);
        assert_eq!(l.get(s1[1]).unwrap().tagbar, r(8, 140, 1466, 64));
        assert_eq!(l.get(s1[1]).unwrap().content, r(8, 204, 1466, 1408), "1670 - 4 - 3 x 64 - 64 - 2");
    }

    // ---------------------------------------------------------------------
    // HALCYON-INSTRUMENT 9.2 (I-6): the divider tracks as pointer targets,
    // the drag, the double-click, the clamps -- and the fits-check on a
    // mutation judged on a copy.
    // ---------------------------------------------------------------------

    /// A track is hit by the point inside it and names the pair it
    /// separates; a point in a body, a header or the pad hits nothing.
    #[test]
    fn a_track_is_hit_and_names_its_pair() {
        let (mut l, [p1, p2, p3], _) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        let f1 = parent(&l, p1);
        let splitv = parent(&l, parent(&l, p2));
        assert_eq!(l.track_at(738, 400), Some((root, 0)), "the root track's first column");
        assert_eq!(l.track_at(744, 870), Some((root, 0)), "its last column, last row");
        assert_eq!(l.track_at(745, 400), None, "the right column's frame");
        assert_eq!(l.track_at(737, 400), None, "the left stack's frame");
        assert_eq!(l.track_at(1000, 443), Some((splitv, 0)), "the column's track");
        assert_eq!(l.track_at(1000, 450), None, "p3's frame");
        assert_eq!(l.track_at(1, 1), None);
        assert_eq!(l.divide_of(root), vec![f1, splitv]);
        assert_eq!(l.divide_of(splitv), vec![parent(&l, p2), parent(&l, p3)]);
        assert!(l.divide_of(f1).is_empty(), "a stack has no tracks");
        assert!(l.divide_of(p3).is_empty(), "a leaf has no tracks");
    }

    /// The drag (9.2): the root track follows the pointer by the mockup's
    /// ratio -- 100 px right of the track's centre puts the boundary at
    /// 837 (`carve::drag_pair`'s 834 + the origin 3) -- the weights become
    /// the extents (834 : 593), the right column keeps its own division
    /// (49 : 51, its track on the same rows), and a second drag to the same
    /// point changes nothing.
    #[test]
    fn a_drag_moves_the_track_and_the_weights_become_the_extents() {
        let (mut l, [p1, p2, p3], _) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        let f1 = parent(&l, p1);
        let splitv = parent(&l, parent(&l, p2));
        assert_eq!(l.drag_track(root, 0, (841, 400)), DragVerdict::Changed);
        assert_eq!((weight(&l, f1), weight(&l, splitv)), (834, 593));
        assert_eq!((weight(&l, parent(&l, p2)), weight(&l, parent(&l, p3))), (49, 51), "the column's weights untouched");
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        assert_eq!(l.get(root).unwrap().dividers, vec![r(837, 37, 7, 835)], "the track at the pointer less r * t");
        assert_eq!(l.get(f1).unwrap().rect, r(3, 37, 834, 835));
        assert_eq!(l.get(splitv).unwrap().rect, r(844, 37, 593, 835));
        assert_eq!(l.get(splitv).unwrap().dividers, vec![r(844, 443, 593, 7)], "the column's track on its rows");
        assert_eq!(l.drag_track(root, 0, (841, 400)), DragVerdict::Unchanged);
        // A vertical track reads the pointer's y: the column's pair (406,
        // 422 tall, minima 153 each) is held by the band -- 22 % of 828 is
        // 183 -- when the pointer goes to the top.
        assert_eq!(l.drag_track(splitv, 0, (1000, 0)), DragVerdict::Changed);
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        assert_eq!(l.get(splitv).unwrap().dividers, vec![r(844, 220, 593, 7)], "37 + 183");
        assert_eq!(l.get(parent(&l, p2)).unwrap().rect.h, 183);
        assert_eq!(l.get(parent(&l, p3)).unwrap().rect.h, 645);
        assert_eq!(l.get(root).unwrap().dividers, vec![r(837, 37, 7, 835)], "the root untouched by the column's drag");
    }

    /// The clamps (5.2): the band where it is tighter (78 % of the root's
    /// 1427 is 1113, past which the pointer cannot pull), and the overflow
    /// refused -- a 500 px workspace cannot hold two 260 minima, so the
    /// pair is not draggable at all and nothing changes.
    #[test]
    fn a_drag_is_clamped_and_the_overflow_is_refused() {
        let (mut l, [p1, p2, _], _) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        let f1 = parent(&l, p1);
        let splitv = parent(&l, parent(&l, p2));
        assert_eq!(l.drag_track(root, 0, (5000, 400)), DragVerdict::Changed);
        assert_eq!((weight(&l, f1), weight(&l, splitv)), (1113, 314));
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        assert_eq!(l.get(root).unwrap().dividers, vec![r(1116, 37, 7, 835)]);
        assert_eq!(l.get(splitv).unwrap().rect.w, 314, "the column past its minimum still");
        // The overflow: at 500 wide the root's usable 487 is short of 520.
        let (mut l, _, _) = reference();
        l.recompute(r(0, 34, 500, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        let before: Vec<u16> = l.divide_of(root).iter().map(|&c| weight(&l, c)).collect();
        assert_eq!(l.drag_track(root, 0, (300, 400)), DragVerdict::Refused);
        assert_eq!(l.equalise_track(root, 0), DragVerdict::Refused);
        let after: Vec<u16> = l.divide_of(root).iter().map(|&c| weight(&l, c)).collect();
        assert_eq!(before, after, "a refused drag changes no weight");
        // No such track.
        assert_eq!(l.drag_track(root, 5, (300, 400)), DragVerdict::Refused);
        assert_eq!(l.drag_track(l.focused, 0, (300, 400)), DragVerdict::Refused, "a leaf has no track");
    }

    /// Double-click (9.2): the root's pair halves (714 : 713 of 1427), the
    /// track moving from 738 to 717; the column is untouched.
    #[test]
    fn a_double_click_equalises_the_pair() {
        let (mut l, [p1, p2, _], _) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        let f1 = parent(&l, p1);
        let splitv = parent(&l, parent(&l, p2));
        assert_eq!(l.equalise_track(root, 0), DragVerdict::Changed);
        assert_eq!((weight(&l, f1), weight(&l, splitv)), (714, 713));
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        assert_eq!(l.get(root).unwrap().dividers, vec![r(717, 37, 7, 835)]);
        assert_eq!(l.get(splitv).unwrap().dividers, vec![r(724, 443, 713, 7)]);
        assert_eq!(l.equalise_track(root, 0), DragVerdict::Unchanged);
    }

    /// The fits-check on a copy (5.2; r1 A-F1's owed half): turning the
    /// reference root vertical needs 505 rows (185 for the stack of four,
    /// 313 for the column, a track), so in a 400-tall workspace it is
    /// refused and the tree is untouched; in 841 it fits; a tree ALREADY
    /// past its minima stays mutable; a no-op mutation is never refused.
    #[test]
    fn a_mutation_that_would_create_an_overflow_is_judged_on_a_copy() {
        let (mut l, [p1, _, _], _) = reference();
        l.recompute(r(0, 34, 1440, 841), 1, inst100(), Profile::Instrument);
        let root = l.root();
        assert!(l.fits_after(|t| t.set_mode(root, Mode::SplitV)), "505 <= 835");
        l.recompute(r(0, 34, 1440, 400), 1, inst100(), Profile::Instrument);
        let epoch = l.epoch;
        assert!(!l.fits_after(|t| t.set_mode(root, Mode::SplitV)), "505 > 394");
        assert_eq!(l.epoch, epoch, "the copy was mutated, not the tree");
        assert!(matches!(l.get(root).unwrap().kind, Kind::Container { mode: Mode::SplitH, .. }));
        assert!(l.fits_after(|t| t.set_mode(root, Mode::SplitH)), "the same mode: a no-op");
        assert!(l.fits_after(|_| false), "a mutation that fails is not judged");
        assert!(l.fits_after(|t| t.set_mode(p1, Mode::SplitV)), "the left stack as a column: 373 <= 394");
        // Already past the minima (294 < 313): every mutation stays open.
        l.recompute(r(0, 34, 1440, 300), 1, inst100(), Profile::Instrument);
        assert!(l.fits_after(|t| t.set_mode(root, Mode::SplitV)));
        // Legacy has no minima.
        l.recompute(r(0, 0, 1440, 900), 1, theme::builtin().metrics.at(100), Profile::Legacy);
        assert!(l.fits_after(|t| t.set_mode(root, Mode::SplitV)));
    }

    /// The chrome bind's judgement (9.1; r1 A-F6): a session may decorate
    /// its own tile or its own empty leaf, never another principal's tile,
    /// and an empty leaf another principal split is not its to placard.
    #[test]
    fn a_chrome_bind_is_admitted_only_over_its_owners_pane() {
        assert!(chrome_bind_admitted(1001, Some(1001), 0));
        assert!(!chrome_bind_admitted(1001, Some(1002), 1001), "another principal's tile, whatever the leaf's record");
        assert!(chrome_bind_admitted(1001, None, 1001));
        assert!(!chrome_bind_admitted(1001, None, 0), "the environment's empty leaf");
        assert!(!chrome_bind_admitted(1001, None, 1002));
    }

    /// One tile on a display is still a stack of one inside a frame under
    /// two rails (5.6): frame, header, body -- never the legacy borderless
    /// leaf. A zoom is the explicit exception and fills the workspace.
    #[test]
    fn a_lone_tile_is_framed_and_a_zoom_fills_the_workspace() {
        let mut l = Layout::new();
        let a = l.root();
        // A TILE: hosted (an empty lone leaf is the 14.6 placard, tested
        // beside this one).
        assert_eq!(l.host_into(1, a), Some(a));
        l.recompute(r(0, 34, 1280, 661), 1, inst100(), Profile::Instrument);
        let p = l.get(a).unwrap();
        assert!(p.visible);
        assert_eq!(p.rect, r(3, 37, 1274, 655));
        assert_eq!(p.tagbar, r(4, 38, 1272, 32));
        assert_eq!(p.content, r(4, 70, 1272, 621));
        assert!(p.dividers.is_empty());
        assert!(l.zoom_toggle(a));
        l.recompute(r(0, 34, 1280, 661), 1, inst100(), Profile::Instrument);
        let p = l.get(a).unwrap();
        assert_eq!((p.rect, p.content, p.tagbar), (r(0, 34, 1280, 661), r(0, 34, 1280, 661), Rect::ZERO));
        // A workspace too small for anything: nothing published leaves it,
        // nothing panics.
        l.unzoom();
        l.recompute(r(0, 34, 5, 5), 1, inst100(), Profile::Instrument);
        let p = l.get(a).unwrap();
        assert!(p.rect.is_empty() && p.content.is_empty() && p.tagbar.is_empty());
        l.recompute(Rect::ZERO, 1, inst100(), Profile::Instrument);
        assert!(l.get(a).unwrap().rect.is_empty());
    }

    /// The legacy carve is what it was before the profile existed: the
    /// same numbers the compose gate and ls-halcyon measure -- a single
    /// leaf borderless and bar-free, two leaves each with the 4 px ring
    /// (gap 1 + bevel 2 + hairline 1) and the 20 px tag bar, at 1.0 and
    /// 2.0; no dividers, no weights in the dump.
    #[test]
    fn the_legacy_carve_is_pinned() {
        let m = theme::builtin().metrics;
        let mut l = Layout::new();
        let a = l.root();
        l.recompute(r(0, 0, 1280, 780), 1, m.at(100), Profile::Legacy);
        let p = l.get(a).unwrap();
        assert_eq!((p.rect, p.content, p.tagbar), (r(0, 0, 1280, 780), r(0, 0, 1280, 780), Rect::ZERO));
        let b = l.split(a, Mode::SplitH).unwrap();
        l.recompute(r(0, 0, 1280, 780), 1, m.at(100), Profile::Legacy);
        let pa = l.get(a).unwrap();
        let pb = l.get(b).unwrap();
        assert_eq!((pa.rect, pa.tagbar, pa.content), (r(0, 0, 640, 780), r(4, 4, 632, 20), r(4, 24, 632, 752)));
        assert_eq!((pb.rect, pb.tagbar, pb.content), (r(640, 0, 640, 780), r(644, 4, 632, 20), r(644, 24, 632, 752)));
        assert!(l.get(l.root()).unwrap().dividers.is_empty());
        assert!(!l.render_text().contains(" w="), "{}", l.render_text());
        // 2.0: ring 1 + 4 + 2 = 7, the bar 40.
        l.recompute(r(0, 0, 2560, 1560), 1, m.at(200), Profile::Legacy);
        let pa = l.get(a).unwrap();
        assert_eq!((pa.rect, pa.tagbar, pa.content), (r(0, 0, 1280, 1560), r(7, 7, 1266, 40), r(7, 47, 1266, 1506)));
        // A weight set under legacy changes no legacy rect (the division
        // is equal) but travels to the dump for a later Instrument carve.
        assert!(l.set_weight(a, 3));
        l.recompute(r(0, 0, 1280, 780), 1, m.at(100), Profile::Legacy);
        assert_eq!(l.get(a).unwrap().rect, r(0, 0, 640, 780));
        assert!(l.render_text().contains(" w=3\n"));
        assert_eq!(l.min_size(), (0, 0), "no minima under legacy");
        assert!(l.split_fits(a, Mode::SplitH) && l.split_fits(b, Mode::SplitV));
    }

    /// The weight rules (5.2): a newcomer to a container takes the mean of
    /// its siblings; a nesting split's container takes the leaf's weight
    /// and the two inside halve; a dissolved container's survivor takes the
    /// container's; a root has no division; 0 is refused.
    #[test]
    fn weights_follow_the_split_and_dissolve_rules() {
        let mut l = Layout::new();
        let a = l.root();
        let b = l.split(a, Mode::SplitH).unwrap();
        assert_eq!((weight(&l, a), weight(&l, b)), (1, 1));
        assert!(l.set_weight(a, 3) && l.set_weight(b, 5));
        let c = l.split(b, Mode::SplitH).unwrap();
        assert_eq!(weight(&l, c), 4, "the mean of 3 and 5");
        let d = l.split(c, Mode::SplitV).unwrap();
        let cont = parent(&l, c);
        assert_eq!((weight(&l, cont), weight(&l, c), weight(&l, d)), (4, 1, 1));
        let _ = l.close(d);
        assert_eq!(weight(&l, c), 4, "the survivor takes the container's share");
        assert_eq!(parent(&l, c), l.root());
        assert!(!l.set_weight(l.root(), 7), "a root has no division");
        assert!(!l.set_weight(a, 0));
        let e = l.epoch;
        assert!(l.set_weight(a, 3) && l.epoch == e, "the same value moves no epoch");
        assert!(l.set_weight(a, 65535) && l.epoch == e + 1);
        // The mean rounds half up and never reads 0: siblings 1 and 2 -> 2.
        let mut l = Layout::new();
        let a = l.root();
        let b = l.split(a, Mode::SplitV).unwrap();
        assert!(l.set_weight(b, 2));
        let c = l.split(b, Mode::SplitV).unwrap();
        assert_eq!(weight(&l, c), 2);
    }

    /// r1 A-F1: a split past the minima through the tree's OWN api (the
    /// chord path, before its fits-check) lays every child at its minimum
    /// from the origin and the clip takes the overflow to ZERO; such a tile
    /// is DORMANT -- not visible, nothing composes there -- so its owner
    /// cannot lose keys into a tile with no pixels.
    #[test]
    fn a_tile_carved_to_zero_is_dormant() {
        let mut l = Layout::new();
        let area = r(0, 34, 1280, 741); // 1280x800 between the rails: 1274 usable
        l.recompute(area, 1, inst100(), Profile::Instrument);
        let mut f = l.root();
        let mut refused_at = None;
        for i in 0..6 {
            if refused_at.is_none() && !l.split_fits(f, Mode::SplitH) {
                refused_at = Some(i);
            }
            f = l.split(f, Mode::SplitH).unwrap();
            l.recompute(area, 1, inst100(), Profile::Instrument);
        }
        assert_eq!(refused_at, Some(3), "5 x 260 + 4 x 7 = 1328 > 1274: the 4th split is the first refused");
        let newest = l.get(f).unwrap();
        assert!(newest.rect.is_empty(), "the 7th tile is carved to ZERO");
        assert!(!newest.visible && newest.content.is_empty(), "and dormant");
        // Every visible leaf keeps pixels, and at least the four that fit.
        let with_pixels = l
            .live_ids()
            .iter()
            .filter(|&&(slot, _)| l.is_leaf(slot) && l.get(slot).unwrap().visible)
            .inspect(|&&(slot, _)| assert!(!l.get(slot).unwrap().rect.is_empty()))
            .count();
        assert!(with_pixels >= 4 && with_pixels < 7, "{}", with_pixels);
    }

    /// r2 C-F1: dormancy is judged on the carved BODY, not the frame rect.
    /// A lone hosted leaf whose rect is 34 rows tall (2 px of frame + the
    /// 32 px header) has no body and is dormant; one row taller it lives.
    /// An empty leaf's placard is dormant at the frame's own bound.
    #[test]
    fn a_tile_with_a_frame_but_no_body_is_dormant() {
        // The band measured at 1280 wide: workspace heights 7..=40 leave a
        // rect of 1..=34 rows with a ZERO body; 41 is the first with one.
        for (h, dormant) in [(7u32, true), (40, true), (41, false)] {
            let mut l = Layout::new();
            let area = r(0, 34, 1280, h);
            l.recompute(area, 1, inst100(), Profile::Instrument);
            let slot = l.host(1).expect("the root hosts surface 1");
            l.recompute(area, 1, inst100(), Profile::Instrument);
            let p = l.get(slot).unwrap();
            assert!(!p.rect.is_empty(), "h={}: the frame rect is never empty here", h);
            assert_eq!(p.visible, !dormant, "h={}: visible", h);
            assert_eq!(p.content.is_empty(), dormant, "h={}: the body", h);
        }
        // The placard's bound is the frame alone: a rect of 2 rows has no
        // interior; 3 rows has one.
        for (h, dormant) in [(8u32, true), (9, false)] {
            let mut l = Layout::new();
            let area = r(0, 34, 1280, h);
            l.recompute(area, 1, inst100(), Profile::Instrument);
            let p = l.get(l.root()).unwrap();
            assert_eq!(p.visible, !dormant, "placard h={}: visible", h);
        }
    }

    /// The minima (5.2): a split that cannot keep every pane at 260 wide,
    /// every stack at its header budget plus a 54 px body, is refused
    /// before the tree changes; a host that cannot split stacks instead.
    #[test]
    fn a_split_past_the_minima_is_refused_untouched_and_a_host_stacks_instead() {
        let mut l = Layout::new();
        let area = r(0, 34, 600, 300); // root 594 x 235
        l.recompute(area, 1, inst100(), Profile::Instrument);
        let a = l.root();
        assert_eq!(l.min_size(), (260, 88));
        assert!(l.split_fits(a, Mode::SplitH), "2 x 260 + 7 = 527 <= 594");
        let b = l.split(a, Mode::SplitH).unwrap();
        l.recompute(area, 1, inst100(), Profile::Instrument);
        assert_eq!(l.min_size(), (527, 88));
        assert!(!l.split_fits(b, Mode::SplitH), "3 x 260 + 14 = 794 > 594");
        assert!(l.split_fits(b, Mode::SplitV), "b's column: 2 x 88 + 7 = 183 <= 235");
        assert!(l.split_fits(b, Mode::Stacked), "2 + 64 + 1 + 54 = 121 <= 235");
        assert!(l.split_fits(b, Mode::Tabbed));
        // The compositor's own placement: b is focused and empty, so a
        // host lands in it; the next host must split b -- by aspect that is
        // a horizontal split, which does not fit, so the tile joins a stack.
        assert_eq!(l.host(1), Some(b));
        let c = l.host(2).unwrap();
        let stack = parent(&l, c);
        assert!(matches!(l.get(stack).unwrap().kind, Kind::Container { mode: Mode::Stacked, .. }));
        assert_eq!(parent(&l, b), stack);
        l.recompute(area, 1, inst100(), Profile::Instrument);
        assert_eq!(l.min_size(), (527, 121));
        // A stack of two in 235 rows: the open tile is c (the newcomer).
        assert!(l.get(c).unwrap().visible && !l.get(b).unwrap().visible);
        assert_eq!(l.get(b).unwrap().tagbar.h, 32);
        // A workspace that fits nothing: every host past the first is refused.
        let mut l = Layout::new();
        l.recompute(r(0, 34, 300, 150), 1, inst100(), Profile::Instrument);
        let a = l.root();
        assert!(!l.split_fits(a, Mode::SplitH) && !l.split_fits(a, Mode::SplitV));
        assert!(l.split_fits(a, Mode::Stacked), "2 + 64 + 1 + 54 = 121 <= 144");
        assert_eq!(l.host(1), Some(a));
        assert!(l.host(2).is_some(), "the second tile stacks");
        l.recompute(r(0, 34, 300, 150), 1, inst100(), Profile::Instrument);
        let (e, n) = (l.epoch, l.live_ids().len());
        assert_eq!(l.host(3), None, "a third tile would need 2 + 96 + 1 + 54 = 153 > 144: refused");
        assert_eq!((l.epoch, l.live_ids().len()), (e, n), "and the tree is untouched");
    }
    /// HALCYON-INSTRUMENT 14.6 + 6.4 (I-3): a lone EMPTY leaf is the N = 0
    /// pane -- no header row, its `tagbar` the whole interior (the placard's
    /// surface) and its body ZERO; hosted, it is a stack of one again. The
    /// 1 px separator after an open body exists only when a header follows
    /// (the golden's row 806 / 377: `separator`; the last tile's bottom row
    /// is the frame's).
    #[test]
    fn an_empty_lone_leaf_is_the_placard_and_the_separator_follows_an_open_body() {
        let mut l = Layout::new();
        let root = l.root();
        l.recompute(r(0, 34, 1280, 741), 1, inst100(), Profile::Instrument);
        let p = l.get(root).unwrap();
        assert!(p.visible && l.is_empty_leaf(root));
        assert_eq!(p.rect, r(3, 37, 1274, 735), "the frame");
        assert_eq!(p.tagbar, r(4, 38, 1272, 733), "the placard fills the interior");
        assert_eq!(p.content, Rect::ZERO, "no body: no tile exists");
        assert_eq!(p.separator, Rect::ZERO);
        // Hosted: the header returns.
        assert_eq!(l.host(7), Some(root));
        l.recompute(r(0, 34, 1280, 741), 1, inst100(), Profile::Instrument);
        let p = l.get(root).unwrap();
        assert_eq!(p.tagbar, r(4, 38, 1272, 32));
        assert_eq!(p.content, r(4, 70, 1272, 701));
        assert_eq!(p.separator, Rect::ZERO, "a lone open tile is last: no separator");
        // A stack of two with the FIRST open: the separator row sits after
        // the body, before the second header; the collapsed second has none.
        let b = l.split(root, Mode::Stacked).unwrap();
        assert_eq!(l.host(8), Some(b));
        assert!(l.focus(root));
        l.recompute(r(0, 34, 1280, 741), 1, inst100(), Profile::Instrument);
        let (pa, pb) = (l.get(root).unwrap(), l.get(b).unwrap());
        assert_eq!(pa.tagbar, r(4, 38, 1272, 32));
        assert_eq!(pa.content, r(4, 70, 1272, 668), "body: 733 - 32 - 32 - 1");
        assert_eq!(pa.separator, r(4, 738, 1272, 1), "the row after the open body");
        assert_eq!(pb.tagbar, r(4, 739, 1272, 32), "the second header follows the separator");
        assert_eq!(pb.separator, Rect::ZERO);
        assert!(!pb.visible && pb.content.is_empty());
        // The SECOND open: last in the stack, no separator anywhere.
        assert!(l.focus(b));
        l.recompute(r(0, 34, 1280, 741), 1, inst100(), Profile::Instrument);
        let (pa, pb) = (l.get(root).unwrap(), l.get(b).unwrap());
        assert_eq!(pa.separator, Rect::ZERO);
        assert_eq!(pb.separator, Rect::ZERO);
        assert_eq!(pb.tagbar, r(4, 70, 1272, 32));
        assert_eq!(pb.content, r(4, 102, 1272, 669), "body: 733 - 64, no separator row");
        // An EMPTY leaf inside a stack of two keeps a header row (no placard).
        let c = l.split(b, Mode::Stacked).unwrap();
        l.recompute(r(0, 34, 1280, 741), 1, inst100(), Profile::Instrument);
        let pc = l.get(c).unwrap();
        assert!(l.is_empty_leaf(c) && pc.visible);
        assert_eq!(pc.tagbar, r(4, 102, 1272, 32), "a header, not a placard");
        assert_eq!(pc.content, r(4, 134, 1272, 637));
        // The legacy carve is untouched by both rules.
        l.recompute(r(0, 0, 1280, 780), 1, theme::builtin().metrics.at(100), Profile::Legacy);
        for slot in [root, b, c] {
            assert_eq!(l.get(slot).unwrap().separator, Rect::ZERO);
        }
    }

    /// HALCYON-INSTRUMENT 6.5, the successor rule, and the defect the rule
    /// exposed: closing a tile BEFORE the open one must not move the open
    /// tile (the index shifts with it); closing the open one opens the
    /// tile now at its index; closing the open LAST one opens the previous.
    #[test]
    fn closing_a_stacked_tile_keeps_or_hands_on_the_open_one_by_the_successor_rule() {
        let stack = |l: &mut Layout| -> Vec<usize> {
            let a = l.root();
            let b = l.split(a, Mode::Stacked).unwrap();
            let c = l.split(b, Mode::Stacked).unwrap();
            let d = l.split(c, Mode::Stacked).unwrap();
            for (i, s) in [a, b, c, d].iter().enumerate() {
                assert_eq!(l.host_into(10 + i, *s), Some(*s));
            }
            vec![a, b, c, d]
        };
        let active_of = |l: &Layout, s: usize| -> usize {
            match &l.get(parent(l, s)).unwrap().kind {
                Kind::Container { children, active, .. } => children[*active],
                _ => unreachable!(),
            }
        };
        // [A, B, C*, D]: close A -> C stays open (the index followed it).
        let mut l = Layout::new();
        let t = stack(&mut l);
        assert!(l.focus(t[2]));
        l.close(t[0]);
        assert_eq!(active_of(&l, t[2]), t[2], "the open tile survived a close before it");
        assert_eq!(l.focused, t[2]);
        // [A, B*, C, D]: close B (the open one) -> C, the tile now at its index.
        let mut l = Layout::new();
        let t = stack(&mut l);
        assert!(l.focus(t[1]));
        l.close(t[1]);
        assert_eq!(active_of(&l, t[2]), t[2]);
        assert_eq!(l.focused, t[2], "focus follows the successor");
        // [A, B, C, D*]: close D (open, last) -> C, the previous one.
        let mut l = Layout::new();
        let t = stack(&mut l);
        assert!(l.focus(t[3]));
        l.close(t[3]);
        assert_eq!(active_of(&l, t[2]), t[2]);
        assert_eq!(l.focused, t[2]);
        // [A*, B, C, D]: close D (after the open one) -> A stays.
        let mut l = Layout::new();
        let t = stack(&mut l);
        assert!(l.focus(t[0]));
        l.close(t[3]);
        assert_eq!(active_of(&l, t[0]), t[0]);
        assert_eq!(l.focused, t[0]);
    }

    // ---- HALCYON-WORKSPACES 4 (W-1): the live roots ----

    fn ws_disp() -> Rect {
        Rect {
            x: 0,
            y: 0,
            w: 1280,
            h: 800,
        }
    }

    fn ws_lay(l: &mut Layout) {
        l.apply_backgrounded(&[]);
        l.recompute(ws_disp(), 1, theme::builtin().metrics, Profile::Legacy);
    }

    #[test]
    fn a_switch_creates_any_free_number_and_focus_is_per_workspace() {
        let mut l = Layout::new();
        let a_root = l.root();
        assert_eq!(l.workspace_count(), 1);
        assert_eq!(l.active_number(), 1);

        // S4: a SKIPPED number is CREATED, not refused -- the INVERSE of the
        // assertion this replaces. The old "only the next free number" rule
        // kept a dense vector hole-free, a property of the representation,
        // and it was attributed to i3, which creates workspace 5 on Super+5
        // whether or not 2, 3 and 4 exist.
        assert!(l.switch_workspace(3), "a skipped number is CREATED (i3)");
        assert_eq!(l.workspace_count(), 2);
        assert_eq!(l.active_number(), 3);
        assert_eq!(l.workspace_numbers(), alloc::vec![1u8, 3], "the set is SPARSE");
        assert_ne!(l.root(), a_root, "the new workspace has its own root");
        assert!(!l.in_active_root(a_root), "the old root is not in this tree");

        assert!(!l.switch_workspace(MAX_WORKSPACES as u8 + 1), "past the bound");
        assert!(!l.switch_workspace(0), "zero is not a workspace");
        assert!(!l.switch_workspace(3), "already active");

        let b_focus = l.focused;
        assert!(l.switch_workspace(1));
        assert_eq!(l.focused, a_root, "workspace 1's focus came back");
        assert!(l.switch_workspace(3));
        assert_eq!(l.focused, b_focus, "and workspace 3's did too");
    }

    /// `ensure_workspace` INSERTS by number to keep the set sorted, and an
    /// insert at or below `active` shifts the active index -- which must move
    /// with it, or the seat silently changes workspace under the user.
    ///
    /// Creating a LOWER number while a higher one is active is the only way
    /// to reach that line, and every other workspace test creates in
    /// ascending order, so without this one it is unexercised: a sabotage
    /// there would not fire and the coverage would be shape, not bound.
    ///
    /// SABOTAGE: drop `if at <= self.active { self.active += 1; }` and the
    /// active number becomes 2 -- the seat moved on its own.
    #[test]
    fn creating_a_lower_number_keeps_the_seat_where_it_was() {
        let mut l = Layout::new();
        assert!(l.switch_workspace(4), "to 4, skipping 2 and 3");
        let four_root = l.root();
        assert_eq!(l.active_number(), 4);
        let _ = l.host_for(7, 0, 0).expect("a tile to move");

        // Creating 2 sorts it BELOW the active workspace 4, shifting 4's
        // index from 1 to 2.
        assert!(l.move_focused_to_workspace(2), "create 2 below the active 4");
        assert_eq!(
            l.workspace_numbers(),
            alloc::vec![1u8, 2, 4],
            "inserted in order, not appended"
        );
        assert_eq!(l.active_number(), 4, "and the seat stayed on 4");

        // The moved leaf WAS workspace 4's root, and `detach_leaf` no-ops on a
        // parentless pane -- so that branch mints a FRESH empty root to leave
        // behind rather than leaving one slot rooted in two workspaces.
        // Asserting the root was UNCHANGED would contradict that design (it is
        // what this test first claimed, and the failure was the premise, not
        // the code). What must hold: 4 keeps a root of its own, now empty, and
        // the tile is found in 2.
        assert_ne!(l.root(), four_root, "workspace 4 got a fresh root");
        assert!(
            l.leaf_surface(l.root()).is_none(),
            "and it is an empty placeholder"
        );
        assert!(l.switch_workspace(2));
        assert_eq!(
            l.leaf_surface(l.root()),
            Some(7),
            "the tile landed in workspace 2"
        );
    }

    /// S4 (operator-ratified 2026-09-15): a workspace's NUMBER is its
    /// identity, so a vanish must not renumber the survivors. Identity was
    /// the vector index, and dropping an empty MIDDLE workspace shifted every
    /// higher one down -- the user's tiles stayed alive but Super+4 stopped
    /// reaching them and made a fresh empty workspace instead.
    ///
    /// SABOTAGE: have `ensure_workspace` push instead of inserting by number
    /// (or drop `Workspace.number` and index again) and the survivor comes
    /// back as 2 rather than 4.
    #[test]
    fn a_vanish_does_not_renumber_the_survivors() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "workspace 2, left empty");
        assert!(l.switch_workspace(4), "workspace 4");
        let four = l.host_for(8, 0, 0).expect("workspace 4's tile");
        assert_eq!(l.workspace_numbers(), alloc::vec![1u8, 2, 4]);

        assert!(l.switch_workspace(1), "back to 1, so 2 is inactive AND empty");
        assert_eq!(l.reap_empty_workspaces(), 1, "the empty middle one goes");
        assert_eq!(
            l.workspace_numbers(),
            alloc::vec![1u8, 4],
            "and 4 is STILL 4 -- a vanish never renumbers the survivors"
        );
        assert_eq!(l.active_number(), 1, "the seat did not move");
        assert!(l.switch_workspace(4), "Super+4 still reaches the same work");
        assert_eq!(l.leaf_surface(four), Some(8), "with its tile intact");
    }

    #[test]
    fn an_inactive_workspace_is_dormant_and_carves_to_nothing() {
        let mut l = Layout::new();
        let a_root = l.root();
        let _ = l.host_for(7, 0, 0);
        assert!(l.switch_workspace(2));
        let _ = l.host_for(8, 0, 0);
        ws_lay(&mut l);
        let a = l.get(a_root).expect("the other root outlives the switch");
        assert!(a.backgrounded, "an inactive root is stamped dormant");
        assert!(!a.visible, "and the carve leaves it invisible");
        assert!(a.rect.is_empty(), "with no pixels");
        let b = l.get(l.root()).unwrap();
        assert!(!b.backgrounded, "the active root is not dormant");
        assert!(b.visible);
    }

    #[test]
    fn closing_the_active_root_leaves_every_other_workspace_alive() {
        // The defect this exists for: `close_inner`'s root arm used to free
        // the WHOLE POOL -- "the subtree was the whole tree" was true with
        // one root and annihilates every other workspace with nine.
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0);
        assert!(l.switch_workspace(2));
        let _ = l.host_for(8, 0, 0);
        let b_root = l.root();
        assert!(l.switch_workspace(1));
        let a_root = l.root();
        l.close(a_root);
        assert!(l.get(b_root).is_some(), "the other root SURVIVES the close");
        assert_eq!(l.workspace_count(), 2);
        assert!(l.switch_workspace(2));
        assert_eq!(l.root(), b_root);
        assert_eq!(l.leaf_surface(b_root), Some(8), "and still hosts its tile");
    }

    #[test]
    fn an_empty_inactive_workspace_vanishes_and_the_active_one_never_does() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0);
        assert!(l.switch_workspace(2)); // empty AND active
        assert_eq!(l.reap_empty_workspaces(), 0, "the active one never goes");
        assert_eq!(l.workspace_count(), 2);
        assert!(l.switch_workspace(1)); // now the empty one is inactive
        assert_eq!(l.reap_empty_workspaces(), 1, "i3: the empty one goes");
        assert_eq!(l.workspace_count(), 1);
        assert_eq!(l.active_number(), 1);
    }

    /// F1 (the W-2b architecture review, verified): `dissolve_if_single`
    /// re-seated `workspaces[active].root` UNCONDITIONALLY, so dissolving an
    /// INACTIVE workspace's root moved the ACTIVE workspace onto a pane in
    /// another tree and then freed the slot the inactive one still named --
    /// both corrupted, from one `close` in a workspace nobody was looking at.
    /// Reachable because `slot_of_id` is global.
    ///
    /// SABOTAGE: restore the active-based `set_root(only)` and the first
    /// assertion fails. No existing workspace test could catch this -- every
    /// one of them closes the ACTIVE root.
    #[test]
    fn a_dissolve_in_an_inactive_workspace_leaves_the_active_root_alone() {
        let mut l = Layout::new();
        let one = l.host_for(7, 0, 0).expect("workspace 1, first tile");
        let two = l.host_for(8, 0, 0).expect("workspace 1, second tile");
        assert_ne!(one, two, "the premise: workspace 1's root is a container");
        assert!(l.switch_workspace(2), "to workspace 2");
        let ws2_root = l.root();
        // Close a tile INSIDE the dormant workspace. Its root then has one
        // child left and dissolves -- the moment the old code re-seated the
        // wrong workspace.
        let _ = l.close(one);
        assert_eq!(
            l.root(),
            ws2_root,
            "the ACTIVE workspace's root must not move when another workspace dissolves"
        );
        assert!(l.in_active_root(ws2_root));
        assert!(l.switch_workspace(1), "back to workspace 1");
        assert!(
            l.get(l.root()).is_some(),
            "workspace 1's root is a live slot, not the freed container"
        );
        assert_eq!(
            l.leaf_surface(l.root()),
            Some(8),
            "and it is the tile that survived the close"
        );
    }

    /// F2 (the same review, verified): the cross-workspace zoom guard was
    /// added to `recompute_instrument` ONLY, while `recompute_legacy` -- the
    /// profile that SHIPS -- had none, and the W-1a commit body and audit row
    /// both claimed "the zoom" was guarded.
    ///
    /// NOTE THE ORDER: a switch CLEARS `zoomed_id` (the test above pins
    /// that), so this state is reachable only by zooming a foreign slot
    /// AFTER the switch. Zoom-then-switch would pass with the guard deleted,
    /// which is the check that cannot fail.
    ///
    /// SABOTAGE: drop `&& self.in_active_root(z)` from `recompute_legacy` and
    /// the dormant pane is painted at the full display rect over the active
    /// tree.
    /// ROUND 1 F2, the SETTER half. The test below used to construct its
    /// state with `zoom_toggle(dormant)` and assert only on the carve -- so
    /// it passed while the reaching defect was live, which is exactly how the
    /// round found it. The setter now refuses, and this pins that.
    ///
    /// SABOTAGE: drop `|| !self.in_active_root(slot)` from `zoom_toggle` and
    /// the first assertion fails.
    #[test]
    fn a_zoom_targeting_another_workspace_is_refused_at_the_setter() {
        let mut l = Layout::new();
        let dormant = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        assert!(
            !l.zoom_toggle(dormant),
            "a pane in another workspace is not zoomable"
        );
        assert!(l.zoom_id().is_none(), "and nothing was recorded");
    }

    /// F2 (the W-2b architecture review, verified): the cross-workspace zoom
    /// guard was added to `recompute_instrument` ONLY, while
    /// `recompute_legacy` -- the profile that SHIPS -- had none, and the W-1a
    /// commit body and audit row both claimed "the zoom" was guarded.
    ///
    /// The public path can no longer reach this state (the setter refuses,
    /// and a switch clears `zoomed_id`), so the field is set DIRECTLY here.
    /// That is deliberate: the carve guard is defence in depth for a state
    /// the tree should never hold, and a guard worth keeping is worth
    /// testing even once its reachability is closed.
    ///
    /// SABOTAGE: drop `&& self.in_active_root(z)` from `recompute_legacy` and
    /// the dormant pane is painted at the full display rect over the active
    /// tree.
    #[test]
    fn the_legacy_carve_refuses_a_zoom_that_lives_in_another_workspace() {
        let mut l = Layout::new();
        let dormant = l.host_for(7, 0, 0).expect("workspace 1's tile");
        let dormant_id = l.id_of(dormant).expect("its id");
        assert!(l.switch_workspace(2), "to workspace 2");
        l.zoomed_id = Some(dormant_id); // unreachable via the verbs; see above
        ws_lay(&mut l); // Profile::Legacy -- the shipped one
        let z = l.get(dormant).expect("the dormant leaf outlives the switch");
        assert!(!z.visible, "a dormant workspace's zoom must not be painted");
        assert!(z.rect.is_empty(), "and must claim no pixels");
        assert!(
            l.get(l.root()).unwrap().visible,
            "while the active workspace carves normally"
        );
    }

    /// ROUND 1 F1 [P0]. `close_inner`'s root arm tested `slot == self.root()`
    /// -- the ACTIVE root -- so an INACTIVE workspace's root fell through to
    /// the parentless early return, freeing nothing and unhosting nothing,
    /// while `collect_surfaces` had ALREADY handed the caller every surface
    /// in it. The leaf went on naming a surface slot the caller then freed,
    /// and surface slots are reused first-free.
    ///
    /// SABOTAGE: restore `if slot == self.root()` and the leaf keeps hosting
    /// surface 8 after the close.
    #[test]
    fn closing_an_inactive_workspace_root_really_collapses_it() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let ws2_root = l.root();
        let _ = l.host_for(8, 0, 0).expect("workspace 2's tile");
        assert_eq!(l.leaf_surface(ws2_root), Some(8));
        assert!(l.switch_workspace(1), "back to workspace 1");

        let unhosted = l.close(ws2_root);
        assert_eq!(unhosted, alloc::vec![8], "the surface is reported unhosted");
        assert!(l.get(ws2_root).is_some(), "the root itself never leaves");
        assert_eq!(
            l.leaf_surface(ws2_root),
            None,
            "and it must NOT still name the surface it just released"
        );
        // The I-32 half: a workspace stuck hosting a dead index could never
        // be reaped, leaking the workspace and its pane slots for the session.
        assert_eq!(
            l.reap_empty_workspaces(),
            1,
            "an emptied inactive workspace can now vanish"
        );
    }

    /// ROUND 1 F2, the FOCUS half. `slot_of_id` is global by design, so every
    /// focus-moving verb could name a pane in a dormant workspace; keys then
    /// routed to an invisible tile.
    ///
    /// SABOTAGE: drop the `in_active_root` guard at the head of `focus` and
    /// the focus moves into workspace 1.
    #[test]
    fn focus_refuses_a_pane_in_another_workspace() {
        let mut l = Layout::new();
        let dormant = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let here = l.focused;
        assert!(!l.focus(dormant), "a dormant pane is not focusable");
        assert_eq!(l.focused, here, "and focus did not move");
        assert!(l.in_active_root(l.focused));
    }

    /// ROUND 1 F2, the SPLIT half -- and the one with teeth: `host_for`
    /// places the next surface at `self.focused`, so a split that dragged
    /// focus into a dormant workspace hosted the NEXT CLIENT there, where it
    /// was never seen.
    ///
    /// SABOTAGE: restore the bare `self.focused = new_leaf;` in `split` and
    /// the new surface lands in workspace 1.
    #[test]
    fn a_split_in_a_dormant_workspace_does_not_capture_focus() {
        let mut l = Layout::new();
        let dormant = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let here = l.focused;
        // The NEST branch: `dormant` is workspace 1's parentless root.
        let made = l.split(dormant, Mode::SplitH).expect("the tree still splits");
        assert!(!l.in_active_root(made), "the new leaf is in workspace 1");
        assert_eq!(l.focused, here, "but focus stayed in workspace 2");

        // The FLATTEN branch, which is a DIFFERENT assignment in the same
        // function: `made` now has a same-mode parent, so this one inserts a
        // sibling instead of nesting. Guarding only the nest branch would
        // leave this path live, and no test above would have said so.
        let flat = l.split(made, Mode::SplitH).expect("a same-mode sibling");
        assert!(!l.in_active_root(flat), "still workspace 1");
        assert_eq!(l.focused, here, "and focus STILL stayed in workspace 2");

        let landed = l.host_for(9, 0, 0).expect("the next client");
        assert!(
            l.in_active_root(landed),
            "so the next surface is hosted where the user is looking"
        );
    }

    /// ROUND 1 F4. `move_focused_to_workspace` ran `detach_leaf` BEFORE its
    /// last allocation, so on an exhausted pane table the container alloc
    /// failed AFTER the detach and the function returned false with the leaf
    /// PARENTLESS: in no tree, still hosting its surface, invisible,
    /// un-reapable, and still addressable by id through the global `pane/`
    /// readdir. `move_dir` already had this ordering right ("pane table
    /// full: untouched"); this path had inverted it.
    ///
    /// The parent must keep >= 2 children after the detach, or the dissolve
    /// frees a slot, the old code's alloc SUCCEEDS, and this passes for the
    /// wrong reason. Same-mode splits FLATTEN, so repeated SplitH builds one
    /// wide container rather than a binary chain.
    ///
    /// SABOTAGE: restore the detach-then-alloc order and the leaf is orphaned.
    #[test]
    fn a_move_refused_by_a_full_pane_table_leaves_the_leaf_attached() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let _ = l
            .host_for(8, 0, 0)
            .expect("workspace 2's tile -- so its root is NOT a placeholder");
        assert!(l.switch_workspace(1), "back to workspace 1");

        let mut last = l.focused;
        while let Some(made) = l.split(l.focused, Mode::SplitH) {
            last = made;
        }
        assert_eq!(l.focused, last, "focus followed the last split");
        // The move refuses an EMPTY tile outright -- `!is_leaf ||
        // leaf_surface().is_none()` is its first line -- so without a surface
        // here the refusal would come from THAT guard and the test would pass
        // having never reached the allocation it exists to constrain. The
        // sabotage caught exactly this: asserting the shape is not exercising
        // the bound.
        let _ = l.host_for(9, 0, 0).expect("host into the focused empty leaf");
        assert_eq!(l.leaf_surface(last), Some(9), "the tile is occupied");
        let parent = l
            .get(last)
            .and_then(|p| p.parent)
            .expect("the filled tree gave it a parent");
        let kids = match l.get(parent).map(|p| &p.kind) {
            Some(Kind::Container { children, .. }) => children.len(),
            _ => 0,
        };
        assert!(
            kids >= 3,
            "the premise: a detach here must dissolve nothing (children={})",
            kids
        );

        assert!(
            !l.move_focused_to_workspace(2),
            "an exhausted pane table refuses the move"
        );
        assert_eq!(
            l.get(last).and_then(|p| p.parent),
            Some(parent),
            "and a refused move must leave the leaf IN the tree"
        );
        assert!(l.in_active_root(last), "exactly where it was");
    }

    /// ROUND 1 F3. `Workspace.focused` stored a SLOT, and `alloc` hands out
    /// the first FREE slot -- so a pane created in another workspace could
    /// land on the remembered slot and sail through the `is_leaf` restore
    /// guard as a genuinely live leaf. Ids are never reused, so storing the
    /// id makes a dead remembered focus resolve to nothing, which is exactly
    /// the fallback wanted.
    ///
    /// SABOTAGE: store `self.focused` (the slot) in `switch_workspace` and
    /// restore with `is_leaf(want)` -- focus comes back on a workspace-1 leaf.
    #[test]
    fn a_reused_slot_cannot_resurrect_a_remembered_focus() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let ws2_root = l.root();
        let t2 = l.split(ws2_root, Mode::SplitH).expect("a second tile here");
        assert!(l.focus(t2));
        let remembered = l.focused;
        assert!(l.switch_workspace(1), "to workspace 1 -- t2's slot is saved");

        // t2 dies while we are away, and workspace 1 then allocates enough
        // panes to reuse its slot.
        let _ = l.close(t2);
        assert!(l.get(remembered).is_none(), "the slot really is free");
        let mut reused = false;
        for _ in 0..4 {
            if let Some(made) = l.split(l.focused, Mode::SplitH) {
                if made == remembered {
                    reused = true;
                }
            }
        }
        assert!(reused, "the premise: workspace 1 took the freed slot");

        assert!(l.switch_workspace(2), "back to workspace 2");
        assert!(
            l.in_active_root(l.focused),
            "focus must not be resurrected onto another workspace's pane"
        );
    }

    /// ROUND 1 S5 (my own self-audit, not the agent's). The vanish rule
    /// tested only for HOSTED surfaces, so a workspace holding nothing but a
    /// RESERVED empty leaf -- the skeleton a layout-restore tool builds,
    /// stamped with its `creator_conn` by H-4d precisely so the session's own
    /// compositor cannot fill it mid-build -- read as empty and was destroyed
    /// at the next reconcile. The vanish rule was deleting the very thing
    /// that reservation exists to protect.
    ///
    /// SABOTAGE: drop `&& !self.subtree_reserved(r)` and the first assertion
    /// fails -- the half-built workspace vanishes under its builder.
    #[test]
    fn a_workspace_holding_a_reserved_skeleton_does_not_vanish() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let skeleton = l.root();
        l.set_creator(skeleton, 42, 7); // a restore tool is building here
        assert!(l.switch_workspace(1), "back to workspace 1");

        assert_eq!(
            l.reap_empty_workspaces(),
            0,
            "a reserved skeleton is not an empty workspace"
        );
        assert_eq!(l.workspace_count(), 2);

        // The builder goes: the reservation lifts and the ordinary rule
        // applies again.
        l.release_creator(42);
        assert_eq!(
            l.reap_empty_workspaces(),
            1,
            "and once nothing is reserved, the empty workspace vanishes"
        );
    }

    /// S4: the header carries the ascending LIST of live numbers and the
    /// ACTIVE number -- identities, not positions.
    ///
    /// The sparse case is the load-bearing one: with workspaces 1 and 3 a
    /// COUNT would render "2", so asserting "1,3" is the only form a count
    /// cannot satisfy. Asserting the dense case alone would pass under either
    /// design, which is a check that cannot fail.
    #[test]
    fn the_layout_header_carries_the_list_and_the_active_number() {
        let mut l = Layout::new();
        let first = l.render_text();
        let head = first.lines().next().unwrap();
        assert!(head.contains("workspaces 1 active 1"), "{}", head);

        assert!(l.switch_workspace(3), "skip 2 -- the set goes sparse");
        let second = l.render_text();
        let head = second.lines().next().unwrap();
        assert!(head.contains("workspaces 1,3 active 3"), "{}", head);
    }

    #[test]
    fn a_zoom_does_not_survive_a_workspace_switch() {
        let mut l = Layout::new();
        let a_root = l.root();
        let _ = l.host_for(7, 0, 0);
        assert!(l.zoom_toggle(a_root));
        assert!(l.zoom_id().is_some());
        assert!(l.switch_workspace(2));
        assert!(l.zoom_id().is_none(), "the zoom belonged to the tree it was made in");
        ws_lay(&mut l);
        assert!(!l.get(a_root).unwrap().visible, "the other tree stays dark");
        assert!(l.get(l.root()).unwrap().visible, "this one carves normally");
    }

    #[test]
    fn a_moved_tile_keeps_its_surface_and_leaves_no_aliased_root() {
        let mut l = Layout::new();
        let a_root = l.root();
        let _ = l.host_for(7, 0, 0);
        assert!(l.switch_workspace(2));
        assert!(l.switch_workspace(1));
        // The focused leaf IS this workspace's root: `detach_leaf` no-ops on
        // a parentless pane, so without the re-seat this slot would end up
        // rooted in BOTH workspaces.
        assert_eq!(l.focused, a_root);
        assert!(l.move_focused_to_workspace(2));
        assert_ne!(l.root(), a_root, "the workspace it left was re-seated");
        assert!(l.switch_workspace(2));
        assert_eq!(l.leaf_surface(a_root), Some(7), "the tile kept its surface");
        assert!(l.in_active_root(a_root), "and lives in the target tree now");
    }

    /// r2 F2 (P0): S5 taught the vanish rule to respect a placement
    /// RESERVATION, but `creator_conn` was never cleared when the leaf it
    /// reserved was FILLED or when the root it sat on COLLAPSED. The rail's
    /// SPLIT H stamps halcyond's own session conn -- alive for the whole
    /// session -- so every workspace a session had split in stopped
    /// vanishing, silently, for the rest of that session.
    ///
    /// The battery cannot see this: it never issues a `split` VERB, so its
    /// roots keep `creator_conn == 0` and vanish normally.
    ///
    /// SABOTAGE: drop the `creator_conn` clear in `host_into` and the reap
    /// returns 0 -- the emptied workspace is pinned for good.
    #[test]
    fn a_workspace_a_session_split_and_emptied_still_vanishes() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let a = l.host_for(8, 0, 0).expect("tile A");

        // The rail's SPLIT H as `pane_cmd` performs it: the new empty leaf is
        // RESERVED to the conn that split it (H-4d).
        let b = l.split(a, Mode::SplitH).expect("the split");
        l.set_creator(b, 99, 5);
        let _ = l.host_into(9, b).expect("tile B fills the reserved leaf");

        let _ = l.close(a);
        assert_eq!(l.root(), b, "the dissolve promoted B to be the root");
        let _ = l.close(b);
        assert!(l.is_empty_leaf(b), "the root collapsed back to an empty leaf");

        assert!(l.switch_workspace(1), "back to workspace 1");
        assert_eq!(
            l.reap_empty_workspaces(),
            1,
            "a workspace whose tiles are gone vanishes -- split in or not"
        );
    }

    /// r2 F1: round 1 closed the cross-workspace class at the FOCUS
    /// chokepoint. `move_dir` is a STRUCTURAL verb and read the ACTIVE root
    /// unconditionally, so its root-wrap branch grafted a pane out of a
    /// DORMANT workspace onto the active root -- re-parenting a root the user
    /// never asked about, and taking a tile from the workspace holding it.
    /// `slot_of_id` is global by design, so the id is reachable by verb.
    ///
    /// The tile needs a PARENT for the walk to reach the wrap branch: a
    /// parentless one returns false at `sub == slot` and the test would pass
    /// having never reached the line it exists to constrain.
    ///
    /// SABOTAGE: drop the `in_active_root` guard and this returns true with
    /// the active root re-parented under a fresh container.
    #[test]
    fn a_directional_move_refuses_a_pane_in_another_workspace() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let t1 = l.host_for(8, 0, 0).expect("workspace 2's first tile");
        let t2 = l.split(t1, Mode::SplitH).expect("a second tile beside it");
        let _ = l.host_into(9, t2).expect("fill it");
        assert!(l.get(t2).and_then(|p| p.parent).is_some(), "it has a parent");
        assert!(l.switch_workspace(1), "back to workspace 1");
        let a_root = l.root();

        assert!(
            !l.move_dir(t2, Dir::Up),
            "a pane in another workspace is not movable from this seat"
        );
        assert_eq!(l.root(), a_root, "the active root was not re-parented");
        assert!(!l.in_active_root(t2), "and the tile stayed where it was");
    }

    /// r2 F3: `reap_empty_workspaces` was taught to respect a reservation
    /// (S5); this path was not. The move judges "placeholder" on EMPTINESS
    /// alone, so an arriving tile freed a restore tool's reserved skeleton
    /// root and the tool's later `create claim=` found nothing.
    ///
    /// SABOTAGE: drop the `subtree_reserved` conjunct and the skeleton slot
    /// is freed by `free_subtree`.
    #[test]
    fn a_move_does_not_destroy_a_reserved_skeleton_root() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("the tile that will move");
        assert!(l.switch_workspace(3), "to workspace 3");
        let skeleton = l.root();
        l.set_creator(skeleton, 42, 7); // a restore tool is building here
        assert!(l.switch_workspace(1), "back to workspace 1");

        assert!(l.move_focused_to_workspace(3), "the move lands");
        assert!(
            l.get(skeleton).is_some(),
            "the reserved skeleton survived the arriving tile"
        );
    }

    /// r2 F4: a move refused by an exhausted pane table must leave NOTHING
    /// behind -- and the commit body claimed exactly that, having closed only
    /// one of the two doors. The empty-tile refusal is judged before the
    /// ensure; the ALLOCATION refusals were not.
    ///
    /// The premise needs care: the strand only occurs when the focused leaf is
    /// its own workspace's ROOT (so a replacement must be allocated) AND the
    /// target is absent (so the ensure mints one). Filling the pool from
    /// ANOTHER workspace keeps workspace 1 a single parentless hosted leaf,
    /// and freeing exactly one slot leaves room for the ensure but not for
    /// the leaf after it -- which is the window.
    ///
    /// SABOTAGE: move the `pre_fresh` alloc back below the ensure and the
    /// count goes to 2 -- workspace 5 exists, empty and unasked-for.
    #[test]
    fn a_move_refused_by_a_full_pane_table_mints_no_workspace() {
        let mut l = Layout::new();
        let home = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "fill the pool from somewhere else");
        while l.split(l.focused, Mode::SplitH).is_some() {}

        // Free EXACTLY one slot: a leaf whose parent keeps >= 2 children
        // dissolves nothing, so one close frees one pane.
        let parent = l
            .get(l.focused)
            .and_then(|p| p.parent)
            .expect("the fill nested it");
        let kids = match l.get(parent).map(|p| &p.kind) {
            Some(Kind::Container { children, .. }) => children.clone(),
            _ => Vec::new(),
        };
        assert!(kids.len() >= 3, "the premise: closing one dissolves nothing");
        assert!(l.is_leaf(kids[0]), "and the one closed is a leaf");
        let _ = l.close(kids[0]);

        assert!(l.switch_workspace(1), "back to workspace 1");
        assert_eq!(l.focused, home, "its focused leaf IS its root");
        assert!(l.get(home).and_then(|p| p.parent).is_none(), "parentless");
        let before = l.workspace_count();

        assert!(!l.move_focused_to_workspace(5), "one free slot is not two");
        assert_eq!(
            l.workspace_count(),
            before,
            "and the refusal minted no workspace"
        );
    }

    /// r2 F2, the HOST half in isolation. The end-to-end vanish test covers
    /// this clear and `close_inner`'s TOGETHER -- measured: reverting either
    /// one alone leaves that test green, because along its path the other
    /// still lifts the reservation. A property with no per-site witness is
    /// shape, not bound, so each site gets its own.
    ///
    /// SABOTAGE: drop the clear in `host_into` and the second assert fails.
    #[test]
    fn filling_a_reserved_leaf_spends_its_reservation() {
        let mut l = Layout::new();
        let a = l.host_for(7, 0, 0).expect("a tile");
        let b = l.split(a, Mode::SplitH).expect("the split's new leaf");
        l.set_creator(b, 99, 5);
        assert!(l.subtree_reserved(b), "the premise: the split reserved it");

        let _ = l.host_into(9, b).expect("a program takes the tile");
        assert!(
            !l.subtree_reserved(b),
            "a FILLED leaf is no longer spoken for -- the reservation existed \
             to keep it empty until its builder arrived"
        );
    }

    /// r2 F2, the COLLAPSE half in isolation. The stamp sits on the root
    /// CONTAINER, which `host_into` never touches, so only the root-collapse
    /// arm's clear can lift it.
    ///
    /// SABOTAGE: drop the clear in `close_inner`'s root arm and the reap
    /// returns 0.
    #[test]
    fn a_collapsed_root_drops_its_reservation() {
        let mut l = Layout::new();
        let _ = l.host_for(7, 0, 0).expect("workspace 1's tile");
        assert!(l.switch_workspace(2), "to workspace 2");
        let a = l.host_for(8, 0, 0).expect("tile A");
        let b = l.split(a, Mode::SplitH).expect("tile B's leaf");
        let _ = l.host_into(9, b).expect("fill it");
        let root = l.root();
        assert!(!l.is_leaf(root), "the premise: the root is a CONTAINER");
        l.set_creator(root, 99, 5);
        assert!(l.subtree_reserved(root), "and it is reserved");

        let _ = l.close(root);
        assert!(l.is_empty_leaf(root), "the root collapsed to an empty leaf");
        assert!(
            !l.subtree_reserved(root),
            "a COLLAPSED root is a pristine root -- its reservation went with \
             the tiles it was stamped beside"
        );

        assert!(l.switch_workspace(1), "back to workspace 1");
        assert_eq!(l.reap_empty_workspaces(), 1, "so the workspace vanishes");
    }
}
