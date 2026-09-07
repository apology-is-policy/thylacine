// halcyond::tiles -- the pure per-leaf session-tile PLANNER (HALCYON.md
// 14.11.6, KT-1.5d-3). The per-user session compositor hosts one terminal
// tile per compositor leaf, reconciled off the `layout` file each relayout.
// This is the pure half of the 13.1 lib/bin split: given the parsed leaves
// and the leaves we already host (plus the leaves we have permanently
// closed), it computes the create/drop diff. The bin (`session.rs`) does the
// I/O -- claim, spawn, poll, ingest -- against this plan. Host-tested here so
// the diff logic cannot regress silently in the untestable guest body.

use crate::chrome::Leaf;
use alloc::string::String;
use alloc::vec::Vec;
use libhalcyon::tag::{argv_of, resolve_prog};

/// The session's shell.
pub const TILE_SHELL: &str = "/bin/ut";

/// H-4d: the argv a tile's `kaua-term` hosts for an empty leaf's tag -- the
/// tag IS the tile's command line (acme; rio's `window cmd`): empty = the
/// shell; else the tag's words, its program resolved through the shell's
/// search (`exists` probes). The shell gets the session's `--home` when the
/// tag did not name one, so a tagged `ut` is the same shell the compositor
/// spawns by default.
pub fn tile_command(tag: &str, home: Option<&str>, exists: impl Fn(&str) -> bool) -> Vec<String> {
    let words = argv_of(tag);
    let mut out: Vec<String> = Vec::new();
    match words.first() {
        None => out.push(String::from(TILE_SHELL)),
        Some(p) => {
            out.push(resolve_prog(p, exists));
            out.extend(words[1..].iter().map(|w| String::from(*w)));
        }
    }
    let is_shell = out[0] == TILE_SHELL || out[0].ends_with("/ut");
    if is_shell && !out.iter().any(|w| w == "--home") {
        if let Some(h) = home {
            out.push(String::from("--home"));
            out.push(String::from(h));
        }
    }
    out
}

/// The reconcile diff: leaves that need a new tile, and tiles whose leaf is
/// gone (to be reaped).
pub struct SessionPlan {
    /// Empty leaves we do not yet host and have not closed -- candidates for
    /// a new tile. The runtime still gates each on `pane/<id>/claim`, which
    /// is the compositor's owner+emptiness authority (HALCYON.md 13.7): a
    /// candidate whose claim read fails is not ours and is dropped there.
    pub create: Vec<u32>,
    /// Leaf ids we host whose leaf has vanished from the layout -- orphaned
    /// tiles to tear down.
    pub drop: Vec<u32>,
}

