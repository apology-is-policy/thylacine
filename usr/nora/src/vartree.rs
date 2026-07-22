// nora::vartree -- the Variables tile's variable tree model (pure,
// host-testable). The DAP host (dap_host.rs) owns a forest of these; on each
// publish it flattens the visible nodes into `debug::VarRow`s. A node's children
// are fetched lazily the first time it is expanded, so the model tracks
// expand/fetch state, and the tree ops -- flatten, the flatten<->path inverse
// the row cursor rides, and the by-reference lookup a `variables` response routes
// through -- all live here where they can be unit-tested without a debugger.

use alloc::string::String;
use alloc::vec::Vec;

use crate::debug::VarRow;

/// One node of the Variables tree. A frame's top-level locals are the roots; an
/// expandable node fetches its children the first time it is opened.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VarNode {
    pub name: String,
    pub value: String,
    /// The DAP `variablesReference`: 0 = a leaf, else the handle to fetch this
    /// node's children (`variables(var_ref)`).
    pub var_ref: i64,
    /// Open -- its children are shown (and fetched on the first open).
    pub expanded: bool,
    /// The children have been fetched, so a re-expand does not re-request.
    pub fetched: bool,
    pub children: Vec<VarNode>,
}

impl VarNode {
    /// A fresh, collapsed, unfetched node.
    pub fn new(name: String, value: String, var_ref: i64) -> VarNode {
        VarNode {
            name,
            value,
            var_ref,
            expanded: false,
            fetched: false,
            children: Vec::new(),
        }
    }

    /// The value is structured and can be expanded (a ▸/▾ marker).
    pub fn expandable(&self) -> bool {
        self.var_ref != 0
    }
}

/// Flatten the visible tree (a node, then its children iff expanded) into display
/// rows -- the same depth-first order the editor's row cursor + the renderer
/// walk, so a flattened index is a stable expand/collapse handle.
pub fn flatten(nodes: &[VarNode], depth: u16, out: &mut Vec<VarRow>) {
    for n in nodes {
        out.push(VarRow {
            name: n.name.clone(),
            value: n.value.clone(),
            depth,
            expandable: n.expandable(),
            expanded: n.expanded,
        });
        if n.expanded {
            flatten(&n.children, depth + 1, out);
        }
    }
}

/// The number of currently-visible nodes (== [`flatten`]'s row count).
pub fn visible_count(nodes: &[VarNode]) -> usize {
    let mut count = 0;
    for n in nodes {
        count += 1;
        if n.expanded {
            count += visible_count(&n.children);
        }
    }
    count
}

/// The path (root-to-node child indices) of the `target`-th node in the visible
/// (flattened) order -- the inverse of [`flatten`]'s numbering.
pub fn visible_path(nodes: &[VarNode], target: usize) -> Option<Vec<usize>> {
    fn walk(
        nodes: &[VarNode],
        target: usize,
        counter: &mut usize,
        prefix: &mut Vec<usize>,
    ) -> Option<Vec<usize>> {
        for (idx, n) in nodes.iter().enumerate() {
            prefix.push(idx);
            if *counter == target {
                let found = prefix.clone();
                prefix.pop();
                return Some(found);
            }
            *counter += 1;
            if n.expanded {
                if let Some(found) = walk(&n.children, target, counter, prefix) {
                    prefix.pop();
                    return Some(found);
                }
            }
            prefix.pop();
        }
        None
    }
    let mut counter = 0;
    let mut prefix = Vec::new();
    walk(nodes, target, &mut counter, &mut prefix)
}

/// Descend `path` (from [`visible_path`]) to its node.
pub fn node_at_path_mut<'a>(roots: &'a mut [VarNode], path: &[usize]) -> Option<&'a mut VarNode> {
    let (first, rest) = path.split_first()?;
    let mut cur = roots.get_mut(*first)?;
    for &idx in rest {
        cur = cur.children.get_mut(idx)?;
    }
    Some(cur)
}

