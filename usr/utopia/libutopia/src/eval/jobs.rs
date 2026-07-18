// libutopia::eval::jobs -- the background-job table (U-7a).
//
// === Position in the U-7 job-control arc ===
//
// U-7a is "job table + `&` background + foreground pid-demux + lazy reap"
// (UTOPIA-SHELL-DESIGN.md section 10.1 + 10.5). This module is the PURE
// half: it records the pids of `&`-launched jobs, decides when a job is
// "done", and formats the `[N]+ Done` notification line. It performs NO
// syscalls -- the actual reaping (`wait_pid_for` WNOHANG) is driven by the
// `ut` REPL's prompt-cycle reaper, which feeds reap results back via
// `mark_reaped`. Keeping the table pure makes it host-testable against
// injected `(pid, status)` pairs.
//
// The job table lives in `Env` (alongside the function table + note
// handler registry), so a function or a sourced sub-script can spawn a
// background job and it is tracked in the one place the shell's state
// lives.
//
// === The bash job model, minus process groups ===
//
// A `&`-launched PIPELINE (`a | b &`) is ONE job tracking N pids -- bash's
// model. The `[N] pid` launch announcement prints the LAST element's pid
// (bash's `$!`), and the job is "done" only when EVERY element has been
// reaped. Process-group machinery (`setpgid`, the controlling-terminal
// foreground pgid, `SIGTSTP`/`SIGCONT` stop/resume) is U-PTY territory; at
// U-7a a job is just a set of pids the shell reaps in the background.
//
// === Spec (`[N]`) assignment ===
//
// `add` assigns `max(existing specs) + 1`, or 1 when the table is empty.
// This matches bash: numbers climb while jobs coexist and reset to 1 once
// the table drains. No spec is reused while its job is live, so `%N`
// references (U-7b) are stable for a job's lifetime.

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

/// One element of a background job -- a single spawned process.
struct JobPid {
    pid: i32,
    /// The process has been reaped (its zombie reclaimed via the syscall
    /// layer's `wait_pid_for`). Until then the kernel holds the zombie.
    reaped: bool,
    /// The reaped exit status (meaningful once `reaped`).
    status: i32,
}

/// One background job: a `&`-launched pipeline of 1+ external commands.
pub struct Job {
    /// The `[N]` jobspec -- 1-based; stable for the job's lifetime.
    spec: u32,
    /// The rendered command line, for the `[N]+ Done` line + `jobs` (U-7b).
    cmd: String,
    /// Per-element reap tracking (1+ entries; a single command has one).
    pids: Vec<JobPid>,
    /// Whether the `[N]+ Done` line has been emitted (one-shot, so a job is
    /// reported exactly once).
    notified: bool,
    /// PTY-4b: the job's process group (0 = none recorded -- the console
    /// path, where jobs are never pgrp-formed). `fg`/`bg` resume via
    /// `SYS_TTY_CONT` on this group; `fg` hands it the terminal.
    pgid: u64,
    /// PTY-4b: the job is job-stopped (a `^Z` `WAIT_UNTRACED` stop-report
    /// moved it here from the foreground, or a re-stop after `fg`). A
    /// stopped job always has unreaped pids, so it can never satisfy
    /// `is_complete` -- the Done notification is structurally excluded.
    stopped: bool,
}

impl Job {
    /// The `[N]` jobspec.
    #[must_use]
    pub fn spec(&self) -> u32 {
        self.spec
    }

    /// The rendered command line.
    #[must_use]
    pub fn cmd(&self) -> &str {
        &self.cmd
    }

    /// The representative pid -- the last pipeline element (bash's `$!`).
    /// A job always has at least one pid, so this never panics for a job
    /// produced by `JobTable::add` (which rejects an empty pid list).
    #[must_use]
    pub fn last_pid(&self) -> i32 {
        self.pids[self.pids.len() - 1].pid
    }

    /// `true` while any element has not yet been reaped (the job is still
    /// live -- Running or Stopped -- for a `jobs` listing, U-7b).
    #[must_use]
    pub fn is_running(&self) -> bool {
        self.pids.iter().any(|p| !p.reaped)
    }

    /// PTY-4b: whether the job is job-stopped (`^Z`). Only meaningful while
    /// `is_running` (a fully-reaped job cannot be stopped).
    #[must_use]
    pub fn stopped(&self) -> bool {
        self.stopped
    }

    /// PTY-4b: the job's process group (0 = none recorded).
    #[must_use]
    pub fn pgid(&self) -> u64 {
        self.pgid
    }

    /// Every element reaped -- the job has fully finished.
    fn is_complete(&self) -> bool {
        self.pids.iter().all(|p| p.reaped)
    }
}

