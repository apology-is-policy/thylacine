//! The LSP client policy layer (Stage 8e-2) -- pure, host-testable.
//!
//! [`crate::jsonrpc`] is the protocol *grammar*; this module is the *client*. It
//! mints request ids, remembers what each outstanding id asked for, turns a
//! classified [`Incoming`] into an [`Action`] the host acts on, and owns the
//! per-file diagnostic store the editor renders.
//!
//! **No I/O.** The caller owns the transport ([`crate::transport::Server`]);
//! every method that produces traffic RETURNS the message to send. So the whole
//! client -- handshake, dispatch, staleness, diagnostics -- is exercised on the
//! host with no process, and the device wiring stays a thin loop.
//!
//! Two decisions worth naming, both visible in the tests:
//!
//!   - **Latest-wins for cursor-following requests.** Minting a hover /
//!     definition / completion request DROPS any outstanding request of the same
//!     kind, so the superseded reply classifies as [`Action::Ignored`] instead of
//!     painting an answer for a cursor position the user already left. (A stale
//!     answer that *looks* current is worse than no answer.)
//!   - **Position encoding is negotiated, not assumed.** LSP counts `character`
//!     in UTF-16 code units by default -- a silent corruption for any non-ASCII
//!     line if the editor treats it as bytes. We advertise `utf-8` (LSP 3.17
//!     `general.positionEncodings`) and record what the server picked;
//!     [`char_to_byte`] / [`byte_to_char`] convert under whichever it chose.

use crate::json::Value;
use crate::jsonrpc::{self, Id, Incoming};
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

/// A zero-based LSP position. `character` counts units of the negotiated
/// [`PositionEncoding`] -- NOT bytes (unless that encoding is `utf-8`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Position {
    pub line: u32,
    pub character: u32,
}

impl Position {
    pub fn new(line: u32, character: u32) -> Position {
        Position { line, character }
    }
}

/// A half-open `[start, end)` span.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Range {
    pub start: Position,
    pub end: Position,
}

/// Diagnostic severity (LSP 1..=4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    Error,
    Warning,
    Information,
    Hint,
}

/// A diagnostic, reduced to what an editor line actually renders.
#[derive(Clone, Debug, PartialEq)]
pub struct Diagnostic {
    pub range: Range,
    pub severity: Severity,
    pub message: String,
    /// The producing tool (`"compiler"`, `"go vet"`, ...) when the server says.
    pub source: Option<String>,
}

/// A resolved source location (go-to-definition).
#[derive(Clone, Debug, PartialEq)]
pub struct Location {
    pub uri: String,
    pub range: Range,
}

/// One completion candidate, reduced to what a list renders.
#[derive(Clone, Debug, PartialEq)]
pub struct CompletionItem {
    pub label: String,
    pub detail: Option<String>,
    /// Text to insert; falls back to `label` when the server sends none.
    pub insert_text: String,
}

/// How a server counts `character` offsets. UTF-16 is the LSP default; we ask
/// for UTF-8 (byte offsets -- what the editor already has) and honor the answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PositionEncoding {
    Utf8,
    Utf16,
    Utf32,
}

/// What an outstanding request asked for -- so a response dispatches without
/// re-parsing the request that caused it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Pending {
    Initialize,
    Definition,
    Hover,
    Completion,
    Shutdown,
}

impl Pending {
    /// Cursor-following kinds: a newer request supersedes an older one, so the
    /// older reply must not paint (see the module header).
    fn supersedes(self) -> bool {
        matches!(self, Pending::Definition | Pending::Hover | Pending::Completion)
    }
}

/// What the host should do after [`Client::handle`].
#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    /// Write this message to the server (an auto-reply to a server request).
    /// Not sending it can stall a server that waits on the answer.
    Send(Value),
    /// The `initialize` handshake completed: send [`Client::initialized`], then
    /// open documents.
    Ready,
    /// `uri`'s diagnostics were replaced. Repaint if it is on screen; read them
    /// with [`Client::diagnostics_for`].
    Diagnostics(String),
    /// A `textDocument/definition` result (`None` = the server knows of none).
    Definition(Option<Location>),
    /// A `textDocument/hover` result, flattened to plain text.
    Hover(Option<String>),
    /// A `textDocument/completion` result.
    Completion(Vec<CompletionItem>),
    /// A server-side message worth surfacing (`window/logMessage`,
    /// `window/showMessage`).
    Log(String),
    /// One of our requests failed; the string is the server's message.
    Failed(String),
    /// Nothing to do: an unmatched or superseded response, or a notification we
    /// do not consume. NEVER an error -- a client must tolerate anything a
    /// server says without falling over.
    Ignored,
}

/// The LSP client: id allocation, the pending map, the diagnostic store, and
/// the negotiated encoding.
pub struct Client {
    next_id: Id,
    pending: Vec<(Id, Pending)>,
    diagnostics: Vec<(String, Vec<Diagnostic>)>,
    encoding: PositionEncoding,
    ready: bool,
}

impl Default for Client {
    fn default() -> Client {
        Client::new()
    }
}

impl Client {
    pub fn new() -> Client {
        Client {
            next_id: 1,
            pending: Vec::new(),
            diagnostics: Vec::new(),
            // The LSP default until `initialize` says otherwise. Starting here
            // (not at Utf8) means a server that ignores our request, or a
            // handshake that never completes, still converts correctly.
            encoding: PositionEncoding::Utf16,
            ready: false,
        }
    }

