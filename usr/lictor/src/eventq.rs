//! What the input ring's low-water mark can prove. A device with no room for a report
//! drops it whole and tells nobody (QEMU's virtio_input_send), so a release can go
//! missing with no other trace.

/// The deepest ring in use: QEMU's virtio-input offers 64, and one page holds it.
pub const DEPTH_MAX: u16 = 64;
const DEPTH_MIN: u16 = 16;
/// More than one report holds: a position, the buttons that changed, a wheel, a sync.
const REPORT_MAX: u16 = 8;

/// The depth to run at, given what the device offers. A split ring is a power of two.
pub fn depth(offered: u16) -> Option<u16> {
    if !offered.is_power_of_two() || offered < DEPTH_MIN {
        return None;
    }
    Some(offered.min(DEPTH_MAX))
}

/// Whether a report may have been dropped since the last recycle. Only the device
/// consumes between recycles, so what is free at the next drain is the least it had in
/// all that time.
pub fn may_have_dropped(published: u16, used: u16) -> bool {
    published.wrapping_sub(used) < REPORT_MAX
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn depth_takes_what_a_page_holds_and_refuses_what_is_no_ring() {
        assert_eq!(depth(64), Some(64));
        assert_eq!(depth(256), Some(64));
        assert_eq!(depth(32), Some(32));
        assert_eq!(depth(16), Some(16));
        assert_eq!(depth(8), None);
        assert_eq!(depth(24), None);
        assert_eq!(depth(0), None);
    }

    #[test]
    fn a_report_can_be_dropped_only_with_less_than_a_report_free() {
        assert!(!may_have_dropped(64, 0));
        assert!(!may_have_dropped(64, 56));
        assert!(may_have_dropped(64, 57));
        assert!(may_have_dropped(64, 64));
    }

    #[test]
    fn the_free_count_survives_the_index_wrapping() {
        assert!(!may_have_dropped(10, 65530));
        assert!(may_have_dropped(3, 65535));
        assert!(may_have_dropped(0, 0));
    }
}
