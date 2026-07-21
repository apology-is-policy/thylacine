//! The DAP client policy layer (Stage 8e-3b) -- pure, host-testable.
//!
//! [`crate::dap`] is the protocol *grammar*; this module is the *client*. It
//! mints sequence numbers, remembers what each outstanding `seq` asked for,
//! turns a classified [`Incoming`](crate::dap::Incoming) into an [`Action`] the
//! host acts on, and parses the response/event bodies down to what a debugger UI
//! actually shows (stack frames, scopes, variables, an evaluate result).
//!
//! **No I/O.** The caller owns the transport ([`crate::transport::Server`]);
//! every method that produces traffic RETURNS the message to send. So the whole
//! client -- the launch handshake, dispatch, the pending map -- is exercised on
//! the host with no process, and the device wiring stays a thin loop.
//!
//! The launch handshake the host drives (the canonical VS Code / Delve order):
//!
//! ```text
//!   send initialize
//!   Action::Initialized(caps)  -> send launch (or attach)
//!   Action::ConfigureBreakpoints (the `initialized` EVENT)
//!                              -> send setFunctionBreakpoints/... then configurationDone
//!   Action::Stopped(entry)     -> the target halted; inspect / continue / step
//! ```
//!
//! Unlike LSP there is no "latest-wins" supersession: every DAP request carries a
//! unique `seq` and a response matches exactly one pending entry, so a stale
//! reply is impossible by construction. A response that fails (`success:false`)
//! carries the server's message; the host presents it.

use crate::dap::{self, Incoming, Seq};
use crate::json::Value;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

/// The server capabilities we bother to record (Delve sets all of these). The
/// host does not branch on them at v1.0 -- they are captured so a future UI can
/// gate a feature it must not offer against a server that lacks it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Capabilities {
    pub configuration_done: bool,
    pub function_breakpoints: bool,
    pub conditional_breakpoints: bool,
    pub evaluate_for_hovers: bool,
}

/// A `stopped` event, reduced to what a debugger surfaces.
#[derive(Clone, Debug, PartialEq)]
pub struct Stopped {
    /// `breakpoint` / `step` / `entry` / `pause` / `exception` / ... Delve may
    /// send its own strings (e.g. a hardware breakpoint), so a UI must not gate
    /// on an exact value -- it reports the reason, it does not switch on it.
    pub reason: String,
    pub thread_id: i64,
    pub description: Option<String>,
    pub text: Option<String>,
    /// Whether every thread stopped (Go: normally true -- the whole process).
    pub all_threads_stopped: bool,
}

/// An `output` event (the debuggee's stdout/stderr, or a debug-console line).
#[derive(Clone, Debug, PartialEq)]
pub struct Output {
    /// `console` / `stdout` / `stderr` / `important` -- absent defaults to
    /// `console`.
    pub category: String,
    pub output: String,
}

/// One stack frame (a `stackTrace` response entry).
#[derive(Clone, Debug, PartialEq)]
pub struct StackFrame {
    /// The frame id -- the handle passed to `scopes`.
    pub id: i64,
    pub name: String,
    /// The source file, when the frame has one (a runtime frame may not).
    pub source_path: Option<String>,
    pub line: i64,
    pub column: i64,
}

/// One variable scope (a `scopes` response entry).
#[derive(Clone, Debug, PartialEq)]
pub struct Scope {
    pub name: String,
    /// The handle passed to `variables`.
    pub variables_reference: i64,
    /// A hint that reading this scope is costly (do not auto-expand).
    pub expensive: bool,
}

/// One variable (a `variables` response entry, or a resolved `evaluate`).
#[derive(Clone, Debug, PartialEq)]
pub struct Variable {
    pub name: String,
    pub value: String,
    pub ty: Option<String>,
    /// Non-zero when the value is structured and can be expanded (pass it back
    /// to `variables`).
    pub variables_reference: i64,
}

/// An `evaluate` result.
#[derive(Clone, Debug, PartialEq)]
pub struct EvalResult {
    pub result: String,
    pub ty: Option<String>,
    pub variables_reference: i64,
}

/// One breakpoint's verification state (a `setBreakpoints` /
/// `setFunctionBreakpoints` response entry).
#[derive(Clone, Debug, PartialEq)]
pub struct BreakpointInfo {
    /// The server bound this breakpoint to real code.
    pub verified: bool,
    pub id: Option<i64>,
    pub line: Option<i64>,
    /// Why it could not be verified, when the server says.
    pub message: Option<String>,
}