    /// The encoding the server chose (valid after [`Action::Ready`]).
    pub fn encoding(&self) -> PositionEncoding {
        self.encoding
    }

    /// Has the `initialize` handshake completed?
    pub fn is_ready(&self) -> bool {
        self.ready
    }

    /// Outstanding request count (tests + a "still thinking" indicator).
    pub fn outstanding(&self) -> usize {
        self.pending.len()
    }

    /// Diagnostics currently known for `uri` (empty when the server has sent
    /// none, or sent an empty list -- which is how it clears them).
    pub fn diagnostics_for(&self, uri: &str) -> &[Diagnostic] {
        self.diagnostics
            .iter()
            .find(|(u, _)| u == uri)
            .map(|(_, d)| d.as_slice())
            .unwrap_or(&[])
    }

    /// Drop everything remembered about `uri` (pair with `didClose`).
    pub fn forget(&mut self, uri: &str) {
        self.diagnostics.retain(|(u, _)| u != uri);
    }

    fn mint(&mut self, kind: Pending) -> Id {
        if kind.supersedes() {
            self.pending.retain(|&(_, k)| k != kind);
        }
        let id = self.next_id;
        self.next_id += 1;
        self.pending.push((id, kind));
        id
    }

    fn take_pending(&mut self, id: Id) -> Option<Pending> {
        self.pending
            .iter()
            .position(|&(i, _)| i == id)
            .map(|ix| self.pending.remove(ix).1)
    }

    // ---- outgoing: the handshake -------------------------------------------

    /// The `initialize` request. `root_uri` is the workspace folder (a
    /// `file://` URI -- see [`path_to_uri`]).
    ///
    /// `processId` is sent as null: it is the server's liveness hint for
    /// self-exit, and the pure layer has no pid. The transport's
    /// close-stdin/kill shutdown is the real lifetime control.
    pub fn initialize(&mut self, root_uri: &str) -> Value {
        let id = self.mint(Pending::Initialize);
        let caps = obj(vec![
            (
                "general",
                obj(vec![(
                    "positionEncodings",
                    Value::Array(vec![Value::from("utf-8"), Value::from("utf-16")]),
                )]),
            ),
            (
                "textDocument",
                obj(vec![
                    ("synchronization", obj(vec![("didSave", Value::Bool(true))])),
                    ("publishDiagnostics", obj(vec![])),
                    (
                        "hover",
                        obj(vec![(
                            "contentFormat",
                            Value::Array(vec![Value::from("plaintext"), Value::from("markdown")]),
                        )]),
                    ),
                    ("definition", obj(vec![])),
                    (
                        "completion",
                        obj(vec![(
                            "completionItem",
                            // We insert plain text; a snippet would arrive with
                            // ${1:...} placeholders we would paste literally.
                            obj(vec![("snippetSupport", Value::Bool(false))]),
                        )]),
                    ),
                ]),
            ),
        ]);
        let params = obj(vec![
            ("processId", Value::Null),
            ("clientInfo", obj(vec![("name", Value::from("nora"))])),
            ("rootUri", Value::from(root_uri)),
            ("capabilities", caps),
            (
                "workspaceFolders",
                Value::Array(vec![obj(vec![
                    ("uri", Value::from(root_uri)),
                    ("name", Value::from("workspace")),
                ])]),
            ),
        ]);
        jsonrpc::request(id, "initialize", params)
    }

    /// The `initialized` notification -- send it once, after [`Action::Ready`].
    pub fn initialized(&self) -> Value {
        jsonrpc::notification("initialized", obj(vec![]))
    }

    /// The `shutdown` request. Follow its response with [`Client::exit`].
    pub fn shutdown(&mut self) -> Value {
        let id = self.mint(Pending::Shutdown);
        jsonrpc::request(id, "shutdown", Value::Null)
    }

    /// The `exit` notification (the server terminates).
    pub fn exit(&self) -> Value {
        jsonrpc::notification("exit", Value::Null)
    }

    // ---- outgoing: document sync -------------------------------------------

    /// `textDocument/didOpen`. `version` starts at 1 and increments per change.
    pub fn did_open(&self, uri: &str, language_id: &str, version: i64, text: &str) -> Value {
        jsonrpc::notification(
            "textDocument/didOpen",
            obj(vec![(
                "textDocument",
                obj(vec![
                    ("uri", Value::from(uri)),
                    ("languageId", Value::from(language_id)),
                    ("version", Value::Int(version)),
                    ("text", Value::from(text)),
                ]),
            )]),
        )
    }

    /// `textDocument/didChange` carrying the WHOLE document (sync kind Full).
    ///
    /// Incremental sync would send just the edited range, but it requires the
    /// client and server to agree on the document state edit-for-edit; a single
    /// dropped or misordered change desynchronizes them silently (the server
    /// then reports diagnostics against text the user never typed). Full sync
    /// is self-correcting by construction. Editor buffers here are source
    /// files, and the send is one write on a keystroke-idle path.
    pub fn did_change_full(&self, uri: &str, version: i64, text: &str) -> Value {
        jsonrpc::notification(
            "textDocument/didChange",
            obj(vec![
                (
                    "textDocument",
                    obj(vec![("uri", Value::from(uri)), ("version", Value::Int(version))]),
                ),
                ("contentChanges", Value::Array(vec![obj(vec![("text", Value::from(text))])])),
            ]),
        )
    }

