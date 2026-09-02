// halcyon (the binary) -- the syscalling half of the session tool (H-4a-2
// save; H-4b-3b restore).
//
// `halcyon layout save <name>` reads the live compositor tree from
// /dev/tapestry (the `layout` dump + each leaf's `pane/<id>/tag` +
// `pane/<id>/owner`), folds it into a libhalcyon::layout tree -- marking every
// tile that is not this session's as `env` -- serializes it to
// `halcyon-layout v1`, and writes it durably into the SESSION tier
// ($HOME/lib/halcyon/layouts/<name>): the user's own namespace, run as the
// user (HALCYON.md 13.7, the D decision: no CAP, no server verb).
//
// `halcyon layout restore <name>` reads the layout (session tier, then the
// device tier), keeps the session's part (the env tiles are the console's
// and stay where the environment put them), and grows that subtree beside
// the console on its OWN /srv/tapestry session -- a `Session(principal)` peer
// (HALCYON.md 13.6): a throwaway placeholder surface gives it a leaf it may
// split (the console's is not its tile), the libhalcyon::skeleton plan turns
// the tree into `split`/`mode` verbs whose every result is checked against
// the live dump, the placeholder is destroyed, and each leaf with a tag is
// claimed (`pane/<id>/claim`), named (`pane/<id>/tag`), and spawned as the
// user with the claim token in the child's /env (`TAPESTRY_CLAIM`) so its
// libtapestry lands it in that leaf without the program knowing (13.7's
// opaque cookie). The tool then waits for the programs to host and replays
// the saved focus.
//
// The restore drives /srv/tapestry directly, never the shared /dev/tapestry
// mount: the mount's peer is the mounter (joey), so a mutation through it
// would be judged as joey's, not this session's. Reads are ungated, so the
// save side may use either.
//
// All decision logic (dispatch, name validation, path building, the restore
// plan) lives in pure, host-tested libs; this file is I/O only.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use halcyon::{
    argv_of, device_layout_path, parse_cmd, prog_candidates, session_dir_chain,
    session_layout_path, Cmd, CmdError,
};
use libhalcyon::layout::{self, LayoutMode};
use libhalcyon::skeleton::{self, Op};
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{self, Write};
use libthyla_rs::process::Command;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{
    env, eprintln, identity, println, t_close, t_fsync, t_open, t_read, t_write, T_OREAD, T_ORDWR,
    T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};

const TAPESTRY_LAYOUT: &str = "/dev/tapestry/layout";
const TAPESTRY_SRV: &str = "/srv/tapestry";
/// The compositor tree dump is a few KB; cap generously against a runaway file.
const LAYOUT_READ_CAP: usize = 128 * 1024;
/// A tag file is one command line plus a trailing '\n'.
const TAG_READ_CAP: usize = layout::MAX_TAG_LEN + 8;
/// The claim token's /env name (libtapestry reads it back on the child's
/// first open) and its path.
const CLAIM_ENV_PATH: &str = "/env/TAPESTRY_CLAIM";
/// The placeholder surface: the smallest weave the compositor takes; it is
/// never presented and lives only while the skeleton is built.
const PLACEHOLDER_W: u32 = 16;
const PLACEHOLDER_H: u32 = 16;
/// A layout verb refused E_AGAIN (the compositor's per-pass mutation budget)
/// is retried after a nap; the whole retry window is ~2 s.
const VERB_RETRIES: u32 = 400;
const VERB_NAP_MS: u64 = 5;
/// How long the tool waits for every spawned program to host into its leaf.
const LAND_TIMEOUT_MS: u64 = 10_000;
const LAND_POLL_MS: u64 = 50;
/// EAGAIN as the syscall returns it (the kernel maps the Rlerror ecode).
const E_AGAIN: i64 = -11;

const USAGE: &str = "\
usage: halcyon layout save <name>
  Save the current Halcyon pane layout to $HOME/lib/halcyon/layouts/<name>.
  <name> is one path component: letters/digits/._- , no leading dot.

  halcyon layout restore <name>
  Rebuild the saved layout beside the console: grow its panes, then spawn
  each pane's command line as you, placed into its pane. Reads the session
  tier first, then /lib/halcyon/layouts/<name>.

  halcyon --help
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}

