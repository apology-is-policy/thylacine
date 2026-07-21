//! The Debug Adapter Protocol message model -- what Ambush (Delve) speaks.
//!
//! DAP is a *different* envelope from LSP's JSON-RPC 2.0 ([`crate::jsonrpc`]): a
//! message has a `seq` and a `type` of `request` / `response` / `event`, with no
//! `jsonrpc`/`method`/`id`. Correlation is by `request_seq` (a response echoes
//! the `seq` of the request it answers); dispatch is by `command` (on requests
//! and responses) or by `event` (on events). The Content-Length framing
//! ([`crate::frame`]) and the JSON codec ([`crate::json`]) are shared with LSP,
//! but nothing here touches jsonrpc.
//!
//! This module is the pure protocol *grammar*: it builds outgoing requests (and
//! responses to the server's reverse-requests), and **classifies** an incoming
//! parsed value into the three things a client must react to:
//!
//!   - a **Response** to one of *our* requests (matched by the `request_seq`,
//!     which is the `seq` we minted);
//!   - a server **Event** (e.g. `stopped`, `output`, `terminated`), which we act
//!     on but never answer;
//!   - a server-initiated **Request** (a DAP *reverse request*, e.g.
//!     `runInTerminal` / `startDebugging`), which we must answer -- the response
//!     echoes the server's `seq` as our `request_seq`.
//!
//! Seq allocation + response matching (a counter + a pending map) belong to the
//! client layer ([`crate::dapc`]); this module is the pure envelope.

use crate::json::Value;
use alloc::string::String;
use alloc::vec::Vec;

/// A DAP sequence number. Every message from a source carries a strictly
/// increasing `seq`; we mint our own for the requests (and reverse-request
/// responses) we send, and correlate a response by its `request_seq`.
pub type Seq = i64;

fn obj(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (String::from(k), v)).collect())
}

/// Build an outgoing request:
/// `{"seq":seq,"type":"request","command":command,"arguments":arguments}`.
///
/// `arguments` is always included (pass `Value::Object(vec![])` for a command
/// that takes none) -- an explicit empty object is what every DAP server accepts,
/// and it keeps the wire shape deterministic.
pub fn request(seq: Seq, command: &str, arguments: Value) -> Value {
    obj(alloc::vec![
        ("seq", Value::Int(seq)),
        ("type", Value::from("request")),
        ("command", Value::from(command)),
        ("arguments", arguments),
    ])
}

/// Build a success response to a server reverse-request (echo its `seq` as
/// `request_seq`).
pub fn response(seq: Seq, request_seq: Seq, command: &str, body: Value) -> Value {
    obj(alloc::vec![
        ("seq", Value::Int(seq)),
        ("type", Value::from("response")),
        ("request_seq", Value::Int(request_seq)),
        ("success", Value::Bool(true)),
        ("command", Value::from(command)),
        ("body", body),
    ])
}

/// Build an error response to a server reverse-request.
pub fn error_response(seq: Seq, request_seq: Seq, command: &str, message: &str) -> Value {
    obj(alloc::vec![
        ("seq", Value::Int(seq)),
        ("type", Value::from("response")),
        ("request_seq", Value::Int(request_seq)),
        ("success", Value::Bool(false)),
        ("command", Value::from(command)),
        ("message", Value::from(message)),
    ])
}

/// A classified incoming DAP message.
#[derive(Clone, Debug, PartialEq)]
pub enum Incoming {
    /// A reply to one of our requests (matched by `request_seq`). `success` is
    /// the request's outcome; on failure `message` carries the server's reason
    /// and `body` is usually `Null`.
    Response {
        request_seq: Seq,
        command: String,
        success: bool,
        message: Option<String>,
        body: Value,
    },
    /// A server event (no reply). `body` is `Null` when the event carries none.
    Event { event: String, body: Value },
    /// A server reverse-request we must answer (`seq` echoed back as our
    /// `request_seq`). Rare: `runInTerminal`, `startDebugging`.
    Request { seq: Seq, command: String, arguments: Value },
}

