//! Filtered ordinary input. Physical events and secret input terminate in Lictor.
use core::cell::RefCell;
use libdriver::Error;
use crate::{gpu_api::{Drained, Request}, rpc_client::Client};
pub const INPUT_DMA_SIZE: usize = 4096;
pub const EV_SYN: u16 = 0;
pub const EV_KEY: u16 = 1;
pub const EV_REL: u16 = 2;
pub const EV_ABS: u16 = 3;
pub const ABS_X: u16 = 0;
pub const ABS_Y: u16 = 1;
pub const REL_X: u16 = 0;
pub const REL_Y: u16 = 1;
pub const REL_WHEEL: u16 = 8;
pub const BTN_LEFT: u16 = 0x110;
#[derive(Clone, Copy)]
pub struct RawInputEvent { pub etype: u16, pub code: u16, pub value: u32 }
pub struct InputDev { client: RefCell<Client>, index: u32, abs: bool, rel: bool, x: u32, y: u32 }
impl InputDev {
    pub fn probe(index: u32, _bar: u64, _dma: u64) -> Result<Self, Error> {
        let mut client = Client::connect()?;
        let info: Option<(bool, bool, u32, u32)> = client.call(Request::InputInfo { index })?;
        let (abs, rel, x, y) = info.ok_or(Error::NoSuchResource)?;
        Ok(Self { client: RefCell::new(client), index, abs, rel, x, y })
    }
    pub fn supports_abs(&self) -> bool { self.abs }
    pub fn supports_rel(&self) -> bool { self.rel }
    pub fn abs_max(&self, axis: u8) -> u32 { if axis == 0 { self.x } else { self.y } }
    /// The device's events since it was last drained, and the seat's
    /// generation and phase as the broker answered them in the same reply:
    /// the seat those events are of. `None` when the broker did not answer.
    #[must_use = "the seat a drain's events are of is dealt with before they are handed on"]
    pub fn drain(&mut self, mut f: impl FnMut(RawInputEvent)) -> Option<(u64, u32)> {
        let reply: Result<Drained, _> = self.client.borrow_mut().call(Request::InputDrain { index: self.index });
        let (generation, phase, events) = reply.ok()?;
        for (etype, code, value) in events { f(RawInputEvent { etype, code, value }); }
        Some((generation, phase))
    }
}