/// One thread (a `threads` response entry).
#[derive(Clone, Debug, PartialEq)]
pub struct ThreadInfo {
    pub id: i64,
    pub name: String,
}

/// What an outstanding request asked for, so a response dispatches without
/// re-parsing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cmd {
    Initialize,
    Launch,
    Attach,
    SetFunctionBreakpoints,
    SetBreakpoints,
    ConfigurationDone,
    Continue,
    Next,
    StepIn,
    StepOut,
    Pause,
    StackTrace,
    Scopes,
    Variables,
    Evaluate,
    Threads,
    Disconnect,
}

impl Cmd {
    /// The DAP command string (for the [`Action::Ack`] label + failure text).
    fn as_str(self) -> &'static str {
        match self {
            Cmd::Initialize => "initialize",
            Cmd::Launch => "launch",
            Cmd::Attach => "attach",
            Cmd::SetFunctionBreakpoints => "setFunctionBreakpoints",
            Cmd::SetBreakpoints => "setBreakpoints",
            Cmd::ConfigurationDone => "configurationDone",
            Cmd::Continue => "continue",
            Cmd::Next => "next",
            Cmd::StepIn => "stepIn",
            Cmd::StepOut => "stepOut",
            Cmd::Pause => "pause",
            Cmd::StackTrace => "stackTrace",
            Cmd::Scopes => "scopes",
            Cmd::Variables => "variables",
            Cmd::Evaluate => "evaluate",
            Cmd::Threads => "threads",
            Cmd::Disconnect => "disconnect",
        }
    }
}

/// What the host should do after [`Client::handle`].
#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    /// Write this message to the server (a reply to a reverse-request). Not
    /// sending it can stall a server that waits on the answer.
    Send(Value),
    /// The `initialize` RESPONSE arrived: send `launch` / `attach` now.
    Initialized(Capabilities),
    /// The `initialized` EVENT arrived: the server is ready for breakpoint
    /// configuration. Send setFunctionBreakpoints/setBreakpoints, then
    /// [`Client::configuration_done`].
    ConfigureBreakpoints,
    /// A `stopped` event -- the target halted.
    Stopped(Stopped),
    /// A `continued` event.
    Continued,
    /// An `output` event (debuggee output / debug-console text).
    Output(Output),
    /// The debuggee `exited` with this code.
    Exited(i64),
    /// The debug session `terminated` (no more debuggee).
    Terminated,
    /// A `thread` event (`started` / `exited`) for this thread id.
    Thread { reason: String, thread_id: i64 },
    /// A `setBreakpoints` / `setFunctionBreakpoints` response.
    Breakpoints(Vec<BreakpointInfo>),
    /// A `stackTrace` response.
    StackTrace(Vec<StackFrame>),
    /// A `scopes` response.
    Scopes(Vec<Scope>),
    /// A `variables` response.
    Variables(Vec<Variable>),
    /// An `evaluate` response.
    Evaluate(EvalResult),
    /// A `threads` response.
    Threads(Vec<ThreadInfo>),
    /// A request with no structured result succeeded (launch / attach /
    /// configurationDone / continue / next / stepIn / stepOut / pause /
    /// disconnect). The `&str` is the command.
    Ack(&'static str),
    /// One of our requests failed; the string is `"<command>: <server message>"`.
    Failed(String),
    /// Nothing to do: an unmatched response, or an event we do not consume.
    /// NEVER an error -- a client must tolerate anything the server says.
    Ignored,
}

/// The DAP client: seq allocation, the pending map, and the negotiated
/// capabilities.
pub struct Client {
    next_seq: Seq,
    pending: Vec<(Seq, Cmd)>,
    caps: Capabilities,
    configured: bool,
}

impl Default for Client {
    fn default() -> Client {
        Client::new()
    }
}

impl Client {
    pub fn new() -> Client {
        Client { next_seq: 1, pending: Vec::new(), caps: Capabilities::default(), configured: false }
    }

    /// The capabilities the server reported (valid after [`Action::Initialized`]).
    pub fn capabilities(&self) -> Capabilities {
        self.caps
    }

    /// Has the `initialized` event arrived (breakpoints may be configured)?
    pub fn is_configured(&self) -> bool {
        self.configured
    }

    /// Outstanding request count (tests + a "still thinking" indicator).
    pub fn outstanding(&self) -> usize {
        self.pending.len()
    }

    fn mint_seq(&mut self) -> Seq {
        let s = self.next_seq;
        self.next_seq += 1;
        s
    }

