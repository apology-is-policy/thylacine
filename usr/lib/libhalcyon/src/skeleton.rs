// skeleton -- the pure RESTORE planner (HALCYON.md 13.7, H-4b-3b).
//
// A saved tree comes back as a sequence of compositor verbs. The compositor
// offers exactly two structural primitives -- `split <leaf> h|v` (which NESTS
// a new container when the leaf's parent has a different mode and FLATTENS
// into the parent when the modes agree, always inserting the new empty leaf
// right after the split one) and `mode <container> <m>` -- so a tree is built
// leaf-first: for each container, split its first leaf N-1 times (the first
// split nests, the rest flatten into that container), fix a tabbed/stacked
// mode afterwards, then recurse into each child's leaf. Alternating modes
// (what the compositor itself produces) nest at every level by construction;
// a hand-written same-mode nesting flattens into its parent, which is the
// shape the compositor would give a user splitting by hand.
//
// The planner is a MODEL of the compositor's split rule. It names leaves and
// containers by symbolic refs; the executor (the `halcyon` tool, or a future
// renderer-side restore) resolves each ref by diffing the live `layout` dump
// after every verb, and checks the model's nest/flatten expectation against
// what the compositor actually did -- a mismatch aborts the restore instead of
// placing programs into the wrong tiles.
//
// Ref 0 is the ANCHOR: the empty leaf the tree grows from. The session tool
// gets it by splitting a placeholder surface it created beside the console
// (it cannot split the console's leaf -- not its tile); a renderer-authority
// restore may anchor on the root leaf itself.

use alloc::string::String;
use alloc::vec::Vec;

use crate::layout::{LayoutMode, LayoutNode};

/// A symbolic leaf (0 = the anchor); the executor maps it to a pane id.
pub type LeafRef = usize;
/// A symbolic container the plan expects a nesting split to create.
pub type ContRef = usize;

/// The two directions `split` accepts.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SplitDir {
    H,
    V,
}

impl SplitDir {
    /// The verb argument (`split <id> h|v`).
    pub fn verb(self) -> &'static str {
        match self {
            SplitDir::H => "h",
            SplitDir::V => "v",
        }
    }
    /// The container mode a split in this direction creates or joins.
    pub fn mode(self) -> LayoutMode {
        match self {
            SplitDir::H => LayoutMode::SplitH,
            SplitDir::V => LayoutMode::SplitV,
        }
    }
    fn of(mode: LayoutMode) -> Option<SplitDir> {
        match mode {
            LayoutMode::SplitH => Some(SplitDir::H),
            LayoutMode::SplitV => Some(SplitDir::V),
            LayoutMode::Tabbed | LayoutMode::Stacked => None,
        }
    }
}

/// One compositor verb.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Op {
    /// `split <at> <dir>`: the compositor yields one new empty leaf
    /// (`new_leaf`); when `nests` is Some it must ALSO yield one new
    /// container (the plan's model says the parent's mode differs), and when
    /// None it must yield none (a flatten). The executor verifies both.
    Split {
        at: LeafRef,
        dir: SplitDir,
        new_leaf: LeafRef,
        nests: Option<ContRef>,
    },
    /// `mode <cont> <mode>` on a container a nesting split created.
    SetMode { cont: ContRef, mode: LayoutMode },
}

/// A leaf of the finished skeleton and the command line it should host
/// (empty = an empty pane; nothing is spawned).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PlannedLeaf {
    pub leaf: LeafRef,
    pub tag: String,
}

#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Plan {
    /// The verbs, in order.
    pub ops: Vec<Op>,
    /// Every leaf of the tree, pre-order, with its tag.
    pub leaves: Vec<PlannedLeaf>,
    /// The leaves to `focus`, in order, to reproduce every container's
    /// `active` (post-order: each container's active-path leaf after its
    /// descendants', so the shallower focus wins on shared ancestors); the
    /// last entry is the tree's own active path -- the final focus.
    pub focus: Vec<LeafRef>,
    /// How many leaf refs the plan uses (0..leaf_count).
    pub leaf_count: usize,
    /// How many container refs the plan uses (0..cont_count).
    pub cont_count: usize,
}

/// A split direction the anchor's parent is NOT (so a split there nests).
fn away_from(parent: Option<LayoutMode>) -> SplitDir {
    if parent == Some(LayoutMode::SplitH) {
        SplitDir::V
    } else {
        SplitDir::H
    }
}