fn run() -> i64 {
    let args = env::args();
    let mut toks: Vec<&str> = Vec::new();
    for i in 1..args.len() {
        match args.get_str(i) {
            Some(s) => toks.push(s),
            None => {
                eprintln!("halcyon: argument {} is not UTF-8", i);
                return 2;
            }
        }
    }
    match parse_cmd(&toks) {
        Ok(Cmd::Help) => {
            io::out(USAGE.as_bytes());
            0
        }
        Ok(Cmd::LayoutSave { name }) => layout_save(name),
        Ok(Cmd::LayoutRestore { name }) => layout_restore(name),
        Err(e) => {
            report_cmd_error(e);
            2
        }
    }
}

fn report_cmd_error(e: CmdError) {
    match e {
        CmdError::UnknownCommand => eprintln!("halcyon: unknown command (try `halcyon --help`)"),
        CmdError::BadLayoutVerb => eprintln!("halcyon: layout: expected `save` or `restore`"),
        CmdError::MissingName => eprintln!("halcyon: layout: missing <name>"),
        CmdError::ExtraOperand => eprintln!("halcyon: layout: too many operands"),
        CmdError::BadName => {
            eprintln!("halcyon: invalid layout name (letters/digits/._- , no leading dot)")
        }
    }
}

/// $HOME, or None with the diagnostic already printed.
fn home_dir() -> Option<String> {
    match env::var("HOME") {
        Some(h) => {
            let h = h.trim();
            if h.is_empty() {
                eprintln!("halcyon: $HOME is empty");
                return None;
            }
            Some(String::from(h))
        }
        None => {
            eprintln!("halcyon: $HOME is unset -- run me from a logged-in session");
            None
        }
    }
}

// =============================================================================
// save
// =============================================================================

fn layout_save(name: &str) -> i64 {
    let home = match home_dir() {
        Some(h) => h,
        None => return 1,
    };

    // The compositor tree dump. The offset-0 read snaps a consistent tree
    // (server-side), so a mutation cannot straddle the read.
    let render = match read_capped(TAPESTRY_LAYOUT, LAYOUT_READ_CAP) {
        Ok(bytes) => match String::from_utf8(bytes) {
            Ok(s) => s,
            Err(_) => {
                eprintln!("halcyon: {}: not valid UTF-8", TAPESTRY_LAYOUT);
                return 1;
            }
        },
        Err(e) => {
            eprintln!("halcyon: {}: {}", TAPESTRY_LAYOUT, e);
            return 1;
        }
    };

    // Fold it into a layout tree: each leaf's tag from pane/<id>/tag, and
    // its env marker from pane/<id>/owner against this process's principal.
    let me = identity::uid();
    let tree = match layout::from_render_text(&render, |id| (read_tag(id), is_env_tile(id, me))) {
        Ok(t) => t,
        Err(_) => {
            eprintln!("halcyon: could not parse the compositor layout");
            return 1;
        }
    };
    let text = layout::serialize(&tree);

    // Durable write into the session tier ($HOME/lib/halcyon/layouts/<name>).
    if !mkdir_p(&home) {
        eprintln!("halcyon: could not create {}/lib/halcyon/layouts", home);
        return 1;
    }
    let path = session_layout_path(&home, name);
    if !durable_write(&path, text.as_bytes()) {
        eprintln!("halcyon: could not write {}", path);
        return 1;
    }
    0
}

/// Resolve a leaf pane's tag (its command line) from `pane/<id>/tag`.
/// Fail-soft: any read/UTF-8 failure yields an empty tag, so a save never
/// aborts on one unreadable leaf -- it becomes an empty placeholder.
fn read_tag(id: u32) -> String {
    let path = format!("/dev/tapestry/pane/{}/tag", id);
    match read_capped(&path, TAG_READ_CAP) {
        // The tag file appends exactly one '\n'; strip only the trailing
        // newline(s), never internal/leading bytes (a tag is a command line
        // whose spaces are meaningful).
        Ok(bytes) => match String::from_utf8(bytes) {
            Ok(s) => String::from(s.trim_end_matches(['\n', '\r'])),
            Err(_) => String::new(),
        },
        Err(_) => String::new(),
    }
}