    fn mint(&mut self, kind: Cmd) -> Seq {
        let seq = self.mint_seq();
        self.pending.push((seq, kind));
        seq
    }

    fn take_pending(&mut self, request_seq: Seq) -> Option<Cmd> {
        self.pending
            .iter()
            .position(|&(s, _)| s == request_seq)
            .map(|ix| self.pending.remove(ix).1)
    }

    // ---- outgoing: the handshake -------------------------------------------

    /// The `initialize` request. `adapter_id` names the debug type (`"go"`).
    pub fn initialize(&mut self, adapter_id: &str) -> Value {
        let seq = self.mint(Cmd::Initialize);
        let args = obj(vec![
            ("clientID", Value::from("nora")),
            ("clientName", Value::from("nora")),
            ("adapterID", Value::from(adapter_id)),
            ("locale", Value::from("en")),
            // 1-based lines/columns: what a human types and what a `bt` prints.
            // The editor's own positions are 0-based; the host adds 1 when it
            // translates a cursor into a source breakpoint.
            ("linesStartAt1", Value::Bool(true)),
            ("columnsStartAt1", Value::Bool(true)),
            ("pathFormat", Value::from("path")),
            ("supportsVariableType", Value::Bool(true)),
            // We host no terminal and cannot be asked to start a nested session,
            // so we decline both reverse-requests up front -- a server that
            // honors this never sends them.
            ("supportsRunInTerminalRequest", Value::Bool(false)),
            ("supportsStartDebuggingRequest", Value::Bool(false)),
        ]);
        dap::request(seq, "initialize", args)
    }

    /// The `launch` request. `mode` is `"exec"` (run a prebuilt binary -- the
    /// only mode that needs no on-device `go build`) or `"debug"`; `program` is
    /// the target path.
    pub fn launch(&mut self, mode: &str, program: &str, stop_on_entry: bool) -> Value {
        let seq = self.mint(Cmd::Launch);
        let args = obj(vec![
            ("request", Value::from("launch")),
            ("mode", Value::from(mode)),
            ("program", Value::from(program)),
            ("stopOnEntry", Value::Bool(stop_on_entry)),
        ]);
        dap::request(seq, "launch", args)
    }

    /// The `attach` request against a running process.
    pub fn attach(&mut self, pid: i64, stop_on_entry: bool) -> Value {
        let seq = self.mint(Cmd::Attach);
        let args = obj(vec![
            ("request", Value::from("attach")),
            ("mode", Value::from("local")),
            ("processId", Value::Int(pid)),
            ("stopOnEntry", Value::Bool(stop_on_entry)),
        ]);
        dap::request(seq, "attach", args)
    }

    /// `setFunctionBreakpoints` -- name-based breakpoints (`"main.parkLoop"`).
    /// On Thylacine these resolve to entry PCs and arm hardware breakpoints.
    pub fn set_function_breakpoints(&mut self, names: &[&str]) -> Value {
        let seq = self.mint(Cmd::SetFunctionBreakpoints);
        let bps: Vec<Value> = names.iter().map(|n| obj(vec![("name", Value::from(*n))])).collect();
        dap::request(
            seq,
            "setFunctionBreakpoints",
            obj(vec![("breakpoints", Value::Array(bps))]),
        )
    }

    /// `setBreakpoints` -- source-line breakpoints in one file. `lines` are as
    /// the server counts them (1-based, since we advertised `linesStartAt1`).
    pub fn set_breakpoints(&mut self, source_path: &str, lines: &[i64]) -> Value {
        let seq = self.mint(Cmd::SetBreakpoints);
        let bps: Vec<Value> =
            lines.iter().map(|&l| obj(vec![("line", Value::Int(l))])).collect();
        dap::request(
            seq,
            "setBreakpoints",
            obj(vec![
                ("source", obj(vec![("path", Value::from(source_path))])),
                ("breakpoints", Value::Array(bps)),
            ]),
        )
    }

    /// `configurationDone` -- the last configuration step; launch completes and
    /// (with stopOnEntry) the target stops at entry.
    pub fn configuration_done(&mut self) -> Value {
        let seq = self.mint(Cmd::ConfigurationDone);
        dap::request(seq, "configurationDone", empty_args())
    }

    // ---- outgoing: execution control ---------------------------------------