/// The direction of the session tool's INITIAL split -- the one that makes
/// the anchor beside its placeholder. A split-mode root gets its own
/// direction, so the root's children later flatten into that container and
/// the placeholder's removal leaves exactly the root; any other root (a
/// leaf, tabbed, stacked) gets a direction the outer container is not, so
/// the removal dissolves the throwaway container cleanly. `outer` is the mode
/// of the placeholder leaf's parent (None when the placeholder is the root).
pub fn anchor_split(root: &LayoutNode, outer: Option<LayoutMode>) -> SplitDir {
    match root {
        LayoutNode::Container { mode, .. } => SplitDir::of(*mode).unwrap_or_else(|| away_from(outer)),
        LayoutNode::Leaf { .. } => away_from(outer),
    }
}

/// Plan the verbs that grow `root` from anchor leaf 0, whose parent container
/// has mode `anchor_parent` (None: the anchor is the root leaf). `root` must
/// already be pruned of `env` leaves (`layout::prune_env`); an env leaf here
/// is planned like any other and its tag would be spawned.
pub fn plan(root: &LayoutNode, anchor_parent: Option<LayoutMode>) -> Plan {
    let mut p = Plan {
        leaf_count: 1,
        ..Default::default()
    };
    let last = build(root, 0, anchor_parent, &mut p);
    push_focus(&mut p, last);
    p
}

/// Append a focus step, skipping an immediate repeat (a container whose
/// active path ends where its active child's already did).
fn push_focus(p: &mut Plan, leaf: LeafRef) {
    if p.focus.last() != Some(&leaf) {
        p.focus.push(leaf);
    }
}