    /// `textDocument/didSave` (with the saved text -- gopls re-checks on save).
    pub fn did_save(&self, uri: &str, text: &str) -> Value {
        jsonrpc::notification(
            "textDocument/didSave",
            obj(vec![
                ("textDocument", obj(vec![("uri", Value::from(uri))])),
                ("text", Value::from(text)),
            ]),
        )
    }

    /// `textDocument/didClose`.
    pub fn did_close(&self, uri: &str) -> Value {
        jsonrpc::notification(
            "textDocument/didClose",
            obj(vec![("textDocument", obj(vec![("uri", Value::from(uri))]))]),
        )
    }

    // ---- outgoing: cursor-following requests -------------------------------

    fn text_document_position(uri: &str, pos: Position) -> Value {
        obj(vec![
            ("textDocument", obj(vec![("uri", Value::from(uri))])),
            (
                "position",
                obj(vec![
                    ("line", Value::Int(pos.line as i64)),
                    ("character", Value::Int(pos.character as i64)),
                ]),
            ),
        ])
    }

    /// `textDocument/definition` (supersedes any outstanding definition).
    pub fn definition(&mut self, uri: &str, pos: Position) -> Value {
        let id = self.mint(Pending::Definition);
        jsonrpc::request(id, "textDocument/definition", Self::text_document_position(uri, pos))
    }

    /// `textDocument/hover` (supersedes any outstanding hover).
    pub fn hover(&mut self, uri: &str, pos: Position) -> Value {
        let id = self.mint(Pending::Hover);
        jsonrpc::request(id, "textDocument/hover", Self::text_document_position(uri, pos))
    }

    /// `textDocument/completion` (supersedes any outstanding completion).
    pub fn completion(&mut self, uri: &str, pos: Position) -> Value {
        let id = self.mint(Pending::Completion);
        jsonrpc::request(id, "textDocument/completion", Self::text_document_position(uri, pos))
    }

    // ---- incoming ----------------------------------------------------------

    /// Dispatch one classified message. Total: every input yields an action,
    /// and an unknown/unmatched/malformed one yields [`Action::Ignored`] rather
    /// than an error -- the editor must survive whatever the server says.
    pub fn handle(&mut self, msg: Incoming) -> Action {
        match msg {
            Incoming::Response { id, result } => self.on_response(id, result),
            Incoming::Notification { method, params } => self.on_notification(&method, params),
            Incoming::Request { id, method, params } => self.on_server_request(id, &method, params),
        }
    }

    fn on_response(&mut self, id: Id, result: Result<Value, jsonrpc::RpcError>) -> Action {
        // Take the pending entry FIRST: a failed response must clear the slot
        // too, or a superseding request can never reuse the kind.
        let kind = match self.take_pending(id) {
            Some(k) => k,
            // Superseded (we dropped it when a newer request was minted) or a
            // duplicate/unknown id.
            None => return Action::Ignored,
        };
        let value = match result {
            Ok(v) => v,
            Err(e) => {
                return match kind {
                    // A failed hover/definition/completion is routine (no symbol
                    // under the cursor); surfacing it as an error would spam the
                    // status line. Report the empty result instead.
                    Pending::Hover => Action::Hover(None),
                    Pending::Definition => Action::Definition(None),
                    Pending::Completion => Action::Completion(Vec::new()),
                    _ => Action::Failed(e.message),
                };
            }
        };
        match kind {
            Pending::Initialize => {
                self.encoding = parse_encoding(&value);
                self.ready = true;
                Action::Ready
            }
            Pending::Definition => Action::Definition(parse_definition(&value)),
            Pending::Hover => Action::Hover(parse_hover(&value)),
            Pending::Completion => Action::Completion(parse_completion(&value)),
            Pending::Shutdown => Action::Ignored,
        }
    }

    fn on_notification(&mut self, method: &str, params: Value) -> Action {
        match method {
            "textDocument/publishDiagnostics" => {
                let uri = match params.get("uri").and_then(|u| u.as_str()) {
                    Some(u) => String::from(u),
                    None => return Action::Ignored,
                };
                let list = parse_diagnostics(&params);
                match self.diagnostics.iter_mut().find(|(u, _)| *u == uri) {
                    Some(slot) => slot.1 = list,
                    None => self.diagnostics.push((uri.clone(), list)),
                }
                Action::Diagnostics(uri)
            }
            "window/logMessage" | "window/showMessage" => {
                match params.get("message").and_then(|m| m.as_str()) {
                    Some(m) => Action::Log(String::from(m)),
                    None => Action::Ignored,
                }
            }
            _ => Action::Ignored,
        }
    }