/// Find the node whose `var_ref` equals `reference` (references are unique within
/// a stop, so a `variables` response targets exactly one node).
pub fn find_by_ref_mut(nodes: &mut [VarNode], reference: i64) -> Option<&mut VarNode> {
    for n in nodes.iter_mut() {
        if n.var_ref == reference {
            return Some(n);
        }
        if let Some(found) = find_by_ref_mut(&mut n.children, reference) {
            return Some(found);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    // A leaf `i`, and an expandable `p` (ref 2001) with two fetched children,
    // `p` open. Visible order: i, p, p.a, p.b.
    fn sample() -> Vec<VarNode> {
        let mut p = VarNode::new("p".into(), "*T".into(), 2001);
        p.fetched = true;
        p.expanded = true;
        p.children = vec![
            VarNode::new("a".into(), "1".into(), 0),
            VarNode::new("b".into(), "2".into(), 0),
        ];
        vec![VarNode::new("i".into(), "7".into(), 0), p]
    }

    #[test]
    fn flatten_emits_visible_rows_with_depth_and_markers() {
        let mut out = Vec::new();
        flatten(&sample(), 0, &mut out);
        let got: Vec<(&str, u16, bool, bool)> = out
            .iter()
            .map(|r| (r.name.as_str(), r.depth, r.expandable, r.expanded))
            .collect();
        assert_eq!(
            got,
            vec![
                ("i", 0, false, false),  // a leaf
                ("p", 0, true, true),    // expandable + open
                ("a", 1, false, false),  // a child, one level deeper
                ("b", 1, false, false),
            ]
        );
    }

    #[test]
    fn a_collapsed_node_hides_its_children() {
        let mut roots = sample();
        roots[1].expanded = false; // shut `p`
        let mut out = Vec::new();
        flatten(&roots, 0, &mut out);
        let names: Vec<&str> = out.iter().map(|r| r.name.as_str()).collect();
        assert_eq!(names, vec!["i", "p"]); // a, b hidden; p still shows ▸ (expandable)
        assert!(out[1].expandable && !out[1].expanded);
    }

    #[test]
    fn visible_count_matches_flatten_len() {
        let roots = sample();
        let mut out = Vec::new();
        flatten(&roots, 0, &mut out);
        assert_eq!(visible_count(&roots), out.len());
        assert_eq!(visible_count(&roots), 4);
    }

    #[test]
    fn visible_path_inverts_flatten_at_every_index() {
        let mut roots = sample();
        let mut out = Vec::new();
        flatten(&roots, 0, &mut out);
        // For each visible row, its path must resolve back to the node with the
        // same name -- the flatten<->path round-trip the row cursor relies on.
        for (i, row) in out.iter().enumerate() {
            let path = visible_path(&roots, i).expect("a path for every visible row");
            let node = node_at_path_mut(&mut roots, &path).expect("path resolves");
            assert_eq!(node.name, row.name, "row {i}");
        }
        // Out of range -> None.
        assert!(visible_path(&roots, out.len()).is_none());
    }

    #[test]
    fn find_by_ref_reaches_a_nested_node() {
        let mut roots = sample();
        // The child `a`/`b` have ref 0; `p` has 2001. Give `a` a ref + descend.
        roots[1].children[0].var_ref = 3001;
        let found = find_by_ref_mut(&mut roots, 3001).expect("finds the nested node");
        assert_eq!(found.name, "a");
        assert!(find_by_ref_mut(&mut roots, 9999).is_none());
    }

    #[test]
    fn toggling_a_node_via_its_path_changes_visibility() {
        let mut roots = sample();
        // Collapse `p` (visible index 1) via its path.
        let path = visible_path(&roots, 1).unwrap();
        node_at_path_mut(&mut roots, &path).unwrap().expanded = false;
        assert_eq!(visible_count(&roots), 2); // i, p (a, b hidden)
        // Re-expand -> the cached children reappear (no re-fetch needed).
        let path = visible_path(&roots, 1).unwrap();
        node_at_path_mut(&mut roots, &path).unwrap().expanded = true;
        assert_eq!(visible_count(&roots), 4);
    }
}
