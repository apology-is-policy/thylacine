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
}

impl Role {
    pub fn name(self) -> &'static str {
        match self {
            Role::Content => "content",
            Role::Chrome => "chrome",
            Role::PinTarget => "pin-target",
            Role::Menu => "menu",
            Role::Status => "status",
        }
    }
    pub fn parse(s: &str) -> Option<Role> {
        match s {
            "content" => Some(Role::Content),
            "chrome" => Some(Role::Chrome),
            "pin-target" => Some(Role::PinTarget),
            "menu" => Some(Role::Menu),
            "status" => Some(Role::Status),
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

pub struct Layout {
    panes: Vec<Option<Pane>>,
    pub root: usize,
    /// The focused LEAF slot.
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
            root: 0,
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
        l.root = root;
        l.focused = root;
        l
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
                self.focused = new_leaf;
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
            None => self.root = container,
        }
        self.get_mut(slot).unwrap().parent = Some(container);
        self.get_mut(slot).unwrap().weight = DEFAULT_WEIGHT;
        self.get_mut(container).unwrap().weight = leaf_weight;
        if let Some(Kind::Container { children, .. }) = self.get_mut(container).map(|p| &mut p.kind)
        {
            children.push(slot);
            children.push(new_leaf);
        }
        self.focused = new_leaf;
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
        if slot == self.root {
            // The root never leaves; it collapses back to an empty leaf.
            if let Some(p) = self.get_mut(slot) {
                p.kind = Kind::Leaf { surface: None };
                p.status = Status::Resting;
                p.claim_token = None;
                p.weight = DEFAULT_WEIGHT;
                p.dividers.clear();
                p.separator = Rect::ZERO;
            }
            // Free every other pane (the subtree was the whole tree).
            for i in 0..self.panes.len() {
                if i != slot {
                    self.panes[i] = None;
                }
            }
            self.focused = slot;
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
            let f = self.first_leaf(parent).unwrap_or(self.root);
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
                self.root = only;
                self.get_mut(only).unwrap().parent = None;
            }
        }
        self.panes[slot] = None;
        if self.focused == slot {
            self.focused = self.first_leaf(only).unwrap_or(self.root);
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
        if !self.is_leaf(slot) {
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
                    let oldroot = self.root; // re-read: detach may dissolve
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
                    self.root = c;
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
                Some(z) if self.is_leaf(z) => {
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
        let root = self.root;
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
                Some(z) if self.is_leaf(z) => {
                    let p = self.get_mut(z).unwrap();
                    p.visible = true;
                    p.rect = area;
                    p.content = area;
                    return;
                }
                _ => self.zoomed_id = None,
            }
        }
        let root = self.root;
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
            p.visible = true;
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
                p.visible = is_open;
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
        self.min_size_hyp(self.root, None)
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
        let (w, h) = self.min_size_hyp(self.root, Some((slot, mode)));
        w <= root.w && h <= root.h
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
        let _ = core::fmt::write(
            &mut s,
            format_args!(
                "epoch {} focused {}",
                self.epoch,
                self.id_of(self.focused).unwrap_or(0)
            ),
        );
        if let Some(z) = self.zoomed_id {
            let _ = core::fmt::write(&mut s, format_args!(" zoomed {}", z));
        }
        s.push('\n');
        self.render_pane(&mut s, self.root, 0);
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
        let p1 = l.root;
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
        let root = l.root;
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

    /// One tile on a display is still a stack of one inside a frame under
    /// two rails (5.6): frame, header, body -- never the legacy borderless
    /// leaf. A zoom is the explicit exception and fills the workspace.
    #[test]
    fn a_lone_tile_is_framed_and_a_zoom_fills_the_workspace() {
        let mut l = Layout::new();
        let a = l.root;
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
        let a = l.root;
        l.recompute(r(0, 0, 1280, 780), 1, m.at(100), Profile::Legacy);
        let p = l.get(a).unwrap();
        assert_eq!((p.rect, p.content, p.tagbar), (r(0, 0, 1280, 780), r(0, 0, 1280, 780), Rect::ZERO));
        let b = l.split(a, Mode::SplitH).unwrap();
        l.recompute(r(0, 0, 1280, 780), 1, m.at(100), Profile::Legacy);
        let pa = l.get(a).unwrap();
        let pb = l.get(b).unwrap();
        assert_eq!((pa.rect, pa.tagbar, pa.content), (r(0, 0, 640, 780), r(4, 4, 632, 20), r(4, 24, 632, 752)));
        assert_eq!((pb.rect, pb.tagbar, pb.content), (r(640, 0, 640, 780), r(644, 4, 632, 20), r(644, 24, 632, 752)));
        assert!(l.get(l.root).unwrap().dividers.is_empty());
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
        let a = l.root;
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
        assert_eq!(parent(&l, c), l.root);
        assert!(!l.set_weight(l.root, 7), "a root has no division");
        assert!(!l.set_weight(a, 0));
        let e = l.epoch;
        assert!(l.set_weight(a, 3) && l.epoch == e, "the same value moves no epoch");
        assert!(l.set_weight(a, 65535) && l.epoch == e + 1);
        // The mean rounds half up and never reads 0: siblings 1 and 2 -> 2.
        let mut l = Layout::new();
        let a = l.root;
        let b = l.split(a, Mode::SplitV).unwrap();
        assert!(l.set_weight(b, 2));
        let c = l.split(b, Mode::SplitV).unwrap();
        assert_eq!(weight(&l, c), 2);
    }

    /// The minima (5.2): a split that cannot keep every pane at 260 wide,
    /// every stack at its header budget plus a 54 px body, is refused
    /// before the tree changes; a host that cannot split stacks instead.
    #[test]
    fn a_split_past_the_minima_is_refused_untouched_and_a_host_stacks_instead() {
        let mut l = Layout::new();
        let area = r(0, 34, 600, 300); // root 594 x 235
        l.recompute(area, 1, inst100(), Profile::Instrument);
        let a = l.root;
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
        let a = l.root;
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
        let root = l.root;
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
            let a = l.root;
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
}