/// Classify a parsed JSON value as a DAP message. Consumes the value (moves its
/// fields out -- no clone of a large `body`/`arguments`). Dispatch is on the
/// required `type` member.
pub fn classify(v: Value) -> Result<Incoming, &'static str> {
    let mut kvs = match v {
        Value::Object(kvs) => kvs,
        _ => return Err("message is not a JSON object"),
    };
    let ty = match take_str(&mut kvs, "type") {
        Some(t) => t,
        None => return Err("message has no type"),
    };
    match ty.as_str() {
        "response" => {
            let request_seq =
                take_i64(&mut kvs, "request_seq").ok_or("response missing request_seq")?;
            let command = take_str(&mut kvs, "command").unwrap_or_default();
            // A response with no explicit `success:true` is treated as a
            // failure: a client must never assume an unconfirmed request
            // succeeded (a missing/garbled success degrades to "no result",
            // never to a false positive).
            let success = take_bool(&mut kvs, "success").unwrap_or(false);
            let message = take_str(&mut kvs, "message");
            let body = take(&mut kvs, "body").unwrap_or(Value::Null);
            Ok(Incoming::Response { request_seq, command, success, message, body })
        }
        "event" => {
            let event = take_str(&mut kvs, "event").ok_or("event missing event name")?;
            let body = take(&mut kvs, "body").unwrap_or(Value::Null);
            Ok(Incoming::Event { event, body })
        }
        "request" => {
            let seq = take_i64(&mut kvs, "seq").ok_or("reverse-request missing seq")?;
            let command = take_str(&mut kvs, "command").ok_or("request missing command")?;
            let arguments = take(&mut kvs, "arguments").unwrap_or(Value::Null);
            Ok(Incoming::Request { seq, command, arguments })
        }
        _ => Err("unknown message type"),
    }
}

/// Remove and return the value at `key` from an object's pair list.
fn take(kvs: &mut Vec<(String, Value)>, key: &str) -> Option<Value> {
    kvs.iter().position(|(k, _)| k == key).map(|i| kvs.remove(i).1)
}

/// Remove `key` and return it as a `String` (only when it is a string).
fn take_str(kvs: &mut Vec<(String, Value)>, key: &str) -> Option<String> {
    match take(kvs, key) {
        Some(Value::Str(s)) => Some(s),
        _ => None,
    }
}

/// Remove `key` and return it as an `i64` (only when it is an integer number).
fn take_i64(kvs: &mut Vec<(String, Value)>, key: &str) -> Option<Seq> {
    take(kvs, key).and_then(|v| v.as_i64())
}