/// Is leaf `id`'s tile the ENVIRONMENT's -- one a session restore must not
/// respawn? It is env iff its `pane/<id>/owner` names a real OTHER principal
/// (the console = SYSTEM, another user). Owner 0 is nobody's (an empty
/// placeholder the session may rebuild empty) -> NOT env; owner == me is the
/// session's own tile -> NOT env. Fail-CLOSED: an unreadable owner reads as
/// env, so a restore never respawns a tile it could not classify.
fn is_env_tile(id: u32, me: u32) -> bool {
    let path = format!("/dev/tapestry/pane/{}/owner", id);
    match read_capped(&path, 32) {
        Ok(bytes) => match core::str::from_utf8(&bytes)
            .ok()
            .and_then(|s| s.trim().parse::<u32>().ok())
        {
            Some(0) => false,
            Some(owner) => owner != me,
            None => true,
        },
        Err(_) => true,
    }
}

fn read_capped(path: &str, cap: usize) -> Result<Vec<u8>> {
    let mut f = File::open(path)?;
    io::slurp_capped(&mut f, cap)
}

/// mkdir -p `<home>/lib/halcyon/layouts`, ignoring already-existing components
/// (the kernel create is exclusive; a re-save must not fail on the second run).
fn mkdir_p(home: &str) -> bool {
    for dir in session_dir_chain(home) {
        match fs::create_dir(&dir) {
            Ok(()) | Err(Error::Exists) => {}
            Err(_) => return false,
        }
    }
    true
}

/// The aurora-config durability discipline (gfx-status cfg-2a): write-tmp,
/// content fsync, atomic rename, then a STRICT metadata fsync on the SAME
/// OWRITE fd -- the fid follows the file across the rename, and a fresh OREAD
/// reopen would fail SYS_FSYNC's RIGHT_WRITE gate. A crash before the
/// post-rename barrier rolls back to the old file, never a torn one.
fn durable_write(path: &str, bytes: &[u8]) -> bool {
    let mut tmp = String::from(path);
    tmp.push_str(".tmp");
    let mut f = match File::create(&tmp) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(bytes).is_err() {
        return false;
    }
    let _ = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
    if fs::rename(&tmp, path).is_err() {
        return false;
    }
    let ok = unsafe { t_fsync(f.as_raw_fd() as i64, 0) == 0 };
    drop(f);
    ok
}

// =============================================================================
// restore
// =============================================================================

/// The tool's own compositor session: one attach to /srv/tapestry, every
/// file walked from it, so each write is judged as THIS process's (the
/// Session(principal) actor) and the claims it mints are its own.
struct Tap {
    root: i64,
}

impl Tap {
    fn open() -> Option<Tap> {
        let root = unsafe {
            t_open(
                T_WALK_OPEN_FROM_ROOT,
                TAPESTRY_SRV.as_ptr(),
                TAPESTRY_SRV.len(),
                T_OREAD,
            )
        };
        if root < 0 {
            return None;
        }
        Some(Tap { root })
    }

    /// Open `rel` under the session root with `omode`; the raw fd or the
    /// negated errno.
    fn open_rel(&self, rel: &str, omode: u32) -> i64 {
        unsafe { t_open(self.root, rel.as_ptr(), rel.len(), omode) }
    }

    /// Read `rel` to EOF (capped).
    fn read(&self, rel: &str) -> Option<String> {
        let fd = self.open_rel(rel, T_OREAD);
        if fd < 0 {
            return None;
        }
        let mut buf: Vec<u8> = Vec::new();
        let mut chunk = [0u8; 4096];
        let mut ok = true;
        while buf.len() < LAYOUT_READ_CAP {
            let n = unsafe { t_read(fd, chunk.as_mut_ptr(), chunk.len()) };
            if n < 0 {
                ok = false;
                break;
            }
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n as usize]);
        }
        unsafe { t_close(fd) };
        if !ok {
            return None;
        }
        String::from_utf8(buf).ok()
    }

    /// One write on `rel`; the raw rc (the negated errno on refusal).
    fn write(&self, rel: &str, data: &str) -> i64 {
        let fd = self.open_rel(rel, T_OWRITE);
        if fd < 0 {
            return fd;
        }
        let rc = unsafe { t_write(fd, data.as_ptr(), data.len()) };
        unsafe { t_close(fd) };
        rc
    }

    /// A layout verb on the `layout` file, retried through the compositor's
    /// per-pass mutation budget (E_AGAIN: each verb is a full repaint, and a
    /// skeleton is many verbs in a burst).
    fn verb(&self, cmd: &str) -> i64 {
        let mut rc = E_AGAIN;
        for _ in 0..VERB_RETRIES {
            rc = self.write("layout", cmd);
            if rc != E_AGAIN {
                break;
            }
            let _ = sleep(Duration::from_millis(VERB_NAP_MS));
        }
        rc
    }

    fn dump(&self) -> Option<Dump> {
        self.read("layout").and_then(|t| parse_dump(&t))
    }
}