    pub fn continue_(&mut self, thread_id: i64) -> Value {
        self.thread_request(Cmd::Continue, "continue", thread_id)
    }
    pub fn next(&mut self, thread_id: i64) -> Value {
        self.thread_request(Cmd::Next, "next", thread_id)
    }
    pub fn step_in(&mut self, thread_id: i64) -> Value {
        self.thread_request(Cmd::StepIn, "stepIn", thread_id)
    }
    pub fn step_out(&mut self, thread_id: i64) -> Value {
        self.thread_request(Cmd::StepOut, "stepOut", thread_id)
    }
    pub fn pause(&mut self, thread_id: i64) -> Value {
        self.thread_request(Cmd::Pause, "pause", thread_id)
    }

    fn thread_request(&mut self, kind: Cmd, command: &str, thread_id: i64) -> Value {
        let seq = self.mint(kind);
        dap::request(seq, command, obj(vec![("threadId", Value::Int(thread_id))]))
    }

    // ---- outgoing: inspection ----------------------------------------------

    /// `stackTrace` for one thread. `levels` caps the frames returned (0 = all).
    pub fn stack_trace(&mut self, thread_id: i64, levels: i64) -> Value {
        let seq = self.mint(Cmd::StackTrace);
        dap::request(
            seq,
            "stackTrace",
            obj(vec![
                ("threadId", Value::Int(thread_id)),
                ("startFrame", Value::Int(0)),
                ("levels", Value::Int(levels)),
            ]),
        )
    }

    /// `scopes` for a frame (the id from a [`StackFrame`]).
    pub fn scopes(&mut self, frame_id: i64) -> Value {
        let seq = self.mint(Cmd::Scopes);
        dap::request(seq, "scopes", obj(vec![("frameId", Value::Int(frame_id))]))
    }

    /// `variables` for a reference (from a [`Scope`] or an expandable
    /// [`Variable`]).
    pub fn variables(&mut self, variables_reference: i64) -> Value {
        let seq = self.mint(Cmd::Variables);
        dap::request(
            seq,
            "variables",
            obj(vec![("variablesReference", Value::Int(variables_reference))]),
        )
    }

    /// `evaluate` an expression in a frame's context (`context` = `"repl"` /
    /// `"watch"` / `"hover"`).
    pub fn evaluate(&mut self, expression: &str, frame_id: i64, context: &str) -> Value {
        let seq = self.mint(Cmd::Evaluate);
        dap::request(
            seq,
            "evaluate",
            obj(vec![
                ("expression", Value::from(expression)),
                ("frameId", Value::Int(frame_id)),
                ("context", Value::from(context)),
            ]),
        )
    }

    /// `threads` -- the live thread list.
    pub fn threads(&mut self) -> Value {
        let seq = self.mint(Cmd::Threads);
        dap::request(seq, "threads", empty_args())
    }

    /// `disconnect`. `terminate` kills the debuggee; `false` detaches.
    pub fn disconnect(&mut self, terminate: bool) -> Value {
        let seq = self.mint(Cmd::Disconnect);
        dap::request(
            seq,
            "disconnect",
            obj(vec![
                ("restart", Value::Bool(false)),
                ("terminateDebuggee", Value::Bool(terminate)),
            ]),
        )
    }

    // ---- incoming ----------------------------------------------------------

    /// Dispatch one classified message. Total: every input yields an action, and
    /// an unknown/unmatched/malformed one yields [`Action::Ignored`] rather than
    /// an error -- the debugger UI must survive whatever the server says.
    pub fn handle(&mut self, msg: Incoming) -> Action {
        match msg {
            Incoming::Response { request_seq, command, success, message, body } => {
                self.on_response(request_seq, &command, success, message, body)
            }
            Incoming::Event { event, body } => self.on_event(&event, body),
            Incoming::Request { seq, command, .. } => self.on_reverse_request(seq, &command),
        }
    }

