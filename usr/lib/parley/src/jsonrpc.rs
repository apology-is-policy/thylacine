//! The JSON-RPC 2.0 message model -- what LSP speaks.
//!
//! DAP is a *different* envelope (`seq`/`type`/`command`) and gets its own module
//! when the debug client lands; the Content-Length framing (`parley::frame`) and
//! the JSON codec (`parley::json`) are shared. This module is the message layer:
//! it builds outgoing requests/notifications and **classifies** an incoming
//! parsed value into the three things a client must react to:
//!
//!   - a **Response** to one of *our* requests (matched by the integer id we
//!     minted);
//!   - a server-initiated **Request** (e.g. `workspace/configuration`), which we
//!     must answer -- the id is echoed back verbatim;
//!   - a server **Notification** (e.g. `textDocument/publishDiagnostics`), which
//!     we act on but never answer.
//!
//! Id allocation + response matching (a counter + a pending map) belong to the
//! client layer (8e-2); this module is the pure protocol grammar.

use crate::json::Value;
use alloc::string::String;
use alloc::vec::Vec;

/// We mint integer request ids. (JSON-RPC also permits string ids; we never send
/// one, and a server *request*'s id is echoed verbatim via `Incoming::Request`.)
pub type Id = i64;

fn obj(pairs: Vec<(&str, Value)>) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (String::from(k), v)).collect())
}

/// Build an outgoing request: `{"jsonrpc":"2.0","id":id,"method":method,"params":params}`.
pub fn request(id: Id, method: &str, params: Value) -> Value {
    obj(alloc::vec![
        ("jsonrpc", Value::from("2.0")),
        ("id", Value::Int(id)),
        ("method", Value::from(method)),
        ("params", params),
    ])
}

/// Build an outgoing notification (no id, no response expected).
pub fn notification(method: &str, params: Value) -> Value {
    obj(alloc::vec![
        ("jsonrpc", Value::from("2.0")),
        ("method", Value::from(method)),
        ("params", params),
    ])
}

/// Build a success response to a server-initiated request (echo its raw id).
pub fn response(id: Value, result: Value) -> Value {
    obj(alloc::vec![("jsonrpc", Value::from("2.0")), ("id", id), ("result", result)])
}

/// Build an error response to a server-initiated request.
pub fn error_response(id: Value, code: i64, message: &str) -> Value {
    let err = obj(alloc::vec![("code", Value::Int(code)), ("message", Value::from(message))]);
    obj(alloc::vec![("jsonrpc", Value::from("2.0")), ("id", id), ("error", err)])
}

/// A JSON-RPC error object (the `error` member of a failed response).
#[derive(Clone, Debug, PartialEq)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
    pub data: Option<Value>,
}

/// A classified incoming message.
#[derive(Clone, Debug, PartialEq)]
pub enum Incoming {
    /// A reply to one of our requests.
    Response { id: Id, result: Result<Value, RpcError> },
    /// A server-initiated request we must answer (`id` echoed verbatim).
    Request { id: Value, method: String, params: Value },
    /// A server notification (no reply).
    Notification { method: String, params: Value },
}

/// Classify a parsed JSON value as a JSON-RPC message. Consumes the value (moves
/// its fields out -- no clone of a large `params`/`result`). The `jsonrpc: "2.0"`
/// member is not required to be present (gopls always sends it; we do not gate on
/// it, matching lenient LSP clients).
pub fn classify(v: Value) -> Result<Incoming, &'static str> {
    let mut kvs = match v {
        Value::Object(kvs) => kvs,
        _ => return Err("message is not a JSON object"),
    };
    let method = take_str(&mut kvs, "method");
    let id = take(&mut kvs, "id");
    match (method, id) {
        (Some(method), Some(id)) => Ok(Incoming::Request {
            id,
            method,
            params: take(&mut kvs, "params").unwrap_or(Value::Null),
        }),
        (Some(method), None) => Ok(Incoming::Notification {
            method,
            params: take(&mut kvs, "params").unwrap_or(Value::Null),
        }),
        (None, Some(id)) => {
            let id = id.as_i64().ok_or("response id is not an integer")?;
            if let Some(err) = take(&mut kvs, "error") {
                Ok(Incoming::Response { id, result: Err(parse_error(err)) })
            } else if let Some(res) = take(&mut kvs, "result") {
                Ok(Incoming::Response { id, result: Ok(res) })
            } else {
                Err("response has neither result nor error")
            }
        }
        (None, None) => Err("message has neither method nor id"),
    }
}

