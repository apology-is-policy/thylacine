//! Bounded, per-tile rasters referenced by standalone Beacon objects.
//! The text stream owns ordering; this cache supplies pixels, never positions.
use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec::Vec;
use crate::transcript::{Block, Item};

pub struct Raster { pub w: u32, pub h: u32, pub argb: Vec<u32> }
pub struct InlineCache {
    images: BTreeMap<u128, Raster>,
    order: VecDeque<u128>,
    bytes: usize,
    limit: usize,
}
impl InlineCache {
    pub fn new(limit: usize) -> Self {
        Self { images: BTreeMap::new(), order: VecDeque::new(), bytes: 0, limit }
    }
    pub fn max_pixels(&self) -> u64 { (self.limit / 4) as u64 }
    fn evict(&mut self) {
        if let Some(id) = self.order.pop_front() {
            if let Some(r) = self.images.remove(&id) { self.bytes -= r.argb.len() * 4; }
        }
    }
    pub fn set_limit(&mut self, limit: usize) -> bool {
        self.limit = limit;
        let old = self.images.len();
        while self.bytes > limit { self.evict(); }
        old != self.images.len()
    }
    pub fn insert(&mut self, id: u128, w: u32, h: u32, argb: Vec<u32>) -> bool {
        let cost = argb.len().saturating_mul(4);
        if id == 0 || w == 0 || h == 0 || cost > self.limit
            || argb.len() != (w as usize).saturating_mul(h as usize)
            || self.images.contains_key(&id) { return false; }
        while self.bytes.saturating_add(cost) > self.limit || self.images.len() >= 64 { self.evict(); }
        self.bytes += cost;
        self.order.push_back(id);
        self.images.insert(id, Raster { w, h, argb });
        true
    }
    /// Only replace an entire line belonging to ONE explicit image object.
    /// Mixed text, tables and arbitrary ref strings remain ordinary text.
    pub fn resolve<'a>(&'a self, b: &Block, item: &Item) -> Option<&'a Raster> {
        let Item::Line(line) = item else { return None; };
        let first = line.cells.first()?;
        let obj = b.styles.get(first.style as usize)?.obj;
        if obj == 0 || !line.cells.iter().all(|c| b.styles.get(c.style as usize).is_some_and(|s| s.obj == obj)) { return None; }
        let o = b.objs.get(obj as usize - 1)?;
        if o.ty != "inline-image" || o.refv.len() != 32 || !o.refv.bytes().all(|c| c.is_ascii_hexdigit()) { return None; }
        self.images.get(&u128::from_str_radix(&o.refv, 16).ok()?)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn quota_eviction_duplicate_and_resize() {
        let mut c = InlineCache::new(8);
        assert!(c.insert(1, 1, 1, alloc::vec![1]));
        assert!(!c.insert(1, 1, 1, alloc::vec![2]));
        assert!(c.insert(2, 1, 1, alloc::vec![2]));
        assert!(c.insert(3, 1, 1, alloc::vec![3]));
        assert!(!c.images.contains_key(&1));
        assert_eq!(c.bytes, 8);
        assert!(c.set_limit(0));
        assert_eq!(c.bytes, 0);
        assert!(!c.insert(4, 1, 1, alloc::vec![4]));
    }
    #[test]
    fn invalid_and_zero_ids_never_publish() {
        let mut c = InlineCache::new(100);
        assert!(!c.insert(0, 1, 1, alloc::vec![1]));
        assert!(!c.insert(1, 2, 2, alloc::vec![1]));
        assert!(!c.insert(1, 0, 1, alloc::vec![]));
        assert_eq!(c.bytes, 0);
    }
    #[test]
    fn only_a_whole_matching_caption_resolves_pixels() {
        use crate::transcript::{BlockKind, Line, Obj, Style, TCell};
        let mut cache = InlineCache::new(4);
        assert!(cache.insert(1, 1, 1, alloc::vec![0xffff0000]));
        let mut b = Block {
            id: 1, kind: BlockKind::Output, continuation: false, exit: None,
            cmd: None, items: alloc::vec![], cost: 0, annotated_own: true,
            styles: alloc::vec![
                Style { fg: 0, bg: 0, attrs: 0, em: 0, obj: 1, hdr: 0 },
                Style { fg: 0, bg: 0, attrs: 0, em: 0, obj: 0, hdr: 0 },
            ],
            objs: alloc::vec![Obj { ty: "inline-image".into(), refv: "00000000000000000000000000000001".into() }],
        };
        let mut caption = Item::Line(Line::plain(alloc::vec![TCell { ch: 'x', style: 0 }]));
        assert_eq!(cache.resolve(&b, &caption).unwrap().argb[0], 0xffff0000);
        b.objs[0].ty = "path".into();
        assert!(cache.resolve(&b, &caption).is_none());
        b.objs[0].ty = "inline-image".into();
        b.objs[0].refv = "1".into();
        assert!(cache.resolve(&b, &caption).is_none());
        b.objs[0].refv = "00000000000000000000000000000001".into();
        if let Item::Line(ref mut line) = caption {
            line.cells.push(TCell { ch: 'y', style: 1 });
        }
        assert!(cache.resolve(&b, &caption).is_none(), "mixed text must remain text");
        if let Item::Line(ref mut line) = caption { line.cells.pop(); }
        cache.set_limit(0);
        assert!(cache.resolve(&b, &caption).is_none(), "eviction restores the textual caption");
    }

}
