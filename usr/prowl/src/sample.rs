// prowl::sample -- the pure telemetry layer (terminal-free).
//
// Reads the kernel /ctl/procs + /ctl/sched text (the prowl-1 substrate) and
// derives the process table + per-proc %CPU the htop way: diff cumulative cpu_ns
// across two polls, divide by the monotonic wall delta. 100% == one core fully
// busy; a proc spanning two cores reads 200%. No float -- tenths-of-a-percent
// integer math (delta_ns * 1000 / elapsed_ns). A buggy sampler mis-renders
// prowl's own screen and nothing else; the kernel gates every read.

use alloc::string::String;
use alloc::vec::Vec;

/// One parsed /ctl/procs data line. The kernel format (kernel/devctl.c
/// format_procs) is 7 whitespace-separated columns after a header line:
///   PID  NAME  STATE  THREADS  PAGES  CHILDREN  CPU_NS
/// NAME is a binary basename (no embedded spaces, <=31 bytes); STATE is one
/// token (ALIVE/ZOMBIE/INVALID). So a 7-token whitespace split is exact.
/// (CHILDREN is parsed for column alignment but not surfaced at prowl-2.)
pub struct ProcRow {
    pub pid: i64,
    pub name: String,
    pub state: State,
    pub threads: u32,
    pub pages: u32,
    pub cpu_ns: u64,
    /// Per-poll CPU usage in tenths of a percent (100% == one core). Filled by
    /// `Sampler::update` from the cpu_ns delta; 0 on the first sighting of a pid.
    pub cpu_pct_x10: u64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum State {
    Alive,
    Zombie,
    Other,
}

impl State {
    pub fn as_str(self) -> &'static str {
        match self {
            State::Alive => "ALIVE",
            State::Zombie => "ZOMBIE",
            State::Other => "?",
        }
    }
    fn parse(tok: &str) -> State {
        match tok {
            "ALIVE" => State::Alive,
            "ZOMBIE" => State::Zombie,
            _ => State::Other,
        }
    }
}

/// Parse the `/ctl/procs` text into rows. The header line and any malformed
/// (non-7-token, unparseable-pid) line are skipped -- a truncated last line (the
/// kernel's DEVCTL_READ_BUF overflow early-return) drops cleanly rather than
/// producing a bogus row.
pub fn parse_procs(text: &str) -> Vec<ProcRow> {
    let mut rows = Vec::new();
    for line in text.lines() {
        let mut it = line.split_whitespace();
        let (pid, name, state, threads, pages, _children, cpu_ns) = match (
            it.next(), it.next(), it.next(), it.next(), it.next(), it.next(), it.next(),
        ) {
            (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f), Some(g)) => (a, b, c, d, e, f, g),
            _ => continue, // header / blank / short line
        };
        // A trailing 8th token means a space in a field (a name with a space)
        // would desync the columns -- reject the whole line rather than
        // mis-attribute it.
        if it.next().is_some() {
            continue;
        }
        let pid: i64 = match pid.parse() {
            Ok(v) => v,
            Err(_) => continue, // the header line ("PID") lands here
        };
        rows.push(ProcRow {
            pid,
            name: String::from(name),
            state: State::parse(state),
            threads: threads.parse().unwrap_or(0),
            pages: pages.parse().unwrap_or(0),
            cpu_ns: cpu_ns.parse().unwrap_or(0),
            cpu_pct_x10: 0,
        });
    }
    rows
}

/// Parse the online CPU count from `/ctl/sched`'s `cpus: N` line. Falls back to
/// 1 if the line is absent -- keeps the aggregate meter honest.
pub fn parse_ncpus(sched_text: &str) -> usize {
    field_u64(sched_text, "cpus: ").map(|c| c.max(1) as usize).unwrap_or(1)
}

/// The unsigned integer immediately following `key` in `s` (the cpubench idiom).
fn field_u64(s: &str, key: &str) -> Option<u64> {
    let pos = s.find(key)?;
    let rest = &s[pos + key.len()..];
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    if end == 0 {
        return None;
    }
    rest[..end].parse().ok()
}

/// The cross-poll %CPU deriver. Holds the previous poll's (pid -> cpu_ns) so the
/// next poll can diff. The monotonic wall delta is supplied by the caller
/// (time::Instant), keeping this layer terminal- AND clock-free.
pub struct Sampler {
    prev: Vec<(i64, u64)>, // (pid, cpu_ns) from the last poll
}

impl Sampler {
    pub fn new() -> Sampler {
        Sampler { prev: Vec::new() }
    }

    /// Fill each row's `cpu_pct_x10` from the delta against the previous poll,
    /// then record this poll as the new baseline. `elapsed_ns` is the monotonic
    /// wall time since the last `update` (0 disables the derivation -- first
    /// frame). A pid unseen last poll, or one whose cpu_ns went backward (pid
    /// reuse), reads 0% this frame and corrects on the next.
    pub fn update(&mut self, rows: &mut [ProcRow], elapsed_ns: u64) {
        if elapsed_ns > 0 {
            for r in rows.iter_mut() {
                if let Some(&(_, old)) = self.prev.iter().find(|&&(pid, _)| pid == r.pid) {
                    let delta = r.cpu_ns.saturating_sub(old);
                    // (delta / elapsed) is the fraction of one core; * 1000 ->
                    // tenths of a percent. delta <= elapsed * ncpus, so
                    // delta * 1000 stays far within u64.
                    r.cpu_pct_x10 = delta.saturating_mul(1000) / elapsed_ns;
                }
            }
        }
        self.prev.clear();
        self.prev.extend(rows.iter().map(|r| (r.pid, r.cpu_ns)));
    }

    /// Total %CPU across all rows, in tenths of a percent (approaches
    /// ncpus * 1000 when every core is busy).
    pub fn total_pct_x10(rows: &[ProcRow]) -> u64 {
        rows.iter().map(|r| r.cpu_pct_x10).sum()
    }
}

/// The process-list sort key (cycled by the `s` key; the initial one from `-s`).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Sort {
    Cpu,
    Pid,
    Mem,
    Name,
}

impl Sort {
    pub fn next(self) -> Sort {
        match self {
            Sort::Cpu => Sort::Pid,
            Sort::Pid => Sort::Mem,
            Sort::Mem => Sort::Name,
            Sort::Name => Sort::Cpu,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Sort::Cpu => "cpu",
            Sort::Pid => "pid",
            Sort::Mem => "mem",
            Sort::Name => "name",
        }
    }
    /// Sort in place. CPU + mem descending (the interesting end first); pid
    /// ascending; name lexicographic. A stable secondary key on pid keeps the
    /// list from jittering when two rows tie.
    pub fn apply(self, rows: &mut [ProcRow]) {
        match self {
            Sort::Cpu => rows.sort_by(|a, b| b.cpu_pct_x10.cmp(&a.cpu_pct_x10).then(a.pid.cmp(&b.pid))),
            Sort::Mem => rows.sort_by(|a, b| b.pages.cmp(&a.pages).then(a.pid.cmp(&b.pid))),
            Sort::Pid => rows.sort_by(|a, b| a.pid.cmp(&b.pid)),
            Sort::Name => rows.sort_by(|a, b| a.name.cmp(&b.name).then(a.pid.cmp(&b.pid))),
        }
    }
}