impl Drop for Tap {
    fn drop(&mut self) {
        unsafe { t_close(self.root) };
    }
}

/// One row of the `layout` dump, with its pane id kept (the libhalcyon fold
/// drops ids; the executor lives on them).
#[derive(Clone, Copy, PartialEq, Eq)]
enum DumpRow {
    Leaf { id: u32, surface: Option<u32> },
    Cont { id: u32, mode: LayoutMode },
}

struct Dump {
    /// (depth, row), pre-order as printed.
    rows: Vec<(usize, DumpRow)>,
}

impl Dump {
    fn leaf_ids(&self) -> Vec<u32> {
        self.rows
            .iter()
            .filter_map(|(_, r)| match r {
                DumpRow::Leaf { id, .. } => Some(*id),
                _ => None,
            })
            .collect()
    }

    fn cont_ids(&self) -> Vec<u32> {
        self.rows
            .iter()
            .filter_map(|(_, r)| match r {
                DumpRow::Cont { id, .. } => Some(*id),
                _ => None,
            })
            .collect()
    }

    /// The leaf hosting surface `n`.
    fn leaf_hosting(&self, n: u32) -> Option<u32> {
        self.rows.iter().find_map(|(_, r)| match r {
            DumpRow::Leaf {
                id,
                surface: Some(s),
            } if *s == n => Some(*id),
            _ => None,
        })
    }

    /// The mode of `id`'s parent container (None: `id` is the root, or
    /// unknown).
    fn parent_mode_of(&self, id: u32) -> Option<LayoutMode> {
        let at = self.rows.iter().position(|(_, r)| match r {
            DumpRow::Leaf { id: i, .. } | DumpRow::Cont { id: i, .. } => *i == id,
        })?;
        let depth = self.rows[at].0;
        if depth == 0 {
            return None;
        }
        // The parent is the nearest earlier row one level shallower.
        self.rows[..at].iter().rev().find_map(|(d, r)| match r {
            DumpRow::Cont { mode, .. } if *d + 1 == depth => Some(*mode),
            _ => None,
        })
    }
}

/// Parse the `layout` dump: the `epoch .. focused ..` header, then per row
/// `<id>[*] leaf empty|surface=<n> [..]` or `<id>[*] <mode> n=.. active=.. [..]`,
/// two spaces of indent per depth. None on any malformed row (the executor
/// aborts rather than guess at the tree).
fn parse_dump(text: &str) -> Option<Dump> {
    let mut lines = text.split('\n');
    if !lines.next()?.starts_with("epoch ") {
        return None;
    }
    let mut rows: Vec<(usize, DumpRow)> = Vec::new();
    for raw in lines {
        let line = raw.trim_end_matches('\r');
        if line.is_empty() {
            continue;
        }
        let spaces = line.len() - line.trim_start_matches(' ').len();
        if !spaces.is_multiple_of(2) {
            return None;
        }
        let depth = spaces / 2;
        let mut it = line[spaces..].split(' ');
        let id: u32 = it
            .next()
            .map(|t| t.strip_suffix('*').unwrap_or(t))
            .and_then(|t| t.parse().ok())?;
        let row = match it.next()? {
            "leaf" => {
                let surface = match it.next()? {
                    "empty" => None,
                    tok => Some(tok.strip_prefix("surface=")?.parse::<u32>().ok()?),
                };
                DumpRow::Leaf { id, surface }
            }
            tok => DumpRow::Cont {
                id,
                mode: LayoutMode::parse(tok)?,
            },
        };
        rows.push((depth, row));
    }
    Some(Dump { rows })
}