    fn on_server_request(&mut self, id: Value, method: &str, params: Value) -> Action {
        match method {
            // One config object per requested item. We advertise no settings,
            // so empty objects = "use your defaults" -- and, critically, the
            // server stops waiting.
            "workspace/configuration" => {
                let n = params.get("items").and_then(|i| i.as_array()).map(|a| a.len()).unwrap_or(0);
                let items = (0..n).map(|_| obj(vec![])).collect::<Vec<_>>();
                Action::Send(jsonrpc::response(id, Value::Array(items)))
            }
            // Housekeeping requests whose contract is "answer null".
            "client/registerCapability"
            | "client/unregisterCapability"
            | "window/workDoneProgress/create"
            | "workspace/semanticTokens/refresh"
            | "workspace/codeLens/refresh"
            | "workspace/diagnostic/refresh" => Action::Send(jsonrpc::response(id, Value::Null)),
            // Anything else: the protocol-correct refusal. Silence would hang a
            // server that blocks on the reply.
            _ => Action::Send(jsonrpc::error_response(id, -32601, "method not found")),
        }
    }
}

// ---- parsing helpers (free fns: pure, individually testable) ----------------

fn obj(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (String::from(k), v)).collect())
}

fn parse_encoding(init_result: &Value) -> PositionEncoding {
    match init_result
        .get("capabilities")
        .and_then(|c| c.get("positionEncoding"))
        .and_then(|e| e.as_str())
    {
        Some("utf-8") => PositionEncoding::Utf8,
        Some("utf-32") => PositionEncoding::Utf32,
        // Absent = the LSP default. An unknown string is also safest read as the
        // default rather than guessed.
        _ => PositionEncoding::Utf16,
    }
}

fn parse_position(v: &Value) -> Position {
    Position {
        line: v.get("line").and_then(|l| l.as_i64()).unwrap_or(0).max(0) as u32,
        character: v.get("character").and_then(|c| c.as_i64()).unwrap_or(0).max(0) as u32,
    }
}

fn parse_range(v: &Value) -> Range {
    Range {
        start: v.get("start").map(parse_position).unwrap_or_default(),
        end: v.get("end").map(parse_position).unwrap_or_default(),
    }
}

fn parse_severity(v: Option<&Value>) -> Severity {
    match v.and_then(|s| s.as_i64()) {
        Some(2) => Severity::Warning,
        Some(3) => Severity::Information,
        Some(4) => Severity::Hint,
        // 1, absent, or out of range. The spec leaves "absent" to the client;
        // the visible choice is the safe one for a compiler diagnostic.
        _ => Severity::Error,
    }
}

fn parse_diagnostics(params: &Value) -> Vec<Diagnostic> {
    let arr = match params.get("diagnostics").and_then(|d| d.as_array()) {
        Some(a) => a,
        None => return Vec::new(),
    };
    arr.iter()
        .map(|d| Diagnostic {
            range: d.get("range").map(parse_range).unwrap_or_default(),
            severity: parse_severity(d.get("severity")),
            message: d.get("message").and_then(|m| m.as_str()).unwrap_or("").to_string(),
            source: d.get("source").and_then(|s| s.as_str()).map(String::from),
        })
        .collect()
}

/// `Location | Location[] | LocationLink[] | null` -- take the first.
fn parse_definition(v: &Value) -> Option<Location> {
    fn one(v: &Value) -> Option<Location> {
        // A LocationLink names the target differently from a Location.
        if let Some(uri) = v.get("targetUri").and_then(|u| u.as_str()) {
            let range = v
                .get("targetSelectionRange")
                .or_else(|| v.get("targetRange"))
                .map(parse_range)
                .unwrap_or_default();
            return Some(Location { uri: String::from(uri), range });
        }
        let uri = v.get("uri").and_then(|u| u.as_str())?;
        Some(Location {
            uri: String::from(uri),
            range: v.get("range").map(parse_range).unwrap_or_default(),
        })
    }
    match v {
        Value::Array(items) => items.first().and_then(one),
        Value::Null => None,
        other => one(other),
    }
}

/// `{contents}` where contents is `MarkupContent | MarkedString | MarkedString[]`.
fn parse_hover(v: &Value) -> Option<String> {
    fn marked(v: &Value) -> Option<String> {
        match v {
            Value::Str(s) => Some(s.clone()),
            // MarkupContent {kind, value} and MarkedString {language, value}
            // both carry the text in `value`.
            _ => v.get("value").and_then(|x| x.as_str()).map(String::from),
        }
    }
    let contents = v.get("contents")?;
    let text = match contents {
        Value::Array(items) => {
            let parts: Vec<String> = items.iter().filter_map(marked).collect();
            if parts.is_empty() {
                return None;
            }
            parts.join("\n")
        }
        other => marked(other)?,
    };
    if text.trim().is_empty() {
        None
    } else {
        Some(text)
    }
}

/// `CompletionItem[] | {isIncomplete, items} | null`.
fn parse_completion(v: &Value) -> Vec<CompletionItem> {
    let items = match v {
        Value::Array(a) => a.as_slice(),
        _ => match v.get("items").and_then(|i| i.as_array()) {
            Some(a) => a,
            None => return Vec::new(),
        },
    };
    items
        .iter()
        .filter_map(|it| {
            let label = it.get("label").and_then(|l| l.as_str())?;
            let insert = it
                .get("insertText")
                .and_then(|t| t.as_str())
                .or_else(|| it.get("textEdit").and_then(|e| e.get("newText")).and_then(|t| t.as_str()))
                .unwrap_or(label);
            Some(CompletionItem {
                label: String::from(label),
                detail: it.get("detail").and_then(|d| d.as_str()).map(String::from),
                insert_text: String::from(insert),
            })
        })
        .collect()
}