fn parse_error(v: Value) -> RpcError {
    RpcError {
        code: v.get("code").and_then(|c| c.as_i64()).unwrap_or(0),
        message: v.get("message").and_then(|m| m.as_str()).unwrap_or("").into(),
        data: v.get("data").cloned(),
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::Value;

    fn parse(s: &str) -> Value {
        Value::parse(s.as_bytes()).expect("valid json")
    }

    #[test]
    fn build_request() {
        let params = parse(r#"{"processId":1,"rootUri":"file:///w"}"#);
        let v = request(1, "initialize", params);
        assert_eq!(
            v.to_string(),
            r#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":1,"rootUri":"file:///w"}}"#
        );
    }

    #[test]
    fn build_notification() {
        let v = notification("initialized", Value::Object(alloc::vec![]));
        assert_eq!(v.to_string(), r#"{"jsonrpc":"2.0","method":"initialized","params":{}}"#);
    }

    #[test]
    fn build_response_and_error() {
        assert_eq!(
            response(Value::Int(7), Value::Bool(true)).to_string(),
            r#"{"jsonrpc":"2.0","id":7,"result":true}"#
        );
        assert_eq!(
            error_response(Value::Int(7), -32601, "method not found").to_string(),
            r#"{"jsonrpc":"2.0","id":7,"error":{"code":-32601,"message":"method not found"}}"#
        );
    }

    #[test]
    fn classify_response_result() {
        let v = parse(r#"{"jsonrpc":"2.0","id":3,"result":{"capabilities":{}}}"#);
        match classify(v).unwrap() {
            Incoming::Response { id, result } => {
                assert_eq!(id, 3);
                assert_eq!(result.unwrap().get("capabilities").unwrap().to_string(), "{}");
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_response_error() {
        let v = parse(r#"{"jsonrpc":"2.0","id":4,"error":{"code":-32602,"message":"bad params","data":[1,2]}}"#);
        match classify(v).unwrap() {
            Incoming::Response { id, result } => {
                assert_eq!(id, 4);
                let e = result.unwrap_err();
                assert_eq!(e.code, -32602);
                assert_eq!(e.message, "bad params");
                assert_eq!(e.data.unwrap().to_string(), "[1,2]");
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_notification() {
        let v = parse(r#"{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///x"}}"#);
        match classify(v).unwrap() {
            Incoming::Notification { method, params } => {
                assert_eq!(method, "textDocument/publishDiagnostics");
                assert_eq!(params.get("uri").unwrap().as_str(), Some("file:///x"));
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_server_request_echoes_id() {
        // a server-initiated request whose id we must echo verbatim
        let v = parse(r#"{"jsonrpc":"2.0","id":"cfg-1","method":"workspace/configuration","params":{}}"#);
        match classify(v).unwrap() {
            Incoming::Request { id, method, .. } => {
                assert_eq!(method, "workspace/configuration");
                assert_eq!(id, Value::Str("cfg-1".into())); // string id preserved
            }
            other => panic!("wrong: {:?}", other),
        }
    }

    #[test]
    fn classify_errors() {
        assert!(classify(Value::parse(b"[1,2]").unwrap()).is_err()); // not an object
        assert!(classify(Value::parse(b"{}").unwrap()).is_err()); // neither method nor id
        assert!(classify(parse(r#"{"id":1}"#)).is_err()); // response w/o result or error
        assert!(classify(parse(r#"{"id":"x","result":1}"#)).is_err()); // non-int response id
    }

    #[test]
    fn roundtrip_through_frame() {
        // build -> serialize -> frame -> decode -> parse -> classify
        use crate::frame;
        let req = request(42, "textDocument/definition", parse(r#"{"position":{"line":10}}"#));
        let wire = frame::encode(req.to_string().as_bytes());

        let mut d = frame::Decoder::new();
        d.push(&wire);
        let body = d.next().unwrap().expect("one frame");
        let parsed = Value::parse(&body).expect("valid json body");
        match classify(parsed).unwrap() {
            Incoming::Request { id, method, params } => {
                assert_eq!(id, Value::Int(42));
                assert_eq!(method, "textDocument/definition");
                assert_eq!(params.get("position").unwrap().get("line").unwrap().as_i64(), Some(10));
            }
            other => panic!("wrong: {:?}", other),
        }
    }
}