/// Diff the parsed layout against what we host. A leaf is a create candidate
/// iff it is EMPTY (`surface=None`), we do not already host it, and we have
/// not closed it. The `closed` set is the respawn guard: a leaf whose tile
/// exited or was closed must NEVER be refilled -- otherwise a lingering empty
/// leaf we still own (a `close` verb that failed its budget, say) would be
/// re-claimed every reconcile, a fork-bomb of kaua-terms. Leaf ids are never
/// reused, so a closed id stays a safe permanent exclusion.
///
/// A leaf occupied by another actor's surface (`surface=Some`, not ours) is
/// never a candidate: only empty leaves are claimable, and the claim would
/// fail anyway. A leaf we host that vanished is a drop -- vanished from the
/// TREE, never merely hidden (a zoom or a tab hides a hosted leaf; its shell
/// keeps running, and it is filled again when shown). A hidden empty leaf is
/// not created either: it gets no geometry until it shows.
pub fn plan_tiles(leaves: &[Leaf], have: &[u32], closed: &[u32]) -> SessionPlan {
    let create = leaves
        .iter()
        .filter(|l| {
            l.surface.is_none() && !l.hidden && !have.contains(&l.id) && !closed.contains(&l.id)
        })
        .map(|l| l.id)
        .collect();
    let drop = have
        .iter()
        .copied()
        .filter(|id| !leaves.iter().any(|l| l.id == *id))
        .collect();
    SessionPlan { create, drop }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn leaf(id: u32, focused: bool, surface: Option<u32>) -> Leaf {
        Leaf {
            id,
            focused,
            surface,
            hidden: false,
        }
    }

    #[test]
    fn an_empty_tag_is_the_shell_with_the_home() {
        assert_eq!(
            tile_command("", Some("/home/cora"), |_| false),
            ["/bin/ut", "--home", "/home/cora"]
        );
        assert_eq!(tile_command("  ", None, |_| false), ["/bin/ut"]);
    }

    #[test]
    fn a_tag_is_resolved_through_the_shell_search() {
        // A bare name found in /goroot/bin resolves there; its args ride.
        assert_eq!(
            tile_command("hx notes.txt", Some("/home/cora"), |p| p
                == "/goroot/bin/hx"),
            ["/goroot/bin/hx", "notes.txt"]
        );
        // Not found anywhere: the shell-identical first candidate.
        assert_eq!(
            tile_command("nope", Some("/home/cora"), |_| false),
            ["/bin/nope"]
        );
        // A tagged shell gets the home once, never twice.
        assert_eq!(
            tile_command("ut", Some("/home/cora"), |p| p == "/bin/ut"),
            ["/bin/ut", "--home", "/home/cora"]
        );
        assert_eq!(
            tile_command("ut --home /tmp/x", Some("/home/cora"), |p| p == "/bin/ut"),
            ["/bin/ut", "--home", "/tmp/x"]
        );
    }

    #[test]
    fn new_empty_leaf_is_a_create() {
        // Leaf 5 is empty and unowned by us -> claim it.
        let leaves = vec![leaf(1, false, Some(100)), leaf(5, true, None)];
        let have = [1u32];
        let plan = plan_tiles(&leaves, &have, &[]);
        assert_eq!(plan.create, vec![5]);
        assert!(plan.drop.is_empty());
    }

    #[test]
    fn a_leaf_we_host_is_neither_created_nor_dropped() {
        // Leaf 1 hosts our surface 100 (surface=Some, in have): kept.
        let leaves = vec![leaf(1, true, Some(100))];
        let have = [1u32];
        let plan = plan_tiles(&leaves, &have, &[]);
        assert!(plan.create.is_empty());
        assert!(plan.drop.is_empty());
    }

    #[test]
    fn a_vanished_leaf_is_a_drop() {
        // We host 1 and 2; the layout now shows only 1 -> reap 2.
        let leaves = vec![leaf(1, true, Some(100))];
        let have = [1u32, 2u32];
        let plan = plan_tiles(&leaves, &have, &[]);
        assert!(plan.create.is_empty());
        assert_eq!(plan.drop, vec![2]);
    }

    #[test]
    fn an_occupied_foreign_leaf_is_never_a_create() {
        // Leaf 9 holds someone else's surface (surface=Some, not in have):
        // not empty, so not claimable -- skip it.
        let leaves = vec![leaf(1, true, Some(100)), leaf(9, false, Some(777))];
        let have = [1u32];
        let plan = plan_tiles(&leaves, &have, &[]);
        assert!(plan.create.is_empty());
        assert!(plan.drop.is_empty());
    }

    #[test]
    fn a_closed_leaf_is_never_refilled() {
        // Leaf 3's tile exited and its `close` did not collapse it yet, so it
        // lingers empty AND ours -- the respawn guard must exclude it.
        let leaves = vec![leaf(1, true, Some(100)), leaf(3, false, None)];
        let have = [1u32];
        let closed = [3u32];
        let plan = plan_tiles(&leaves, &have, &closed);
        assert!(plan.create.is_empty(), "a closed leaf must not respawn");
        assert!(plan.drop.is_empty());
    }

    #[test]
    fn two_new_empty_leaves_both_create() {
        // The welcome / a burst of splits: two empty leaves at once, both
        // claimable (H-4d's two-pane precondition).
        let leaves = vec![leaf(1, false, None), leaf(2, true, None)];
        let plan = plan_tiles(&leaves, &[], &[]);
        assert_eq!(plan.create, vec![1, 2]);
    }

    #[test]
    fn a_hidden_leaf_we_host_is_neither_created_nor_dropped() {
        // A zoom (or a tab) hides leaf 2 -- its shell keeps running. Hidden
        // is not vanished: dropping it here would kill that shell.
        let mut hidden = leaf(2, false, Some(101));
        hidden.hidden = true;
        let leaves = vec![leaf(1, true, Some(100)), hidden];
        let have = [1u32, 2u32];
        let plan = plan_tiles(&leaves, &have, &[]);
        assert!(plan.create.is_empty());
        assert!(
            plan.drop.is_empty(),
            "a hidden hosted leaf must not be dropped"
        );
    }

    #[test]
    fn a_hidden_empty_leaf_is_not_created() {
        // An empty leaf hidden under a zoom has no geometry to size a tile
        // by; it is created once it shows.
        let mut hidden = leaf(3, false, None);
        hidden.hidden = true;
        let leaves = vec![leaf(1, true, Some(100)), hidden];
        let plan = plan_tiles(&leaves, &[1u32], &[]);
        assert!(plan.create.is_empty());
        assert!(plan.drop.is_empty());
    }
}