/// Grow `node` at `leaf` (whose parent container has mode `parent`); returns
/// the leaf on the node's active path (the leaf `focus` reveals it by).
fn build(node: &LayoutNode, leaf: LeafRef, parent: Option<LayoutMode>, p: &mut Plan) -> LeafRef {
    match node {
        LayoutNode::Leaf { tag, .. } => {
            p.leaves.push(PlannedLeaf {
                leaf,
                tag: tag.clone(),
            });
            leaf
        }
        LayoutNode::Container {
            mode,
            active,
            children,
        } => {
            // A one-child container cannot exist in the compositor (it
            // dissolves); the child takes the slot and the mode is dropped.
            if children.len() == 1 {
                return build(&children[0], leaf, parent, p);
            }
            if children.is_empty() {
                return leaf; // unreachable past the parser; nothing to grow
            }
            let dir = SplitDir::of(*mode).unwrap_or_else(|| away_from(parent));
            let nests = parent != Some(dir.mode());
            let cont = if nests {
                let c = p.cont_count;
                p.cont_count += 1;
                Some(c)
            } else {
                None
            };
            let mut refs: Vec<LeafRef> = Vec::with_capacity(children.len());
            refs.push(leaf);
            let mut prev = leaf;
            for i in 1..children.len() {
                let r = p.leaf_count;
                p.leaf_count += 1;
                p.ops.push(Op::Split {
                    at: prev,
                    dir,
                    new_leaf: r,
                    nests: if i == 1 { cont } else { None },
                });
                prev = r;
                refs.push(r);
            }
            if *mode != dir.mode() {
                // Tabbed/stacked: `dir` was chosen away from the parent's
                // mode, so the first split nested and `cont` names it.
                if let Some(c) = cont {
                    p.ops.push(Op::SetMode { cont: c, mode: *mode });
                }
            }
            // The children live in a container of mode `mode` now (their
            // own, or the parent's when the group flattened into it -- the
            // modes agree in that case).
            let mut actives: Vec<LeafRef> = Vec::with_capacity(children.len());
            for (c, r) in children.iter().zip(refs.iter()) {
                actives.push(build(c, *r, Some(*mode), p));
            }
            let a = (*active as usize).min(children.len() - 1);
            let mine = actives[a];
            push_focus(p, mine);
            mine
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    fn leaf(t: &str) -> LayoutNode {
        LayoutNode::Leaf {
            tag: t.to_string(),
            env: false,
        }
    }
    fn cont(m: LayoutMode, a: u32, c: Vec<LayoutNode>) -> LayoutNode {
        LayoutNode::Container {
            mode: m,
            active: a,
            children: c,
        }
    }
    fn split(at: LeafRef, dir: SplitDir, new_leaf: LeafRef, nests: Option<ContRef>) -> Op {
        Op::Split {
            at,
            dir,
            new_leaf,
            nests,
        }
    }
    fn pl(leaf: LeafRef, tag: &str) -> PlannedLeaf {
        PlannedLeaf {
            leaf,
            tag: tag.to_string(),
        }
    }

    /// Every leaf ref is used exactly once, and they are exactly 0..count.
    fn refs_are_a_permutation(p: &Plan) {
        let mut seen = vec![false; p.leaf_count];
        for l in &p.leaves {
            assert!(!seen[l.leaf], "leaf ref {} planned twice", l.leaf);
            seen[l.leaf] = true;
        }
        assert!(seen.iter().all(|&s| s), "unused leaf ref: {:?}", seen);
        let mut minted = vec![false; p.leaf_count];
        minted[0] = true;
        for op in &p.ops {
            if let Op::Split { new_leaf, .. } = op {
                assert!(!minted[*new_leaf], "leaf ref {} split twice", new_leaf);
                minted[*new_leaf] = true;
            }
        }
        assert!(minted.iter().all(|&m| m));
        for f in &p.focus {
            assert!(*f < p.leaf_count);
        }
    }

    #[test]
    fn a_single_leaf_needs_no_verbs() {
        let p = plan(&leaf("tapestry-demo"), Some(LayoutMode::SplitH));
        assert!(p.ops.is_empty());
        assert_eq!(p.leaves, vec![pl(0, "tapestry-demo")]);
        assert_eq!(p.focus, vec![0]);
        assert_eq!((p.leaf_count, p.cont_count), (1, 0));
        refs_are_a_permutation(&p);
    }

    #[test]
    fn a_root_split_flattens_into_the_anchor_container() {
        // The tool's initial split already made a SplitH container: the
        // root's two leaves flatten into it (no new container).
        let t = cont(LayoutMode::SplitH, 1, vec![leaf("a"), leaf("b")]);
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert_eq!(p.ops, vec![split(0, SplitDir::H, 1, None)]);
        assert_eq!(p.leaves, vec![pl(0, "a"), pl(1, "b")]);
        assert_eq!(p.focus, vec![1]);
        assert_eq!((p.leaf_count, p.cont_count), (2, 0));
        refs_are_a_permutation(&p);
    }

    #[test]
    fn the_restore_e2e_tree() {
        // splitv[ demo, splith[ demo, (empty) ] ], active path -> the second
        // demo. Anchored in a SplitV container (the initial split's).
        let t = cont(
            LayoutMode::SplitV,
            1,
            vec![
                leaf("tapestry-demo"),
                cont(LayoutMode::SplitH, 0, vec![leaf("tapestry-demo"), leaf("")]),
            ],
        );
        let p = plan(&t, Some(LayoutMode::SplitV));
        assert_eq!(
            p.ops,
            vec![
                split(0, SplitDir::V, 1, None),    // the root flattens
                split(1, SplitDir::H, 2, Some(0)), // the inner splith nests
            ]
        );
        assert_eq!(
            p.leaves,
            vec![pl(0, "tapestry-demo"), pl(1, "tapestry-demo"), pl(2, "")]
        );
        // Post-order: the inner container's active (its first leaf, ref 1),
        // then the root's active path (child 1 -> ref 1) -- deduplicated.
        assert_eq!(p.focus, vec![1]);
        assert_eq!((p.leaf_count, p.cont_count), (3, 1));
        refs_are_a_permutation(&p);
    }

    #[test]
    fn a_tabbed_container_is_split_away_from_its_parent_then_moded() {
        let t = cont(LayoutMode::Tabbed, 2, vec![leaf("a"), leaf("b"), leaf("c")]);
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert_eq!(
            p.ops,
            vec![
                split(0, SplitDir::V, 1, Some(0)),
                split(1, SplitDir::V, 2, None),
                Op::SetMode {
                    cont: 0,
                    mode: LayoutMode::Tabbed
                },
            ]
        );
        assert_eq!(p.leaves, vec![pl(0, "a"), pl(1, "b"), pl(2, "c")]);
        assert_eq!(p.focus, vec![2]);
        refs_are_a_permutation(&p);
        // Under a SplitV parent the away-direction is H.
        let p = plan(&t, Some(LayoutMode::SplitV));
        assert!(matches!(p.ops[0], Op::Split { dir: SplitDir::H, nests: Some(0), .. }));
        // With no parent at all (a renderer anchoring on the root) any
        // direction nests; H is the choice.
        let p = plan(&t, None);
        assert!(matches!(p.ops[0], Op::Split { dir: SplitDir::H, nests: Some(0), .. }));
    }

    #[test]
    fn nested_tabbed_inside_tabbed_nests_at_both_levels() {
        let t = cont(
            LayoutMode::Tabbed,
            1,
            vec![
                cont(LayoutMode::Tabbed, 1, vec![leaf("a"), leaf("b")]),
                leaf("c"),
            ],
        );
        let p = plan(&t, None);
        assert_eq!(
            p.ops,
            vec![
                split(0, SplitDir::H, 1, Some(0)),
                Op::SetMode {
                    cont: 0,
                    mode: LayoutMode::Tabbed
                },
                // The inner tabbed grows from leaf 0 under a Tabbed parent:
                // an H split nests again (Tabbed != SplitH).
                split(0, SplitDir::H, 2, Some(1)),
                Op::SetMode {
                    cont: 1,
                    mode: LayoutMode::Tabbed
                },
            ]
        );
        assert_eq!(p.leaves, vec![pl(0, "a"), pl(2, "b"), pl(1, "c")]);
        // Inner active -> b (ref 2); outer active -> c (ref 1): both
        // focused, the outer last.
        assert_eq!(p.focus, vec![2, 1]);
        assert_eq!((p.leaf_count, p.cont_count), (3, 2));
        refs_are_a_permutation(&p);
    }

    #[test]
    fn alternating_modes_nest_at_every_level() {
        // splith[ a, splitv[ b, splith[ c, d ] ] ] -- what render_text emits.
        let t = cont(
            LayoutMode::SplitH,
            0,
            vec![
                leaf("a"),
                cont(
                    LayoutMode::SplitV,
                    0,
                    vec![leaf("b"), cont(LayoutMode::SplitH, 1, vec![leaf("c"), leaf("d")])],
                ),
            ],
        );
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert_eq!(
            p.ops,
            vec![
                split(0, SplitDir::H, 1, None),
                split(1, SplitDir::V, 2, Some(0)),
                split(2, SplitDir::H, 3, Some(1)),
            ]
        );
        assert_eq!(p.leaves, vec![pl(0, "a"), pl(1, "b"), pl(2, "c"), pl(3, "d")]);
        // Post-order: innermost active d (3), then splitv's active b (1),
        // then the root's active a (0).
        assert_eq!(p.focus, vec![3, 1, 0]);
        refs_are_a_permutation(&p);
    }

    #[test]
    fn same_mode_nesting_flattens_like_the_compositor_would() {
        // A hand-written splith inside a splith: no compositor verb can nest
        // them, so the inner children flatten into the outer container.
        let t = cont(
            LayoutMode::SplitH,
            0,
            vec![leaf("a"), cont(LayoutMode::SplitH, 0, vec![leaf("b"), leaf("c")])],
        );
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert_eq!(
            p.ops,
            vec![split(0, SplitDir::H, 1, None), split(1, SplitDir::H, 2, None)]
        );
        assert_eq!(p.cont_count, 0);
        refs_are_a_permutation(&p);
    }

    #[test]
    fn a_one_child_container_dissolves_into_its_child() {
        let t = cont(LayoutMode::Tabbed, 0, vec![leaf("only")]);
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert!(p.ops.is_empty());
        assert_eq!(p.leaves, vec![pl(0, "only")]);
        assert_eq!(p.focus, vec![0]);
    }

    #[test]
    fn active_is_clamped_into_range() {
        let t = cont(LayoutMode::SplitV, 7, vec![leaf("a"), leaf("b")]);
        let p = plan(&t, None);
        assert_eq!(p.focus, vec![1]);
    }

    #[test]
    fn anchor_split_direction() {
        // A split root: its own direction, whatever the outer container is.
        let h = cont(LayoutMode::SplitH, 0, vec![leaf("a"), leaf("b")]);
        assert_eq!(anchor_split(&h, Some(LayoutMode::SplitH)), SplitDir::H);
        assert_eq!(anchor_split(&h, Some(LayoutMode::SplitV)), SplitDir::H);
        let v = cont(LayoutMode::SplitV, 0, vec![leaf("a"), leaf("b")]);
        assert_eq!(anchor_split(&v, Some(LayoutMode::SplitH)), SplitDir::V);
        // A leaf / tabbed root: away from the outer mode (nests), H by default.
        assert_eq!(anchor_split(&leaf("a"), Some(LayoutMode::SplitH)), SplitDir::V);
        assert_eq!(anchor_split(&leaf("a"), Some(LayoutMode::SplitV)), SplitDir::H);
        assert_eq!(anchor_split(&leaf("a"), None), SplitDir::H);
        let t = cont(LayoutMode::Tabbed, 0, vec![leaf("a"), leaf("b")]);
        assert_eq!(anchor_split(&t, Some(LayoutMode::SplitH)), SplitDir::V);
        assert_eq!(anchor_split(&t, Some(LayoutMode::Tabbed)), SplitDir::H);
    }

    #[test]
    fn a_wide_tree_stays_within_the_layout_bounds() {
        // MAX_NODES leaves in one container: MAX_NODES-1 splits, one nest.
        let n = crate::layout::MAX_NODES - 1;
        let kids: Vec<LayoutNode> = (0..n).map(|_| leaf("x")).collect();
        let t = cont(LayoutMode::SplitV, 0, kids);
        let p = plan(&t, Some(LayoutMode::SplitH));
        assert_eq!(p.ops.len(), n - 1);
        assert_eq!(p.leaf_count, n);
        assert_eq!(p.cont_count, 1);
        refs_are_a_permutation(&p);
    }
}