/// Remove `key` and return it as a `bool` (only when it is a JSON boolean).
fn take_bool(kvs: &mut Vec<(String, Value)>, key: &str) -> Option<bool> {
    take(kvs, key).and_then(|v| v.as_bool())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::Value;

    fn parse(s: &str) -> Value {
        Value::parse(s.as_bytes()).expect("valid json")
    }

    #[test]
    fn build_request_shape() {
        let args = parse(r#"{"program":"/bin/x","stopOnEntry":true}"#);
        let v = request(3, "launch", args);
        assert_eq!(
            v.to_string(),
            r#"{"seq":3,"type":"request","command":"launch","arguments":{"program":"/bin/x","stopOnEntry":true}}"#
        );
    }

    #[test]
    fn build_request_with_empty_arguments() {
        let v = request(5, "configurationDone", Value::Object(alloc::vec![]));
        assert_eq!(
            v.to_string(),
            r#"{"seq":5,"type":"request","command":"configurationDone","arguments":{}}"#
        );
    }

    #[test]
    fn build_response_and_error() {
        assert_eq!(
            response(9, 4, "runInTerminal", obj(alloc::vec![("processId", Value::Int(0))])).to_string(),
            r#"{"seq":9,"type":"response","request_seq":4,"success":true,"command":"runInTerminal","body":{"processId":0}}"#
        );
        assert_eq!(
            error_response(9, 4, "runInTerminal", "not supported").to_string(),
            r#"{"seq":9,"type":"response","request_seq":4,"success":false,"command":"runInTerminal","message":"not supported"}"#
        );
    }

    #[test]
    fn classify_response_success() {
        let v = parse(
            r#"{"seq":10,"type":"response","request_seq":2,"success":true,"command":"stackTrace",
               "body":{"stackFrames":[{"id":1000,"name":"main.main"}],"totalFrames":1}}"#,
        );
        match classify(v).unwrap() {
            Incoming::Response { request_seq, command, success, message, body } => {
                assert_eq!(request_seq, 2);
                assert_eq!(command, "stackTrace");
                assert!(success);
                assert_eq!(message, None);
                let frames = body.get("stackFrames").unwrap().as_array().unwrap();
                assert_eq!(frames[0].get("name").unwrap().as_str(), Some("main.main"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_response_failure_carries_message() {
        let v = parse(
            r#"{"seq":11,"type":"response","request_seq":7,"success":false,"command":"evaluate","message":"could not find symbol value for zzz"}"#,
        );
        match classify(v).unwrap() {
            Incoming::Response { request_seq, success, message, body, .. } => {
                assert_eq!(request_seq, 7);
                assert!(!success);
                assert_eq!(message.as_deref(), Some("could not find symbol value for zzz"));
                assert!(body.is_null());
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_event() {
        let v = parse(
            r#"{"seq":12,"type":"event","event":"stopped","body":{"reason":"breakpoint","threadId":1}}"#,
        );
        match classify(v).unwrap() {
            Incoming::Event { event, body } => {
                assert_eq!(event, "stopped");
                assert_eq!(body.get("reason").unwrap().as_str(), Some("breakpoint"));
                assert_eq!(body.get("threadId").unwrap().as_i64(), Some(1));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_event_without_body() {
        // `initialized` is a bodyless event; it must classify, not error.
        let v = parse(r#"{"seq":1,"type":"event","event":"initialized"}"#);
        match classify(v).unwrap() {
            Incoming::Event { event, body } => {
                assert_eq!(event, "initialized");
                assert!(body.is_null());
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_reverse_request() {
        let v = parse(
            r#"{"seq":20,"type":"request","command":"runInTerminal","arguments":{"args":["x"],"cwd":"/"}}"#,
        );
        match classify(v).unwrap() {
            Incoming::Request { seq, command, arguments } => {
                assert_eq!(seq, 20);
                assert_eq!(command, "runInTerminal");
                assert_eq!(arguments.get("cwd").unwrap().as_str(), Some("/"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_errors() {
        // not an object
        assert!(classify(parse("[1,2]")).is_err());
        // no type
        assert!(classify(parse(r#"{"seq":1}"#)).is_err());
        // unknown type
        assert!(classify(parse(r#"{"type":"bogus"}"#)).is_err());
        // response with no request_seq
        assert!(classify(parse(r#"{"type":"response","command":"x"}"#)).is_err());
        // event with no event name
        assert!(classify(parse(r#"{"type":"event"}"#)).is_err());
        // reverse-request with no command
        assert!(classify(parse(r#"{"type":"request","seq":1}"#)).is_err());
    }

    #[test]
    fn missing_success_reads_as_failure() {
        // A response that omits `success` must NOT be taken as a success -- a
        // client can never assume an unconfirmed request worked.
        let v = parse(r#"{"type":"response","request_seq":3,"command":"threads"}"#);
        match classify(v).unwrap() {
            Incoming::Response { success, .. } => assert!(!success),
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn roundtrip_through_frame() {
        // build -> serialize -> frame -> decode -> parse -> classify
        use crate::frame;
        let req = request(42, "setFunctionBreakpoints", parse(r#"{"breakpoints":[{"name":"main.f"}]}"#));
        let wire = frame::encode(req.to_string().as_bytes());

        let mut d = frame::Decoder::new();
        d.push(&wire);
        let body = d.next_frame().unwrap().expect("one frame");
        let parsed = Value::parse(&body).expect("valid json body");
        match classify(parsed).unwrap() {
            Incoming::Request { seq, command, arguments } => {
                // A locally-built request classifies as a Request (type=request);
                // over the wire this is what the SERVER receives from us.
                assert_eq!(seq, 42);
                assert_eq!(command, "setFunctionBreakpoints");
                let bps = arguments.get("breakpoints").unwrap().as_array().unwrap();
                assert_eq!(bps[0].get("name").unwrap().as_str(), Some("main.f"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }
}