/// The shell's background-job registry.
pub struct JobTable {
    jobs: Vec<Job>,
}

impl Default for JobTable {
    fn default() -> Self {
        Self::new()
    }
}

impl JobTable {
    /// An empty table.
    #[must_use]
    pub fn new() -> Self {
        JobTable { jobs: Vec::new() }
    }

    /// Register a freshly-launched background job. `pids` are the pipeline's
    /// element pids (1+, in element order). `cmd` is the rendered command
    /// line for display. Returns the assigned `[N]` spec. An empty `pids`
    /// list is rejected (returns 0 and registers nothing) -- the caller must
    /// have spawned at least one element.
    pub fn add(&mut self, pids: Vec<i32>, cmd: String) -> u32 {
        if pids.is_empty() {
            return 0;
        }
        let spec = self.jobs.iter().map(|j| j.spec).max().map_or(1, |m| m + 1);
        self.jobs.push(Job {
            spec,
            cmd,
            pids: pids
                .into_iter()
                .map(|pid| JobPid {
                    pid,
                    reaped: false,
                    status: 0,
                })
                .collect(),
            notified: false,
            pgid: 0,
            stopped: false,
        });
        spec
    }

    /// PTY-4b: record job `spec`'s process group (set once at launch by the
    /// jc spawn path; 0 stays "none recorded" on the console path).
    pub fn set_pgid(&mut self, spec: u32, pgid: u64) {
        if let Some(j) = self.jobs.iter_mut().find(|j| j.spec == spec) {
            j.pgid = pgid;
        }
    }

    /// PTY-4b: the recorded process group of job `spec` (`None` = no such
    /// job; `Some(0)` = a job launched without pgrp formation).
    #[must_use]
    pub fn pgid_of(&self, spec: u32) -> Option<u64> {
        self.jobs.iter().find(|j| j.spec == spec).map(|j| j.pgid)
    }

    /// PTY-4b: flip job `spec`'s stopped state (a `^Z` stop-report sets it;
    /// `fg`/`bg`'s `SYS_TTY_CONT` resume clears it).
    pub fn set_stopped(&mut self, spec: u32, stopped: bool) {
        if let Some(j) = self.jobs.iter_mut().find(|j| j.spec == spec) {
            j.stopped = stopped;
        }
    }

    /// PTY-4b: whether job `spec` is job-stopped (false for an unknown spec).
    #[must_use]
    pub fn is_stopped(&self, spec: u32) -> bool {
        self.jobs
            .iter()
            .find(|j| j.spec == spec)
            .is_some_and(|j| j.stopped)
    }

    /// PTY-4b: the not-yet-reaped element pids of job `spec`, in element
    /// order (`None` = no such job). `fg` waits on exactly these -- an
    /// already-reaped element must not be re-waited (its zombie is gone; a
    /// by-pgrp wait would simply never match it, but the status bookkeeping
    /// wants the live set explicitly).
    #[must_use]
    pub fn live_pids_of(&self, spec: u32) -> Option<Vec<i32>> {
        self.jobs.iter().find(|j| j.spec == spec).map(|j| {
            j.pids
                .iter()
                .filter(|p| !p.reaped)
                .map(|p| p.pid)
                .collect()
        })
    }

    /// Record that `pid` was reaped with `status`. Idempotent: a second call
    /// for an already-reaped pid keeps the first status. A pid not in any job
    /// is ignored (e.g. a foreground child, already reaped by its by-pid
    /// `wait`). Driven by the REPL reaper's WNOHANG `wait_pid_for` ground
    /// truth.
    pub fn mark_reaped(&mut self, pid: i32, status: i32) {
        for job in &mut self.jobs {
            for p in &mut job.pids {
                if p.pid == pid && !p.reaped {
                    p.reaped = true;
                    p.status = status;
                    return;
                }
            }
        }
    }

    /// Every not-yet-reaped pid across all jobs, in registration order. The
    /// REPL reaper WNOHANG-polls each of these and feeds the results back via
    /// `mark_reaped`.
    #[must_use]
    pub fn live_pids(&self) -> Vec<i32> {
        let mut out = Vec::new();
        for job in &self.jobs {
            for p in &job.pids {
                if !p.reaped {
                    out.push(p.pid);
                }
            }
        }
        out
    }