    fn on_response(
        &mut self,
        request_seq: Seq,
        _command: &str,
        success: bool,
        message: Option<String>,
        body: Value,
    ) -> Action {
        // Take the pending entry FIRST -- even a failed response must clear it,
        // or the slot leaks. An unknown request_seq (duplicate / never ours) is
        // simply ignored.
        let kind = match self.take_pending(request_seq) {
            Some(k) => k,
            None => return Action::Ignored,
        };
        if !success {
            let msg = message.unwrap_or_default();
            let mut s = String::from(kind.as_str());
            s.push_str(": ");
            s.push_str(&msg);
            return Action::Failed(s);
        }
        match kind {
            Cmd::Initialize => {
                self.caps = parse_capabilities(&body);
                Action::Initialized(self.caps)
            }
            Cmd::SetFunctionBreakpoints | Cmd::SetBreakpoints => {
                Action::Breakpoints(parse_breakpoints(&body))
            }
            Cmd::StackTrace => Action::StackTrace(parse_stack_frames(&body)),
            Cmd::Scopes => Action::Scopes(parse_scopes(&body)),
            Cmd::Variables => Action::Variables(parse_variables(&body)),
            Cmd::Evaluate => Action::Evaluate(parse_evaluate(&body)),
            Cmd::Threads => Action::Threads(parse_threads(&body)),
            // Requests whose success carries no structured result the UI needs.
            Cmd::Launch => Action::Ack("launch"),
            Cmd::Attach => Action::Ack("attach"),
            Cmd::ConfigurationDone => Action::Ack("configurationDone"),
            Cmd::Continue => Action::Ack("continue"),
            Cmd::Next => Action::Ack("next"),
            Cmd::StepIn => Action::Ack("stepIn"),
            Cmd::StepOut => Action::Ack("stepOut"),
            Cmd::Pause => Action::Ack("pause"),
            Cmd::Disconnect => Action::Ack("disconnect"),
        }
    }

    fn on_event(&mut self, event: &str, body: Value) -> Action {
        match event {
            "initialized" => {
                self.configured = true;
                Action::ConfigureBreakpoints
            }
            "stopped" => Action::Stopped(parse_stopped(&body)),
            "continued" => Action::Continued,
            "output" => Action::Output(parse_output(&body)),
            "exited" => Action::Exited(body.get("exitCode").and_then(|c| c.as_i64()).unwrap_or(0)),
            "terminated" => Action::Terminated,
            "thread" => Action::Thread {
                reason: body.get("reason").and_then(|r| r.as_str()).unwrap_or("").to_string(),
                thread_id: body.get("threadId").and_then(|t| t.as_i64()).unwrap_or(0),
            },
            // process / module / loadedSource / breakpoint / capabilities:
            // informational, not consumed by the headless surface.
            _ => Action::Ignored,
        }
    }

    fn on_reverse_request(&mut self, request_seq: Seq, command: &str) -> Action {
        // We advertised support for neither reverse-request, so a conforming
        // server never sends one. If one arrives anyway, decline it explicitly:
        // silence would hang a server that blocks on the reply.
        let seq = self.mint_seq();
        Action::Send(dap::error_response(seq, request_seq, command, "not supported by nora"))
    }
}

// ---- parsing helpers (free fns: pure, individually testable) ----------------

fn obj(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (String::from(k), v)).collect())
}

/// A command with no arguments still sends `arguments:{}` (see [`dap::request`]).
fn empty_args() -> Value {
    Value::Object(Vec::new())
}

fn cap(body: &Value, key: &str) -> bool {
    body.get(key).and_then(|b| b.as_bool()).unwrap_or(false)
}

fn parse_capabilities(body: &Value) -> Capabilities {
    Capabilities {
        configuration_done: cap(body, "supportsConfigurationDoneRequest"),
        function_breakpoints: cap(body, "supportsFunctionBreakpoints"),
        conditional_breakpoints: cap(body, "supportsConditionalBreakpoints"),
        evaluate_for_hovers: cap(body, "supportsEvaluateForHovers"),
    }
}

fn parse_stopped(body: &Value) -> Stopped {
    Stopped {
        reason: body.get("reason").and_then(|r| r.as_str()).unwrap_or("").to_string(),
        thread_id: body.get("threadId").and_then(|t| t.as_i64()).unwrap_or(0),
        description: body.get("description").and_then(|d| d.as_str()).map(String::from),
        text: body.get("text").and_then(|t| t.as_str()).map(String::from),
        all_threads_stopped: body
            .get("allThreadsStopped")
            .and_then(|a| a.as_bool())
            .unwrap_or(false),
    }
}

fn parse_output(body: &Value) -> Output {
    Output {
        category: body.get("category").and_then(|c| c.as_str()).unwrap_or("console").to_string(),
        output: body.get("output").and_then(|o| o.as_str()).unwrap_or("").to_string(),
    }
}

