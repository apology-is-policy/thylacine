//! Filtered ordinary input. Physical events and secret input terminate in Lictor.
use alloc::vec::Vec;
use core::cell::RefCell;
use libdriver::Error;
use crate::{gpu_api::Request, rpc_client::Client};
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
        let (_, info): (_, Option<(bool, bool, u32, u32)>) = client.call(Request::InputInfo { index })?;
        let (abs, rel, x, y) = info.ok_or(Error::NoSuchResource)?;
        Ok(Self { client: RefCell::new(client), index, abs, rel, x, y })
    }
    pub fn supports_abs(&self) -> bool { self.abs }
    pub fn supports_rel(&self) -> bool { self.rel }
    pub fn abs_max(&self, axis: u8) -> u32 { if axis == 0 { self.x } else { self.y } }
    pub fn drain(&mut self, mut f: impl FnMut(RawInputEvent)) {
        let events: Result<(_, Vec<(u16, u16, u32)>), _> = self.client.borrow_mut().call(Request::InputDrain { index: self.index });
        if let Ok((_, events)) = events {
            for (etype, code, value) in events { f(RawInputEvent { etype, code, value }); }
        }
    }
}
