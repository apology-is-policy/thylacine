//! Boxed "card" output for the presentation tools: a titled `boxd` box around a
//! set of rows. Each row carries its PLAIN text (which sizes the box) and its
//! COLORED text (which prints), per boxd's compute-on-plain / color-at-emit
//! rule. Backend-gated -- it writes to stdout. The net tools use it for netstat's
//! connection table and the nslookup / ping / dial / bench result cards, so every
//! framed result shares one look with `ls -l`.

use crate::{boxd, color, palette};
use alloc::format;
use alloc::string::String;
use libthyla_rs::println;
use libthyla_rs::time::Duration;

/// One card row. `plain` and `colored` are the same text; `colored` adds the
/// zero-width SGR spans. `plain` sizes the box; `colored` is what prints.
pub struct Row {
    pub plain: String,
    pub colored: String,
}

impl Row {
    pub fn new(plain: String, colored: String) -> Row {
        Row { plain, colored }
    }
}

/// Emit a DIM-bordered box titled `title` (with an optional top-right `right`
/// label) around `rows`. Borders match `ls -l`'s furniture; the row content
/// carries its own color.
pub fn card(title: &str, right: &str, rows: &[Row], on: bool) {
    let content_w = rows.iter().map(|r| r.plain.chars().count()).max().unwrap_or(0);
    let total = boxd::fit(content_w, title, right, "");
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);

    println!("{}{}{}", dim, boxd::top(total, title, right), rst);
    for r in rows {
        let pad = boxd::pad(total, r.plain.chars().count());
        let spaces: String = core::iter::repeat_n(' ', pad).collect();
        // │ {colored}{pad} │  -- the borders DIM, the content's own color intact.
        println!("{}{}{} {}{} {}{}{}", dim, boxd::V, rst, r.colored, spaces, dim, boxd::V, rst);
    }
    println!("{}{}{}", dim, boxd::bottom(total, ""), rst);
}

/// Emit a two-row throughput card: `{label} {bytes} bytes in {t} s` over
/// `{rate} MB/s`. Rate is integer-only (bytes * 1e6 / us, then / 2^20, two
/// fractional digits). Shared by the net benches (`nettest`, `weft-bench`).
pub fn rate_card(title: &str, label: &str, bytes: u64, dt: Duration, on: bool) {
    let us = dt.as_micros().max(1);
    let bps = bytes as u128 * 1_000_000 / us; // bytes/second
    let mbps_x100 = bps * 100 / (1024 * 1024); // MB/s * 100
    let secs = us / 1_000_000;
    let ms = (us % 1_000_000) / 1000;

    let gold = color::col(palette::GOLD, on);
    let grn = color::col(palette::GREEN, on);
    let rst = color::reset(on);

    let rows = [
        Row::new(
            format!("{} {} bytes in {}.{:03} s", label, bytes, secs, ms),
            format!("{} {}{}{} bytes in {}.{:03} s", label, gold, bytes, rst, secs, ms),
        ),
        Row::new(
            format!("{}.{:02} MB/s", mbps_x100 / 100, mbps_x100 % 100),
            format!("{}{}.{:02} MB/s{}", grn, mbps_x100 / 100, mbps_x100 % 100, rst),
        ),
    ];
    card(title, "", &rows, on);
}
