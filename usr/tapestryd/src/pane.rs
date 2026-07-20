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
//   - geometry: equal division (remainder to the last child), a 1px
//     content inset per pane iff more than one pane is visible (the
//     single-fullscreen root leaf keeps the stage-0 borderless look).

use alloc::string::String;
use alloc::vec::Vec;

pub const MAX_PANES: usize = 32;

/// Border/background palette (compositor chrome; glyph-free per D7).
pub const BG_COLOR: u32 = 0xFF10_1014;
pub const BORDER_COLOR: u32 = 0xFF3A_3A44;
pub const FOCUS_COLOR: u32 = 0xFF7A_9ECC;

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
}

impl Role {
    pub fn name(self) -> &'static str {
        match self {
            Role::Content => "content",
            Role::Chrome => "chrome",
            Role::PinTarget => "pin-target",
        }
    }
    pub fn parse(s: &str) -> Option<Role> {
        match s {
            "content" => Some(Role::Content),
            "chrome" => Some(Role::Chrome),
            "pin-target" => Some(Role::PinTarget),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

impl Rect {
    pub const ZERO: Rect = Rect { x: 0, y: 0, w: 0, h: 0 };

    /// Intersection (empty rects collapse to ZERO).
    pub fn intersect(self, o: Rect) -> Rect {
        let x1 = self.x.max(o.x);
        let y1 = self.y.max(o.y);
        let x2 = (self.x + self.w).min(o.x + o.w);
        let y2 = (self.y + self.h).min(o.y + o.h);
        if x2 <= x1 || y2 <= y1 {
            return Rect::ZERO;
        }
        Rect { x: x1, y: y1, w: x2 - x1, h: y2 - y1 }
    }
    pub fn is_empty(self) -> bool {
        self.w == 0 || self.h == 0
    }
}

pub enum Kind {
    Leaf { surface: Option<usize> },
    Container { mode: Mode, children: Vec<usize>, active: usize },
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
    /// The content rect (outer minus the border inset; ZERO when hidden).
    pub content: Rect,
    /// Visible under the current layout (tab-inactive subtrees are not).
    pub visible: bool,
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
}

impl Layout {
    pub fn new() -> Layout {
        let mut l = Layout {
            panes: Vec::new(),
            root: 0,
            focused: 0,
            id_seq: 0,
            epoch: 1,
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
        self.id_seq += 1;
        let p = Pane {
            id: self.id_seq,
            parent,
            kind,
            role: Role::Content,
            focusable: true,
            tag: String::new(),
            rect: Rect::ZERO,
            content: Rect::ZERO,
            visible: false,
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

    /// The leaf hosting surface `n` (linear scan; the table is small).
    pub fn find_hosting(&self, n: usize) -> Option<usize> {
        self.panes.iter().enumerate().find_map(|(i, p)| match p {
            Some(Pane { kind: Kind::Leaf { surface: Some(s) }, .. }) if *s == n => Some(i),
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
                let new_leaf = self.alloc(Some(pi), Kind::Leaf { surface: None })?;
                if let Some(Kind::Container { children, active, .. }) =
                    self.get_mut(pi).map(|p| &mut p.kind)
                {
                    let at = children.iter().position(|&c| c == slot).unwrap_or(0);
                    children.insert(at + 1, new_leaf);
                    *active = at + 1;
                }
                self.focused = new_leaf;
                self.epoch += 1;
                return Some(new_leaf);
            }
        }
        // Nest: the leaf's position becomes a container [leaf, new-leaf].
        let container = self.alloc(parent, Kind::Container {
            mode,
            children: Vec::new(),
            active: 1,
        })?;
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
        if let Some(Kind::Container { children, .. }) =
            self.get_mut(container).map(|p| &mut p.kind)
        {
            children.push(slot);
            children.push(new_leaf);
        }
        self.focused = new_leaf;
        self.epoch += 1;
        Some(new_leaf)
    }

    /// Host surface `n` into the focused leaf if empty, else split the
    /// focused leaf (orientation by aspect) and host into the new leaf.
    /// Returns the hosting slot (None: pane table exhausted).
    pub fn host(&mut self, n: usize) -> Option<usize> {
        let f = self.focused;
        if let Some(Kind::Leaf { surface: s @ None }) =
            self.get_mut(f).map(|p| &mut p.kind)
        {
            *s = Some(n);
            self.epoch += 1;
            return Some(f);
        }
        let r = self.get(f)?.content;
        let mode = if r.w >= r.h { Mode::SplitH } else { Mode::SplitV };
        let leaf = self.split(f, mode)?;
        if let Some(Kind::Leaf { surface }) = self.get_mut(leaf).map(|p| &mut p.kind) {
            *surface = Some(n);
        }
        self.epoch += 1;
        Some(leaf)
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
        if let Some(Kind::Container { children, active, .. }) =
            self.get_mut(parent).map(|p| &mut p.kind)
        {
            if let Some(at) = children.iter().position(|&c| c == slot) {
                children.remove(at);
                if *active >= children.len() && !children.is_empty() {
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
        let (only, gp) = match self.get(slot) {
            Some(Pane { kind: Kind::Container { children, .. }, parent, .. })
                if children.len() == 1 =>
            {
                (children[0], *parent)
            }
            _ => return,
        };
        match gp {
            Some(g) => {
                if let Some(Kind::Container { children, .. }) =
                    self.get_mut(g).map(|p| &mut p.kind)
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
            Some(Kind::Container { children, active, .. }) => {
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
                    if let Some(Kind::Container { children, active, .. }) =
                        self.get_mut(pi).map(|p| &mut p.kind)
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

    /// Set a container's mode (a leaf targets its parent container --
    /// the i3 shape). False = no container to act on.
    pub fn set_mode(&mut self, slot: usize, mode: Mode) -> bool {
        let target = match self.get(slot).map(|p| &p.kind) {
            Some(Kind::Container { .. }) => Some(slot),
            Some(Kind::Leaf { .. }) => self.get(slot).and_then(|p| p.parent),
            None => None,
        };
        match target {
            Some(t) => {
                if let Some(Kind::Container { mode: m, .. }) =
                    self.get_mut(t).map(|p| &mut p.kind)
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

    /// The number of visible leaves after the last `recompute` -- the
    /// border-inset decision input + the scanout-mode predicate.
    pub fn visible_leaf_count(&self) -> usize {
        self.panes
            .iter()
            .filter(|p| {
                matches!(p, Some(Pane { kind: Kind::Leaf { .. }, visible: true, .. }))
            })
            .count()
    }

    /// Recompute geometry + visibility for the whole tree.
    pub fn recompute(&mut self, disp_w: u32, disp_h: u32) {
        // Pass 1: mark everything hidden, then walk the visible tree.
        for p in self.panes.iter_mut().flatten() {
            p.visible = false;
            p.rect = Rect::ZERO;
            p.content = Rect::ZERO;
        }
        let root = self.root;
        self.layout_pane(root, Rect { x: 0, y: 0, w: disp_w, h: disp_h });
        // Pass 2: the content inset -- 1px per leaf iff >1 leaf visible.
        let inset = if self.visible_leaf_count() > 1 { 1u32 } else { 0 };
        for p in self.panes.iter_mut().flatten() {
            if !p.visible {
                continue;
            }
            let r = p.rect;
            p.content = if inset > 0
                && matches!(p.kind, Kind::Leaf { .. })
                && r.w > 2 * inset
                && r.h > 2 * inset
            {
                Rect {
                    x: r.x + inset,
                    y: r.y + inset,
                    w: r.w - 2 * inset,
                    h: r.h - 2 * inset,
                }
            } else {
                r
            };
        }
    }

    fn layout_pane(&mut self, slot: usize, rect: Rect) {
        enum Next {
            Done,
            One(usize),
            Split(Mode, Vec<usize>),
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
                Kind::Container { mode, children, active } => match mode {
                    // Tab/stack: only the active child is visible, full rect.
                    Mode::Tabbed | Mode::Stacked => match children.get(*active).copied() {
                        Some(a) => Next::One(a),
                        None => Next::Done,
                    },
                    m => Next::Split(*m, children.clone()),
                },
            }
        };
        match next {
            Next::Done => {}
            Next::One(a) => self.layout_pane(a, rect),
            Next::Split(mode, children) => {
                let n = children.len() as u32;
                if n == 0 {
                    return;
                }
                if mode == Mode::SplitH {
                    let each = rect.w / n;
                    let mut x = rect.x;
                    for (i, &c) in children.iter().enumerate() {
                        let w = if i as u32 == n - 1 { rect.x + rect.w - x } else { each };
                        self.layout_pane(c, Rect { x, y: rect.y, w, h: rect.h });
                        x += w;
                    }
                } else {
                    let each = rect.h / n;
                    let mut y = rect.y;
                    for (i, &c) in children.iter().enumerate() {
                        let h = if i as u32 == n - 1 { rect.y + rect.h - y } else { each };
                        self.layout_pane(c, Rect { x: rect.x, y, w: rect.w, h });
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
            format_args!("epoch {} focused {}\n", self.epoch,
                self.id_of(self.focused).unwrap_or(0)),
        );
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
            Kind::Container { mode, children, active } => {
                let _ = core::fmt::write(
                    s,
                    format_args!("{}{} {} n={} active={}", p.id, star, mode.name(),
                        children.len(), active),
                );
            }
        }
        let c = p.content;
        let _ = core::fmt::write(
            s,
            format_args!(" [{},{},{},{}]{}\n", c.x, c.y, c.w, c.h,
                if p.visible { "" } else { " hidden" }),
        );
        if let Kind::Container { children, .. } = &p.kind {
            for &c in children {
                self.render_pane(s, c, depth + 1);
            }
        }
    }
}