/// The ids in `after` that are not in `before`.
fn new_ids(before: &[u32], after: &[u32]) -> Vec<u32> {
    after
        .iter()
        .copied()
        .filter(|id| !before.contains(id))
        .collect()
}

/// The saved layout's text: the session tier, then the device tier.
fn read_layout_file(home: &str, name: &str) -> Option<(String, String)> {
    for path in [session_layout_path(home, name), device_layout_path(name)] {
        match read_capped(&path, LAYOUT_READ_CAP) {
            Ok(bytes) => match String::from_utf8(bytes) {
                Ok(s) => return Some((s, path)),
                Err(_) => {
                    eprintln!("halcyon: {}: not valid UTF-8", path);
                    return None;
                }
            },
            Err(Error::NotFound) => continue,
            Err(e) => {
                eprintln!("halcyon: {}: {}", path, e);
                return None;
            }
        }
    }
    eprintln!("halcyon: no layout named `{}` (session or device tier)", name);
    None
}

/// The placeholder: a surface minted on the tool's session and created at
/// focus, so the compositor's own placement (host(): split the focused
/// leaf) hands the tool a leaf it owns. Destroyed once the skeleton stands.
struct Placeholder {
    ctl: i64,
    surface: u32,
}

impl Placeholder {
    fn create(tap: &Tap) -> Option<Placeholder> {
        let ctl = tap.open_rel("surface/new", T_ORDWR);
        if ctl < 0 {
            eprintln!("halcyon: surface/new: {}", ctl);
            return None;
        }
        let mut idbuf = [0u8; 16];
        let n = unsafe { t_read(ctl, idbuf.as_mut_ptr(), idbuf.len()) };
        let surface = if n > 0 {
            core::str::from_utf8(&idbuf[..n as usize])
                .ok()
                .and_then(|s| s.trim().parse::<u32>().ok())
        } else {
            None
        };
        let surface = match surface {
            Some(s) => s,
            None => {
                eprintln!("halcyon: surface/new: no surface id");
                unsafe { t_close(ctl) };
                return None;
            }
        };
        let cmd = format!("create {} {}", PLACEHOLDER_W, PLACEHOLDER_H);
        let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
        if rc < 0 {
            eprintln!("halcyon: placeholder create refused ({})", rc);
            unsafe { t_close(ctl) };
            return None;
        }
        Some(Placeholder { ctl, surface })
    }
}

impl Drop for Placeholder {
    fn drop(&mut self) {
        let _ = unsafe { t_write(self.ctl, b"destroy".as_ptr(), 7) };
        unsafe { t_close(self.ctl) };
    }
}