    /// Drain the `[N]+ Done  cmd` notification lines for every job whose
    /// elements have ALL been reaped and that has not yet been reported, and
    /// REMOVE those jobs from the table. One-shot per job. Each returned
    /// string is a complete line (trailing newline included), ready to write
    /// to the terminal.
    pub fn take_done_notifications(&mut self) -> Vec<String> {
        let mut lines = Vec::new();
        for job in &mut self.jobs {
            if job.is_complete() && !job.notified {
                job.notified = true;
                let mut line = String::new();
                // `[N]+ Done  cmd` -- the `+` marks the current job; precise
                // current/previous (`+`/`-`) tracking is U-7b. Two spaces
                // before the command mirror bash's column padding intent
                // without a fixed-width field (the cmd may be long).
                let _ = write!(&mut line, "[{}]+ Done  {}\n", job.spec, job.cmd);
                lines.push(line);
            }
        }
        // Remove the jobs we just reported (all reaped + now notified).
        self.jobs.retain(|j| !(j.is_complete() && j.notified));
        lines
    }

    /// `true` when no jobs are tracked.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.jobs.is_empty()
    }

    /// The number of tracked jobs.
    #[must_use]
    pub fn len(&self) -> usize {
        self.jobs.len()
    }

    /// Iterate the tracked jobs in registration order (the `jobs` builtin,
    /// U-7b).
    pub fn iter(&self) -> impl Iterator<Item = &Job> {
        self.jobs.iter()
    }

    /// The `[N]` spec of the "current" job -- bash's `+` / `%%` / `%+`, the
    /// most-recently-added (highest spec). `None` if the table is empty. The
    /// no-argument default for `fg`/`bg` (U-7b).
    #[must_use]
    pub fn current_spec(&self) -> Option<u32> {
        self.jobs.iter().map(|j| j.spec).max()
    }

    /// The `[N]` spec of the "previous" job -- bash's `-` / `%-`, the
    /// second-most-recently-added. `None` if fewer than two jobs.
    #[must_use]
    pub fn previous_spec(&self) -> Option<u32> {
        let mut specs: Vec<u32> = self.jobs.iter().map(|j| j.spec).collect();
        specs.sort_unstable();
        if specs.len() >= 2 {
            Some(specs[specs.len() - 2])
        } else {
            None
        }
    }

    /// Every element pid (reaped or not, in element order) of the job with
    /// spec `spec`, or `None` if no such job. The `%N` -> pids mapping for
    /// `wait`/`fg`/`kill` (U-7b). A by-pid wait on an already-reaped element
    /// returns -1 from the kernel and is handled by the caller, so returning
    /// every pid (not just the live ones) is safe.
    #[must_use]
    pub fn spec_pids(&self, spec: u32) -> Option<Vec<i32>> {
        self.jobs
            .iter()
            .find(|j| j.spec == spec)
            .map(|j| j.pids.iter().map(|p| p.pid).collect())
    }

    /// The rendered command line of job `spec`, or `None` if no such job
    /// (`fg` echoes it before foregrounding, U-7b).
    #[must_use]
    pub fn cmd_of(&self, spec: u32) -> Option<&str> {
        self.jobs.iter().find(|j| j.spec == spec).map(|j| j.cmd())
    }

    /// Remove job `spec` WITHOUT emitting a `[N]+ Done` notification; returns
    /// whether a job was removed. `fg` uses this: a foregrounded job's
    /// completion is the foreground command's result, not a background event,
    /// so it is consumed silently (no lazy Done line at the next prompt).
    pub fn remove(&mut self, spec: u32) -> bool {
        let before = self.jobs.len();
        self.jobs.retain(|j| j.spec != spec);
        self.jobs.len() != before
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    #[test]
    fn add_assigns_one_based_specs() {
        let mut t = JobTable::new();
        assert_eq!(t.add(vec![10], "echo a".to_string()), 1);
        assert_eq!(t.add(vec![11], "echo b".to_string()), 2);
        assert_eq!(t.len(), 2);
    }

    #[test]
    fn add_rejects_empty_pid_list() {
        let mut t = JobTable::new();
        assert_eq!(t.add(vec![], "nothing".to_string()), 0);
        assert!(t.is_empty());
    }

    #[test]
    fn live_pids_lists_unreaped() {
        let mut t = JobTable::new();
        t.add(vec![10], "a".to_string());
        t.add(vec![20, 21], "b | c".to_string());
        assert_eq!(t.live_pids(), vec![10, 20, 21]);
        t.mark_reaped(20, 0);
        assert_eq!(t.live_pids(), vec![10, 21]);
    }

    #[test]
    fn single_pid_job_done_after_reap() {
        let mut t = JobTable::new();
        let spec = t.add(vec![42], "echo hi".to_string());
        assert_eq!(spec, 1);
        // Not done before reaping -- no notification yet.
        assert!(t.take_done_notifications().is_empty());
        assert_eq!(t.len(), 1);
        t.mark_reaped(42, 0);
        let lines = t.take_done_notifications();
        assert_eq!(lines.len(), 1);
        assert!(lines[0].contains("Done"));
        assert!(lines[0].contains("echo hi"));
        assert!(lines[0].contains("[1]"));
        // The job was removed after reporting.
        assert!(t.is_empty());
    }

    #[test]
    fn pipeline_job_done_only_when_all_reaped() {
        let mut t = JobTable::new();
        t.add(vec![50, 51], "echo hi | tr a-z A-Z".to_string());
        t.mark_reaped(50, 0);
        // One of two reaped -- still running, no notification.
        assert!(t.take_done_notifications().is_empty());
        assert_eq!(t.len(), 1);
        t.mark_reaped(51, 0);
        let lines = t.take_done_notifications();
        assert_eq!(lines.len(), 1);
        assert!(lines[0].contains("echo hi | tr a-z A-Z"));
        assert!(t.is_empty());
    }

    #[test]
    fn mark_reaped_is_idempotent_and_ignores_unknown() {
        let mut t = JobTable::new();
        t.add(vec![60], "x".to_string());
        t.mark_reaped(60, 7);
        // Second call keeps the first status (no panic, no double-state).
        t.mark_reaped(60, 9);
        // An unknown pid is a no-op (a foreground child's already-reaped pid).
        t.mark_reaped(999, 1);
        let lines = t.take_done_notifications();
        assert_eq!(lines.len(), 1);
    }

    #[test]
    fn notification_is_one_shot() {
        let mut t = JobTable::new();
        t.add(vec![70], "y".to_string());
        t.mark_reaped(70, 0);
        assert_eq!(t.take_done_notifications().len(), 1);
        // Already reported + removed; a second drain yields nothing.
        assert!(t.take_done_notifications().is_empty());
    }

    #[test]
    fn spec_resets_to_one_when_table_drains() {
        let mut t = JobTable::new();
        assert_eq!(t.add(vec![1], "a".to_string()), 1);
        assert_eq!(t.add(vec![2], "b".to_string()), 2);
        t.mark_reaped(1, 0);
        t.mark_reaped(2, 0);
        let _ = t.take_done_notifications();
        assert!(t.is_empty());
        // Drained -> the next job is [1] again (bash-like reset).
        assert_eq!(t.add(vec![3], "c".to_string()), 1);
    }

    #[test]
    fn spec_climbs_while_jobs_coexist() {
        let mut t = JobTable::new();
        assert_eq!(t.add(vec![1], "a".to_string()), 1);
        assert_eq!(t.add(vec![2], "b".to_string()), 2);
        // Finish + remove [1]; [2] still live -> next is max+1 = 3.
        t.mark_reaped(1, 0);
        let _ = t.take_done_notifications();
        assert_eq!(t.len(), 1);
        assert_eq!(t.add(vec![3], "c".to_string()), 3);
    }

    #[test]
    fn is_running_reflects_reap_state() {
        let mut t = JobTable::new();
        t.add(vec![80, 81], "p | q".to_string());
        let job = t.iter().next().unwrap();
        assert!(job.is_running());
        assert_eq!(job.last_pid(), 81);
        t.mark_reaped(80, 0);
        assert!(t.iter().next().unwrap().is_running());
        t.mark_reaped(81, 0);
        assert!(!t.iter().next().unwrap().is_running());
    }

    #[test]
    fn current_and_previous_spec_track_recency() {
        let mut t = JobTable::new();
        assert_eq!(t.current_spec(), None);
        assert_eq!(t.previous_spec(), None);
        t.add(vec![10], "a".to_string()); // [1]
        assert_eq!(t.current_spec(), Some(1));
        assert_eq!(t.previous_spec(), None);
        t.add(vec![20], "b".to_string()); // [2]
        assert_eq!(t.current_spec(), Some(2));
        assert_eq!(t.previous_spec(), Some(1));
    }

    #[test]
    fn spec_pids_and_cmd_of_resolve_a_job() {
        let mut t = JobTable::new();
        t.add(vec![30, 31], "p | q".to_string()); // [1]
        assert_eq!(t.spec_pids(1), Some(vec![30, 31]));
        assert_eq!(t.cmd_of(1), Some("p | q"));
        // A reaped element is still returned (the caller's by-pid wait copes).
        t.mark_reaped(30, 0);
        assert_eq!(t.spec_pids(1), Some(vec![30, 31]));
        // No such job.
        assert_eq!(t.spec_pids(99), None);
        assert_eq!(t.cmd_of(99), None);
    }

    #[test]
    fn remove_drops_silently() {
        let mut t = JobTable::new();
        t.add(vec![40], "x".to_string()); // [1]
        assert!(t.remove(1));
        assert!(t.is_empty());
        // A removed job emits NO Done line (fg consumed it).
        assert!(t.take_done_notifications().is_empty());
        // Removing a non-existent spec is a no-op false.
        assert!(!t.remove(7));
    }
}