// ---- position conversion ---------------------------------------------------

/// Convert an LSP `character` offset within `line` to a BYTE offset.
///
/// Clamps: past the end of the line yields `line.len()`, and an offset landing
/// mid-character (a stale position, or an encoding mismatch) rounds UP to the
/// next character boundary -- so the result is always a safe slice index.
pub fn char_to_byte(line: &str, character: u32, enc: PositionEncoding) -> usize {
    let target = character as usize;
    if target == 0 {
        return 0;
    }
    let mut units = 0usize;
    for (off, ch) in line.char_indices() {
        if units >= target {
            return off;
        }
        units += unit_len(ch, enc);
    }
    line.len()
}

/// Convert a BYTE offset within `line` to an LSP `character` offset. A byte
/// offset landing mid-character counts that character (the inverse rounding of
/// [`char_to_byte`]).
pub fn byte_to_char(line: &str, byte: usize, enc: PositionEncoding) -> u32 {
    let end = byte.min(line.len());
    let mut units = 0usize;
    for (off, ch) in line.char_indices() {
        if off >= end {
            break;
        }
        units += unit_len(ch, enc);
    }
    units as u32
}

fn unit_len(ch: char, enc: PositionEncoding) -> usize {
    match enc {
        PositionEncoding::Utf8 => ch.len_utf8(),
        PositionEncoding::Utf16 => ch.len_utf16(),
        PositionEncoding::Utf32 => 1,
    }
}

// ---- file URIs -------------------------------------------------------------

fn hex_digit(n: u8) -> char {
    (if n < 10 { b'0' + n } else { b'A' + (n - 10) }) as char
}

/// Build a `file://` URI from an ABSOLUTE path (the caller resolves relatives
/// against the cwd -- this layer has no filesystem).
///
/// Percent-encodes every byte outside the RFC 3986 unreserved set, keeping `/`
/// as the path separator. A path is bytes, not necessarily UTF-8 text; encoding
/// per byte is correct either way.
pub fn path_to_uri(path: &str) -> String {
    let mut s = String::from("file://");
    for &b in path.as_bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' | b'/' => {
                s.push(b as char)
            }
            _ => {
                s.push('%');
                s.push(hex_digit(b >> 4));
                s.push(hex_digit(b & 0x0f));
            }
        }
    }
    s
}