fn parse_stack_frames(body: &Value) -> Vec<StackFrame> {
    let arr = match body.get("stackFrames").and_then(|f| f.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter()
        .map(|f| StackFrame {
            id: f.get("id").and_then(|i| i.as_i64()).unwrap_or(0),
            name: f.get("name").and_then(|n| n.as_str()).unwrap_or("").to_string(),
            source_path: f.get("source").and_then(|s| s.get("path")).and_then(|p| p.as_str()).map(String::from),
            line: f.get("line").and_then(|l| l.as_i64()).unwrap_or(0),
            column: f.get("column").and_then(|c| c.as_i64()).unwrap_or(0),
        })
        .collect()
}

fn parse_scopes(body: &Value) -> Vec<Scope> {
    let arr = match body.get("scopes").and_then(|s| s.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter()
        .map(|s| Scope {
            name: s.get("name").and_then(|n| n.as_str()).unwrap_or("").to_string(),
            variables_reference: s.get("variablesReference").and_then(|r| r.as_i64()).unwrap_or(0),
            expensive: s.get("expensive").and_then(|e| e.as_bool()).unwrap_or(false),
        })
        .collect()
}

fn parse_variables(body: &Value) -> Vec<Variable> {
    let arr = match body.get("variables").and_then(|v| v.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter().map(parse_one_variable).collect()
}

fn parse_one_variable(v: &Value) -> Variable {
    Variable {
        name: v.get("name").and_then(|n| n.as_str()).unwrap_or("").to_string(),
        value: v.get("value").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        ty: v.get("type").and_then(|t| t.as_str()).map(String::from),
        variables_reference: v.get("variablesReference").and_then(|r| r.as_i64()).unwrap_or(0),
    }
}

fn parse_evaluate(body: &Value) -> EvalResult {
    EvalResult {
        result: body.get("result").and_then(|r| r.as_str()).unwrap_or("").to_string(),
        ty: body.get("type").and_then(|t| t.as_str()).map(String::from),
        variables_reference: body.get("variablesReference").and_then(|r| r.as_i64()).unwrap_or(0),
    }
}

fn parse_breakpoints(body: &Value) -> Vec<BreakpointInfo> {
    let arr = match body.get("breakpoints").and_then(|b| b.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter()
        .map(|b| BreakpointInfo {
            verified: b.get("verified").and_then(|v| v.as_bool()).unwrap_or(false),
            id: b.get("id").and_then(|i| i.as_i64()),
            line: b.get("line").and_then(|l| l.as_i64()),
            message: b.get("message").and_then(|m| m.as_str()).map(String::from),
        })
        .collect()
}

fn parse_threads(body: &Value) -> Vec<ThreadInfo> {
    let arr = match body.get("threads").and_then(|t| t.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter()
        .map(|t| ThreadInfo {
            id: t.get("id").and_then(|i| i.as_i64()).unwrap_or(0),
            name: t.get("name").and_then(|n| n.as_str()).unwrap_or("").to_string(),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dap::classify;

    fn parse(s: &str) -> Value {
        Value::parse(s.as_bytes()).expect("valid json")
    }

    /// Feed the client a raw server message the way the transport would.
    fn feed(c: &mut Client, s: &str) -> Action {
        c.handle(classify(parse(s)).expect("classifiable"))
    }

    /// Read the seq out of a request we just built.
    fn seq_of(v: &Value) -> i64 {
        v.get("seq").and_then(|s| s.as_i64()).expect("request has a seq")
    }

    #[test]
    fn initialize_is_well_formed_and_records_capabilities() {
        let mut c = Client::new();
        let req = c.initialize("go");
        assert_eq!(req.get("type").unwrap().as_str(), Some("request"));
        assert_eq!(req.get("command").unwrap().as_str(), Some("initialize"));
        let args = req.get("arguments").unwrap();
        assert_eq!(args.get("adapterID").unwrap().as_str(), Some("go"));
        // We must decline both reverse-requests or a server may send one.
        assert_eq!(args.get("supportsRunInTerminalRequest").unwrap().as_bool(), Some(false));
        assert_eq!(c.outstanding(), 1);

        let rs = seq_of(&req);
        let act = feed(
            &mut c,
            &alloc::format!(
                r#"{{"type":"response","request_seq":{},"success":true,"command":"initialize","body":{{"supportsConfigurationDoneRequest":true,"supportsFunctionBreakpoints":true}}}}"#,
                rs
            ),
        );
        match act {
            Action::Initialized(caps) => {
                assert!(caps.configuration_done);
                assert!(caps.function_breakpoints);
                assert!(!caps.conditional_breakpoints);
            }
            other => panic!("wrong: {:?}", other),
        }
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn the_full_launch_handshake_sequences_via_actions() {
        let mut c = Client::new();
        // initialize -> Initialized
        let rs = seq_of(&c.initialize("go"));
        assert!(matches!(
            feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"initialize","body":{{}}}}"#, rs)),
            Action::Initialized(_)
        ));
        // host sends launch
        let launch = c.launch("exec", "/bin/prog", true);
        assert_eq!(launch.get("arguments").unwrap().get("mode").unwrap().as_str(), Some("exec"));
        let launch_rs = seq_of(&launch);
        // the server's `initialized` EVENT -> ConfigureBreakpoints
        assert_eq!(feed(&mut c, r#"{"type":"event","event":"initialized"}"#), Action::ConfigureBreakpoints);
        assert!(c.is_configured());
        // host sends function breakpoints + configurationDone
        let bps = c.set_function_breakpoints(&["main.parkLoop"]);
        let bps_rs = seq_of(&bps);
        let cfgdone = c.configuration_done();
        let cfgdone_rs = seq_of(&cfgdone);
        // breakpoints response
        match feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"setFunctionBreakpoints","body":{{"breakpoints":[{{"verified":true,"id":1}}]}}}}"#, bps_rs)) {
            Action::Breakpoints(bl) => {
                assert_eq!(bl.len(), 1);
                assert!(bl[0].verified);
                assert_eq!(bl[0].id, Some(1));
            }
            other => panic!("wrong: {:?}", other),
        }
        // configurationDone + launch acks (order independent -- matched by seq)
        assert_eq!(feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"configurationDone"}}"#, cfgdone_rs)), Action::Ack("configurationDone"));
        assert_eq!(feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"launch"}}"#, launch_rs)), Action::Ack("launch"));
        // the entry stop
        match feed(&mut c, r#"{"type":"event","event":"stopped","body":{"reason":"entry","threadId":1,"allThreadsStopped":true}}"#) {
            Action::Stopped(s) => {
                assert_eq!(s.reason, "entry");
                assert_eq!(s.thread_id, 1);
                assert!(s.all_threads_stopped);
            }
            other => panic!("wrong: {:?}", other),
        }
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn stack_scopes_variables_evaluate_parse() {
        let mut c = Client::new();
        let st_rs = seq_of(&c.stack_trace(1, 20));
        match feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"stackTrace","body":{{"stackFrames":[{{"id":1000,"name":"main.parkLoop","source":{{"path":"/x/main.go"}},"line":12,"column":2}},{{"id":1001,"name":"main.main","line":20}}],"totalFrames":2}}}}"#, st_rs)) {
            Action::StackTrace(frames) => {
                assert_eq!(frames.len(), 2);
                assert_eq!(frames[0].name, "main.parkLoop");
                assert_eq!(frames[0].id, 1000);
                assert_eq!(frames[0].source_path.as_deref(), Some("/x/main.go"));
                assert_eq!(frames[0].line, 12);
                assert_eq!(frames[1].source_path, None); // a frame may lack source
            }
            other => panic!("wrong: {:?}", other),
        }
        let sc_rs = seq_of(&c.scopes(1000));
        match feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"scopes","body":{{"scopes":[{{"name":"Locals","variablesReference":2000,"expensive":false}}]}}}}"#, sc_rs)) {
            Action::Scopes(scopes) => {
                assert_eq!(scopes.len(), 1);
                assert_eq!(scopes[0].name, "Locals");
                assert_eq!(scopes[0].variables_reference, 2000);
            }
            other => panic!("wrong: {:?}", other),
        }
        let v_rs = seq_of(&c.variables(2000));
        match feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"variables","body":{{"variables":[{{"name":"i","value":"7","type":"int","variablesReference":0}},{{"name":"p","value":"*T","type":"*main.T","variablesReference":2001}}]}}}}"#, v_rs)) {
            Action::Variables(vars) => {
                assert_eq!(vars.len(), 2);
                assert_eq!(vars[0].name, "i");
                assert_eq!(vars[0].value, "7");
                assert_eq!(vars[0].ty.as_deref(), Some("int"));
                assert_eq!(vars[1].variables_reference, 2001); // expandable
            }
            other => panic!("wrong: {:?}", other),
        }
        let e_rs = seq_of(&c.evaluate("main.Sentinel", 1000, "repl"));
        match feed(&mut c, &alloc::format!(r#"{{"type":"response","request_seq":{},"success":true,"command":"evaluate","body":{{"result":"768901734683508737","type":"int64","variablesReference":0}}}}"#, e_rs)) {
            Action::Evaluate(er) => {
                assert_eq!(er.result, "768901734683508737");
                assert_eq!(er.ty.as_deref(), Some("int64"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn a_failed_request_surfaces_the_command_and_message() {
        let mut c = Client::new();
        let rs = seq_of(&c.evaluate("zzz", 1, "repl"));
        let act = feed(
            &mut c,
            &alloc::format!(
                r#"{{"type":"response","request_seq":{},"success":false,"command":"evaluate","message":"could not find symbol value for zzz"}}"#,
                rs
            ),
        );
        assert_eq!(
            act,
            Action::Failed(String::from("evaluate: could not find symbol value for zzz"))
        );
        // The slot clears even on failure.
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn events_map_to_actions() {
        let mut c = Client::new();
        assert_eq!(feed(&mut c, r#"{"type":"event","event":"continued","body":{"threadId":1}}"#), Action::Continued);
        match feed(&mut c, r#"{"type":"event","event":"output","body":{"category":"stdout","output":"hello\n"}}"#) {
            Action::Output(o) => {
                assert_eq!(o.category, "stdout");
                assert_eq!(o.output, "hello\n");
            }
            other => panic!("wrong: {:?}", other),
        }
        assert_eq!(feed(&mut c, r#"{"type":"event","event":"exited","body":{"exitCode":3}}"#), Action::Exited(3));
        assert_eq!(feed(&mut c, r#"{"type":"event","event":"terminated"}"#), Action::Terminated);
        match feed(&mut c, r#"{"type":"event","event":"thread","body":{"reason":"started","threadId":5}}"#) {
            Action::Thread { reason, thread_id } => {
                assert_eq!(reason, "started");
                assert_eq!(thread_id, 5);
            }
            other => panic!("wrong: {:?}", other),
        }
        // an event we do not consume
        assert_eq!(feed(&mut c, r#"{"type":"event","event":"module","body":{}}"#), Action::Ignored);
    }

    #[test]
    fn output_defaults_category_to_console() {
        let mut c = Client::new();
        match feed(&mut c, r#"{"type":"event","event":"output","body":{"output":"x"}}"#) {
            Action::Output(o) => assert_eq!(o.category, "console"),
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn a_reverse_request_is_declined_not_ignored() {
        let mut c = Client::new();
        // A conforming server never sends this (we declined support), but if one
        // does we must answer or it may hang.
        match feed(&mut c, r#"{"type":"request","seq":40,"command":"runInTerminal","arguments":{}}"#) {
            Action::Send(reply) => {
                assert_eq!(reply.get("type").unwrap().as_str(), Some("response"));
                assert_eq!(reply.get("request_seq").unwrap().as_i64(), Some(40));
                assert_eq!(reply.get("success").unwrap().as_bool(), Some(false));
                assert_eq!(reply.get("command").unwrap().as_str(), Some("runInTerminal"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn unmatched_response_is_ignored_not_fatal() {
        let mut c = Client::new();
        // never our seq
        assert_eq!(feed(&mut c, r#"{"type":"response","request_seq":9999,"success":true,"command":"threads"}"#), Action::Ignored);
    }

    #[test]
    fn seqs_are_unique_and_monotonic() {
        let mut c = Client::new();
        let a = seq_of(&c.initialize("go"));
        let b = seq_of(&c.threads());
        let d = seq_of(&c.continue_(1));
        assert!(a < b && b < d);
        assert_eq!(c.outstanding(), 3);
    }

    #[test]
    fn execution_control_requests_are_well_formed() {
        let mut c = Client::new();
        for (v, cmd) in [
            (c.continue_(1), "continue"),
            (c.next(1), "next"),
            (c.step_in(1), "stepIn"),
            (c.step_out(1), "stepOut"),
            (c.pause(1), "pause"),
        ] {
            assert_eq!(v.get("command").unwrap().as_str(), Some(cmd));
            assert_eq!(v.get("arguments").unwrap().get("threadId").unwrap().as_i64(), Some(1));
        }
        let d = c.disconnect(true);
        assert_eq!(d.get("command").unwrap().as_str(), Some("disconnect"));
        assert_eq!(d.get("arguments").unwrap().get("terminateDebuggee").unwrap().as_bool(), Some(true));
    }

    #[test]
    fn source_breakpoints_are_well_formed() {
        let mut c = Client::new();
        let v = c.set_breakpoints("/x/main.go", &[10, 20]);
        let args = v.get("arguments").unwrap();
        assert_eq!(args.get("source").unwrap().get("path").unwrap().as_str(), Some("/x/main.go"));
        let bps = args.get("breakpoints").unwrap().as_array().unwrap();
        assert_eq!(bps.len(), 2);
        assert_eq!(bps[0].get("line").unwrap().as_i64(), Some(10));
    }
}