fn layout_restore(name: &str) -> i64 {
    let home = match home_dir() {
        Some(h) => h,
        None => return 1,
    };
    let (text, path) = match read_layout_file(&home, name) {
        Some(t) => t,
        None => return 1,
    };
    let saved = match layout::parse(&text) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("halcyon: {}: malformed layout ({:?})", path, e);
            return 1;
        }
    };
    // The session's part: env tiles (the console's, another user's) stay
    // where the environment put them -- they are not this tool's to grow.
    let tree = match layout::prune_env(&saved) {
        Some(t) => t,
        None => {
            println!(
                "halcyon: {}: every tile is the environment's; nothing to restore",
                name
            );
            return 0;
        }
    };

    let tap = match Tap::open() {
        Some(t) => t,
        None => {
            eprintln!("halcyon: {} unreachable -- is the compositor up?", TAPESTRY_SRV);
            return 1;
        }
    };

    // 1. The placeholder lands beside the focused tile (the console): its
    //    leaf is the one tile this session may split.
    let ph = match Placeholder::create(&tap) {
        Some(p) => p,
        None => return 1,
    };
    let dump = match tap.dump() {
        Some(d) => d,
        None => {
            eprintln!("halcyon: could not read the compositor layout");
            return 1;
        }
    };
    let l1 = match dump.leaf_hosting(ph.surface) {
        Some(id) => id,
        None => {
            eprintln!("halcyon: the placeholder surface was not hosted");
            return 1;
        }
    };
    let outer = dump.parent_mode_of(l1);

    // 2. The anchor: split the placeholder's leaf once; the new empty leaf is
    //    where the tree grows (and the placeholder's later removal leaves
    //    exactly the tree -- skeleton::anchor_split picks the direction).
    let m0 = skeleton::anchor_split(&tree, outer);
    let rc = tap.verb(&format!("split {} {}", l1, m0.verb()));
    if rc < 0 {
        eprintln!("halcyon: split of the placeholder's pane refused ({})", rc);
        return 1;
    }
    let mut prev = match tap.dump() {
        Some(d) => d,
        None => {
            eprintln!("halcyon: could not read the compositor layout");
            return 1;
        }
    };
    let anchor = match new_ids(&dump.leaf_ids(), &prev.leaf_ids()).as_slice() {
        [one] => *one,
        other => {
            eprintln!(
                "halcyon: layout build diverged at the anchor ({} new leaves)",
                other.len()
            );
            return 1;
        }
    };

    // 3. The skeleton, verb by verb, each result checked against the dump.
    let plan = skeleton::plan(&tree, Some(m0.mode()));
    let mut leaf_ids: Vec<Option<u32>> = alloc::vec![None; plan.leaf_count];
    let mut cont_ids: Vec<Option<u32>> = alloc::vec![None; plan.cont_count];
    leaf_ids[0] = Some(anchor);
    for op in &plan.ops {
        match op {
            Op::Split {
                at,
                dir,
                new_leaf,
                nests,
            } => {
                let id = match leaf_ids[*at] {
                    Some(id) => id,
                    None => {
                        eprintln!("halcyon: plan names an unbuilt leaf");
                        return 1;
                    }
                };
                let rc = tap.verb(&format!("split {} {}", id, dir.verb()));
                if rc < 0 {
                    eprintln!("halcyon: split {} {} refused ({})", id, dir.verb(), rc);
                    return 1;
                }
                let now = match tap.dump() {
                    Some(d) => d,
                    None => {
                        eprintln!("halcyon: could not read the compositor layout");
                        return 1;
                    }
                };
                let nl = new_ids(&prev.leaf_ids(), &now.leaf_ids());
                let nc = new_ids(&prev.cont_ids(), &now.cont_ids());
                let expect_cont = usize::from(nests.is_some());
                if nl.len() != 1 || nc.len() != expect_cont {
                    eprintln!(
                        "halcyon: layout build diverged at pane {} ({} new leaves, {} new containers, expected 1 and {})",
                        id,
                        nl.len(),
                        nc.len(),
                        expect_cont
                    );
                    return 1;
                }
                leaf_ids[*new_leaf] = Some(nl[0]);
                if let Some(c) = nests {
                    cont_ids[*c] = Some(nc[0]);
                }
                prev = now;
            }
            Op::SetMode { cont, mode } => {
                let id = match cont_ids[*cont] {
                    Some(id) => id,
                    None => {
                        eprintln!("halcyon: plan names an unbuilt container");
                        return 1;
                    }
                };
                let rc = tap.verb(&format!("mode {} {}", id, mode.name()));
                if rc < 0 {
                    eprintln!("halcyon: mode {} {} refused ({})", id, mode.name(), rc);
                    return 1;
                }
            }
        }
    }

    // 4. The placeholder goes: the skeleton stands on its own, every leaf
    //    empty and this session's (stamped at each split).
    drop(ph);

    // 5. Placement: per tagged leaf, mint its claim, name it, seed the token
    //    into our /env (each spawn deep-copies the env as it stands), spawn.
    let mut spawned: Vec<(u32, i32, String)> = Vec::new();
    let mut failed: usize = 0;
    for pl in &plan.leaves {
        if pl.tag.trim().is_empty() {
            continue;
        }
        let id = match leaf_ids[pl.leaf] {
            Some(id) => id,
            None => {
                failed += 1;
                continue;
            }
        };
        let token = match tap.read(&format!("pane/{}/claim", id)) {
            Some(s) if s.trim().len() == 32 => match u128::from_str_radix(s.trim(), 16) {
                Ok(t) => t,
                Err(_) => {
                    eprintln!("halcyon: pane {}: claim mint unreadable", id);
                    failed += 1;
                    continue;
                }
            },
            _ => {
                eprintln!("halcyon: pane {}: claim mint refused", id);
                failed += 1;
                continue;
            }
        };
        if tap.write(&format!("pane/{}/tag", id), &pl.tag) < 0 {
            eprintln!("halcyon: pane {}: tag write refused", id);
        }
        if !seed_claim(token) {
            eprintln!("halcyon: pane {}: could not seed {}", id, CLAIM_ENV_PATH);
            failed += 1;
            continue;
        }
        let argv = argv_of(&pl.tag);
        let mut cmd = Command::new(resolve_prog(argv[0]));
        cmd.args(argv[1..].iter().copied());
        match cmd.spawn() {
            Ok(child) => {
                println!("halcyon: pane {}: {} (pid {})", id, pl.tag, child.pid());
                spawned.push((id, child.pid(), pl.tag.clone()));
            }
            Err(e) => {
                eprintln!("halcyon: pane {}: cannot spawn `{}`: {}", id, pl.tag, e);
                failed += 1;
            }
        }
    }
    let _ = fs::remove_file(CLAIM_ENV_PATH);

    // 6. Wait for the programs to host (each claim is consumed by its
    //    child's first open), then replay the saved focus over the tiles
    //    that arrived -- a leaf that stayed empty is nobody's to focus.
    let landed = wait_landed(&tap, &spawned);
    let mut last_focus: Option<u32> = None;
    for f in &plan.focus {
        let id = match leaf_ids[*f] {
            Some(id) => id,
            None => continue,
        };
        let hosted = spawned
            .iter()
            .zip(landed.iter())
            .any(|((sid, _, _), l)| *sid == id && *l);
        if hosted && tap.verb(&format!("focus {}", id)) >= 0 {
            last_focus = Some(id);
        }
    }
    if let Some(id) = last_focus {
        println!("halcyon: focus -> pane {}", id);
    }
    let n_landed = landed.iter().filter(|l| **l).count();
    for ((id, pid, tag), l) in spawned.iter().zip(landed.iter()) {
        if !*l {
            eprintln!(
                "halcyon: pane {}: {} (pid {}) did not host a surface within {} s",
                id,
                tag,
                pid,
                LAND_TIMEOUT_MS / 1000
            );
        }
    }
    println!(
        "halcyon: restored {} of {} program(s)",
        n_landed,
        spawned.len() + failed
    );
    if n_landed == spawned.len() && failed == 0 {
        0
    } else {
        1
    }
}