/// Recover the path from a `file://` URI. Returns `None` for a non-`file` URI
/// or a percent-escape that does not decode to UTF-8.
pub fn uri_to_path(uri: &str) -> Option<String> {
    // file:// + path, or file:/path (both appear in the wild); an authority
    // other than empty/localhost is a remote file we cannot open.
    let rest = uri.strip_prefix("file://").or_else(|| uri.strip_prefix("file:"))?;
    let rest = rest.strip_prefix("localhost").unwrap_or(rest);
    let bytes = rest.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hi = (bytes[i + 1] as char).to_digit(16)?;
            let lo = (bytes[i + 2] as char).to_digit(16)?;
            out.push((hi * 16 + lo) as u8);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::jsonrpc::classify;

    fn parse(s: &str) -> Value {
        Value::parse(s.as_bytes()).expect("valid json")
    }

    /// Feed the client a raw server message the way the transport would.
    fn feed(c: &mut Client, s: &str) -> Action {
        c.handle(classify(parse(s)).expect("classifiable"))
    }

    /// Read the id out of a request we just built.
    fn id_of(v: &Value) -> i64 {
        v.get("id").and_then(|i| i.as_i64()).expect("request has an int id")
    }

    #[test]
    fn initialize_advertises_utf8_and_completes() {
        let mut c = Client::new();
        let req = c.initialize("file:///w");
        assert_eq!(req.get("method").unwrap().as_str(), Some("initialize"));
        // We must ASK for utf-8 or the server keeps its utf-16 default.
        let encs = req
            .get("params")
            .and_then(|p| p.get("capabilities"))
            .and_then(|c| c.get("general"))
            .and_then(|g| g.get("positionEncodings"))
            .and_then(|e| e.as_array())
            .expect("positionEncodings advertised");
        assert_eq!(encs[0].as_str(), Some("utf-8"));
        assert!(!c.is_ready());
        assert_eq!(c.outstanding(), 1);

        let id = id_of(&req);
        let act = feed(
            &mut c,
            &alloc::format!(
                r#"{{"jsonrpc":"2.0","id":{},"result":{{"capabilities":{{"positionEncoding":"utf-8"}}}}}}"#,
                id
            ),
        );
        assert_eq!(act, Action::Ready);
        assert!(c.is_ready());
        assert_eq!(c.encoding(), PositionEncoding::Utf8);
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn initialize_without_encoding_keeps_the_lsp_default() {
        let mut c = Client::new();
        let id = id_of(&c.initialize("file:///w"));
        let msg = alloc::format!(r#"{{"id":{},"result":{{"capabilities":{{}}}}}}"#, id);
        assert_eq!(feed(&mut c, &msg), Action::Ready);
        assert_eq!(c.encoding(), PositionEncoding::Utf16);
    }

    #[test]
    fn diagnostics_are_stored_replaced_and_cleared() {
        let mut c = Client::new();
        let act = feed(
            &mut c,
            r#"{"method":"textDocument/publishDiagnostics","params":{"uri":"file:///a.go",
               "diagnostics":[{"range":{"start":{"line":1,"character":8},"end":{"line":1,"character":11}},
                               "severity":1,"message":"undefined: zzz","source":"compiler"}]}}"#,
        );
        assert_eq!(act, Action::Diagnostics(String::from("file:///a.go")));
        let d = c.diagnostics_for("file:///a.go");
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].severity, Severity::Error);
        assert_eq!(d[0].message, "undefined: zzz");
        assert_eq!(d[0].source.as_deref(), Some("compiler"));
        assert_eq!(d[0].range.start, Position::new(1, 8));
        assert_eq!(d[0].range.end, Position::new(1, 11));
        // An unknown file has none.
        assert!(c.diagnostics_for("file:///b.go").is_empty());

        // A second publish REPLACES (an empty list is how a server clears).
        feed(
            &mut c,
            r#"{"method":"textDocument/publishDiagnostics","params":{"uri":"file:///a.go","diagnostics":[]}}"#,
        );
        assert!(c.diagnostics_for("file:///a.go").is_empty());
    }

    #[test]
    fn diagnostic_severities_and_defaults() {
        let mut c = Client::new();
        feed(
            &mut c,
            r#"{"method":"textDocument/publishDiagnostics","params":{"uri":"file:///s.go",
               "diagnostics":[{"severity":2,"message":"w"},{"severity":3,"message":"i"},
                              {"severity":4,"message":"h"},{"message":"no severity"}]}}"#,
        );
        let d = c.diagnostics_for("file:///s.go");
        assert_eq!(d.len(), 4);
        assert_eq!(d[0].severity, Severity::Warning);
        assert_eq!(d[1].severity, Severity::Information);
        assert_eq!(d[2].severity, Severity::Hint);
        assert_eq!(d[3].severity, Severity::Error); // absent -> visible
        // A diagnostic with no range still lands (defaulted), never dropped.
        assert_eq!(d[3].range, Range::default());
    }

    #[test]
    fn forget_drops_a_files_diagnostics() {
        let mut c = Client::new();
        feed(
            &mut c,
            r#"{"method":"textDocument/publishDiagnostics","params":{"uri":"file:///a.go","diagnostics":[{"message":"x"}]}}"#,
        );
        assert_eq!(c.diagnostics_for("file:///a.go").len(), 1);
        c.forget("file:///a.go");
        assert!(c.diagnostics_for("file:///a.go").is_empty());
    }

    #[test]
    fn hover_supersedes_so_a_stale_reply_never_paints() {
        let mut c = Client::new();
        let first = id_of(&c.hover("file:///a.go", Position::new(1, 1)));
        let second = id_of(&c.hover("file:///a.go", Position::new(9, 9)));
        assert_ne!(first, second);
        // Only the newest hover is live.
        assert_eq!(c.outstanding(), 1);

        // The superseded reply is dropped even though it arrives first...
        let stale = alloc::format!(r#"{{"id":{},"result":{{"contents":"STALE"}}}}"#, first);
        assert_eq!(feed(&mut c, &stale), Action::Ignored);
        // ...and the current one still paints.
        let fresh = alloc::format!(r#"{{"id":{},"result":{{"contents":"FRESH"}}}}"#, second);
        assert_eq!(feed(&mut c, &fresh), Action::Hover(Some(String::from("FRESH"))));
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn distinct_kinds_do_not_supersede_each_other() {
        let mut c = Client::new();
        let h = id_of(&c.hover("file:///a.go", Position::default()));
        let d = id_of(&c.definition("file:///a.go", Position::default()));
        let k = id_of(&c.completion("file:///a.go", Position::default()));
        assert_eq!(c.outstanding(), 3);
        let hm = alloc::format!(r#"{{"id":{},"result":{{"contents":"h"}}}}"#, h);
        assert_eq!(feed(&mut c, &hm), Action::Hover(Some(String::from("h"))));
        let dm = alloc::format!(
            r#"{{"id":{},"result":{{"uri":"file:///b.go","range":{{"start":{{"line":3,"character":0}},"end":{{"line":3,"character":4}}}}}}}}"#,
            d
        );
        match feed(&mut c, &dm) {
            Action::Definition(Some(loc)) => {
                assert_eq!(loc.uri, "file:///b.go");
                assert_eq!(loc.range.start, Position::new(3, 0));
            }
            other => panic!("wrong: {:?}", other),
        }
        let km = alloc::format!(r#"{{"id":{},"result":[]}}"#, k);
        assert_eq!(feed(&mut c, &km), Action::Completion(Vec::new()));
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn definition_accepts_every_shape() {
        // bare Location
        assert_eq!(
            parse_definition(&parse(r#"{"uri":"file:///a","range":{"start":{"line":1,"character":2},"end":{"line":1,"character":3}}}"#)),
            Some(Location {
                uri: String::from("file:///a"),
                range: Range { start: Position::new(1, 2), end: Position::new(1, 3) },
            })
        );
        // Location[] -- first wins
        let arr = parse(r#"[{"uri":"file:///first"},{"uri":"file:///second"}]"#);
        assert_eq!(parse_definition(&arr).unwrap().uri, "file:///first");
        // LocationLink[] (targetUri + targetSelectionRange)
        let links = parse(
            r#"[{"targetUri":"file:///t","targetSelectionRange":{"start":{"line":5,"character":1},"end":{"line":5,"character":6}}}]"#,
        );
        let l = parse_definition(&links).unwrap();
        assert_eq!(l.uri, "file:///t");
        assert_eq!(l.range.start, Position::new(5, 1));
        // null / empty
        assert_eq!(parse_definition(&Value::Null), None);
        assert_eq!(parse_definition(&parse("[]")), None);
    }

    #[test]
    fn hover_accepts_every_shape() {
        assert_eq!(parse_hover(&parse(r#"{"contents":"plain"}"#)).as_deref(), Some("plain"));
        assert_eq!(
            // r##..##: the body contains `"#`, which would close an r#"..."#.
            parse_hover(&parse(r##"{"contents":{"kind":"markdown","value":"# md"}}"##)).as_deref(),
            Some("# md")
        );
        assert_eq!(
            parse_hover(&parse(r#"{"contents":[{"language":"go","value":"func f()"},"docs"]}"#)).as_deref(),
            Some("func f()\ndocs")
        );
        // Empty / absent contents are "no hover", not an empty popup.
        assert_eq!(parse_hover(&parse(r#"{"contents":""}"#)), None);
        assert_eq!(parse_hover(&parse(r#"{"contents":[]}"#)), None);
        assert_eq!(parse_hover(&parse(r#"{}"#)), None);
    }

    #[test]
    fn completion_accepts_both_shapes_and_falls_back_to_label() {
        let items = parse_completion(&parse(
            r#"{"isIncomplete":false,"items":[
                 {"label":"Println","detail":"func(a ...any)","insertText":"Println"},
                 {"label":"Printf"},
                 {"label":"Sprint","textEdit":{"newText":"Sprint"}},
                 {"detail":"no label -- skipped"}]}"#,
        ));
        assert_eq!(items.len(), 3);
        assert_eq!(items[0].label, "Println");
        assert_eq!(items[0].detail.as_deref(), Some("func(a ...any)"));
        assert_eq!(items[1].insert_text, "Printf"); // falls back to label
        assert_eq!(items[2].insert_text, "Sprint"); // from textEdit.newText
        // The bare-array shape.
        assert_eq!(parse_completion(&parse(r#"[{"label":"x"}]"#)).len(), 1);
        assert_eq!(parse_completion(&Value::Null).len(), 0);
    }

    #[test]
    fn a_failed_cursor_request_is_an_empty_result_not_an_error() {
        let mut c = Client::new();
        let h = id_of(&c.hover("file:///a.go", Position::default()));
        let msg = alloc::format!(
            r#"{{"id":{},"error":{{"code":-32602,"message":"no identifier found"}}}}"#,
            h
        );
        assert_eq!(feed(&mut c, &msg), Action::Hover(None));
        // The slot is cleared even on failure -- else the kind wedges.
        assert_eq!(c.outstanding(), 0);
    }

    #[test]
    fn a_failed_handshake_surfaces() {
        let mut c = Client::new();
        let id = id_of(&c.initialize("file:///w"));
        let msg = alloc::format!(r#"{{"id":{},"error":{{"code":-32603,"message":"boom"}}}}"#, id);
        assert_eq!(feed(&mut c, &msg), Action::Failed(String::from("boom")));
        assert!(!c.is_ready());
    }

    #[test]
    fn unmatched_and_unknown_messages_are_ignored_not_fatal() {
        let mut c = Client::new();
        assert_eq!(feed(&mut c, r#"{"id":9999,"result":{}}"#), Action::Ignored);
        assert_eq!(feed(&mut c, r#"{"method":"$/progress","params":{}}"#), Action::Ignored);
        // A diagnostics notification with no uri cannot be stored.
        assert_eq!(
            feed(&mut c, r#"{"method":"textDocument/publishDiagnostics","params":{}}"#),
            Action::Ignored
        );
    }

    #[test]
    fn log_messages_surface() {
        let mut c = Client::new();
        assert_eq!(
            feed(&mut c, r#"{"method":"window/logMessage","params":{"type":3,"message":"hello"}}"#),
            Action::Log(String::from("hello"))
        );
        assert_eq!(
            feed(&mut c, r#"{"method":"window/showMessage","params":{"message":"hi"}}"#),
            Action::Log(String::from("hi"))
        );
    }

    #[test]
    fn server_requests_are_answered() {
        let mut c = Client::new();
        // workspace/configuration -> one object per item, id echoed.
        match feed(
            &mut c,
            r#"{"id":11,"method":"workspace/configuration","params":{"items":[{"section":"gopls"},{"section":"x"}]}}"#,
        ) {
            Action::Send(v) => {
                assert_eq!(v.get("id").unwrap().as_i64(), Some(11));
                assert_eq!(v.get("result").unwrap().as_array().unwrap().len(), 2);
            }
            other => panic!("wrong: {:?}", other),
        }
        // registerCapability -> null result (a STRING id must echo verbatim).
        match feed(&mut c, r#"{"id":"reg-1","method":"client/registerCapability","params":{}}"#) {
            Action::Send(v) => {
                assert_eq!(v.get("id").unwrap().as_str(), Some("reg-1"));
                assert!(v.get("result").unwrap().is_null());
            }
            other => panic!("wrong: {:?}", other),
        }
        // Unknown -> MethodNotFound, never silence (a blocked server hangs).
        match feed(&mut c, r#"{"id":12,"method":"workspace/applyEdit","params":{}}"#) {
            Action::Send(v) => {
                assert_eq!(v.get("error").unwrap().get("code").unwrap().as_i64(), Some(-32601));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn document_sync_messages_are_well_formed() {
        let c = Client::new();
        let open = c.did_open("file:///a.go", "go", 1, "package main");
        assert_eq!(open.get("method").unwrap().as_str(), Some("textDocument/didOpen"));
        let td = open.get("params").unwrap().get("textDocument").unwrap();
        assert_eq!(td.get("languageId").unwrap().as_str(), Some("go"));
        assert_eq!(td.get("version").unwrap().as_i64(), Some(1));
        assert_eq!(td.get("text").unwrap().as_str(), Some("package main"));
        // didOpen is a NOTIFICATION -- an id would make the server reply forever.
        assert!(open.get("id").is_none());

        let chg = c.did_change_full("file:///a.go", 2, "package main // x");
        let changes = chg.get("params").unwrap().get("contentChanges").unwrap().as_array().unwrap();
        assert_eq!(changes.len(), 1);
        // Full sync: the whole text, no range member.
        assert_eq!(changes[0].get("text").unwrap().as_str(), Some("package main // x"));
        assert!(changes[0].get("range").is_none());

        assert_eq!(
            c.did_save("file:///a.go", "t").get("method").unwrap().as_str(),
            Some("textDocument/didSave")
        );
        assert_eq!(
            c.did_close("file:///a.go").get("method").unwrap().as_str(),
            Some("textDocument/didClose")
        );
    }

    #[test]
    fn position_conversion_utf8_utf16_utf32() {
        // "é" is 2 bytes / 1 utf16 unit; the emoji is 4 bytes / 2 utf16 units.
        let line = "aébc";
        assert_eq!(char_to_byte(line, 0, PositionEncoding::Utf16), 0);
        assert_eq!(char_to_byte(line, 1, PositionEncoding::Utf16), 1); // after 'a'
        assert_eq!(char_to_byte(line, 2, PositionEncoding::Utf16), 3); // after 'é'
        // Under utf-8 the character offset IS the byte offset -- so a server
        // reports 3 (not 2) for the same spot, and it maps straight through.
        assert_eq!(char_to_byte(line, 3, PositionEncoding::Utf8), 3);
        assert_eq!(char_to_byte(line, 1, PositionEncoding::Utf8), 1);
        assert_eq!(byte_to_char(line, 3, PositionEncoding::Utf16), 2);
        assert_eq!(byte_to_char(line, 3, PositionEncoding::Utf8), 3);

        let emoji = String::from("x") + core::str::from_utf8(&[0xF0, 0x9F, 0x92, 0xA1]).unwrap() + "y";
        assert_eq!(char_to_byte(&emoji, 1, PositionEncoding::Utf16), 1);
        assert_eq!(char_to_byte(&emoji, 3, PositionEncoding::Utf16), 5); // surrogate pair
        assert_eq!(char_to_byte(&emoji, 2, PositionEncoding::Utf32), 5);
        assert_eq!(byte_to_char(&emoji, 5, PositionEncoding::Utf16), 3);
    }

    #[test]
    fn position_conversion_clamps_instead_of_panicking() {
        let line = "aé";
        // Past the end.
        assert_eq!(char_to_byte(line, 99, PositionEncoding::Utf16), line.len());
        assert_eq!(byte_to_char(line, 99, PositionEncoding::Utf16), 2);
        // Mid-character (a stale position, or an encoding mismatch): rounds UP
        // to a boundary, so the result is always a safe slice index. Here
        // utf-8 character 2 lands inside 'é' (bytes 1..3) -> 3, not 2.
        let b = char_to_byte(line, 2, PositionEncoding::Utf8);
        assert!(line.is_char_boundary(b));
        assert_eq!(b, 3);
        assert!(line.is_char_boundary(char_to_byte(line, 1, PositionEncoding::Utf16)));
        assert_eq!(char_to_byte("", 5, PositionEncoding::Utf8), 0);
    }

    #[test]
    fn uri_roundtrip_and_encoding() {
        assert_eq!(path_to_uri("/home/m/a.go"), "file:///home/m/a.go");
        // A space must not land raw in a URI.
        assert_eq!(path_to_uri("/tmp/my file.go"), "file:///tmp/my%20file.go");
        assert_eq!(uri_to_path("file:///tmp/my%20file.go").as_deref(), Some("/tmp/my file.go"));
        assert_eq!(uri_to_path("file:///home/m/a.go").as_deref(), Some("/home/m/a.go"));
        // Both the two-slash and the bare form, plus the localhost authority.
        assert_eq!(uri_to_path("file:/x").as_deref(), Some("/x"));
        assert_eq!(uri_to_path("file://localhost/x").as_deref(), Some("/x"));
        // Not a file URI.
        assert_eq!(uri_to_path("https://example.com/x"), None);
        // Round-trip a path needing several escapes.
        let p = "/a b/c%d/e.go";
        assert_eq!(uri_to_path(&path_to_uri(p)).as_deref(), Some(p));
    }
}