/// Resolve a bare program name to a path the kernel can spawn: probe the
/// shell's PROG_DIRS (an O-read existence check) and spawn the first hit; a
/// slashed name is used verbatim. On no hit, `/bin/<name>` (the first
/// candidate) so the spawn fails with a clean, shell-identical error.
fn resolve_prog(argv0: &str) -> String {
    let cands = prog_candidates(argv0);
    for c in &cands {
        if File::open(c).is_ok() {
            return c.clone();
        }
    }
    cands
        .into_iter()
        .next()
        .unwrap_or_else(|| String::from(argv0))
}

/// Write the claim token into this process's /env (`TAPESTRY_CLAIM`), where
/// the next spawn's deep copy carries it to the child (login's
/// seed_session_env idiom; a re-create truncates the previous token).
fn seed_claim(token: u128) -> bool {
    let val = format!("{:032x}", token);
    match File::create(CLAIM_ENV_PATH) {
        Ok(mut f) => f.write_all(val.as_bytes()).is_ok(),
        Err(_) => false,
    }
}

/// Poll each spawned leaf's `pane/<id>/surface` until it hosts, within the
/// landing budget. Returns one flag per entry of `spawned`.
fn wait_landed(tap: &Tap, spawned: &[(u32, i32, String)]) -> Vec<bool> {
    let mut landed: Vec<bool> = alloc::vec![false; spawned.len()];
    let mut waited: u64 = 0;
    loop {
        for (i, (id, _, _)) in spawned.iter().enumerate() {
            if landed[i] {
                continue;
            }
            if let Some(s) = tap.read(&format!("pane/{}/surface", id)) {
                if s.trim() != "none" {
                    landed[i] = true;
                }
            }
        }
        if landed.iter().all(|l| *l) || waited >= LAND_TIMEOUT_MS {
            return landed;
        }
        let _ = sleep(Duration::from_millis(LAND_POLL_MS));
        waited += LAND_POLL_MS;
    }
}
