//! Role-checked terminal observations; binding IDs are locators, not credentials.

use crate::err::{Error, Result};
use crate::fs::File;
use crate::handle::Rights;
use crate::io::Read;
use crate::pty_interaction::*;

/// A well-formed locator; the kernel rechecks the calling process on every use.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BindingId(u64);

impl BindingId {
    pub fn from_locator(id: u64) -> Result<Self> {
        if id == 0 || id > T_PTY_INTERACTION_ID_MAX {
            return Err(Error::InvalidArgument);
        }
        Ok(Self(id))
    }

    pub fn locator(self) -> u64 {
        self.0
    }

    /// The binder must already be sealed; both descriptors are borrowed.
    pub fn bind(master_fd: i32, observer_fd: i32) -> Result<Self> {
        if master_fd < 0 || observer_fd < 0 {
            return Err(Error::BadHandle);
        }
        let id = call(
            T_PTY_INTERACTION_BIND,
            master_fd as u64,
            observer_fd as u64,
            0,
        )?;
        Self::from_locator(id as u64)
    }

    /// Revocation is explicit: dropping a copied locator must not revoke a seat.
    pub fn unbind(self) -> Result<()> {
        call(T_PTY_INTERACTION_UNBIND, self.0, 0, 0).map(|_| ())
    }

    pub fn state(self) -> Result<TPtyInteractionState> {
        let mut state = TPtyInteractionState::default();
        call(
            T_PTY_INTERACTION_STATE,
            self.0,
            (&mut state as *mut TPtyInteractionState) as u64,
            T_PTY_INTERACTION_STATE_BYTES,
        )?;
        Ok(state)
    }

    /// Only the observer may nominate; subject zero acknowledges APP mode.
    pub fn acknowledge(self, epoch: u64, subject_stripes: u64) -> Result<()> {
        self.request(T_PTY_INTERACTION_ACK, epoch, subject_stripes)
    }

    /// A successful check covers terminal ownership only, never graphical focus.
    pub fn check(self, epoch: u64, subject_stripes: u64) -> Result<()> {
        self.request(T_PTY_INTERACTION_CHECK, epoch, subject_stripes)
    }

    fn request(self, operation: u64, epoch: u64, subject_stripes: u64) -> Result<()> {
        let request = TPtyInteractionCheck {
            version: T_PTY_INTERACTION_VERSION as u32,
            size: T_PTY_INTERACTION_CHECK_BYTES as u32,
            expected_epoch: epoch,
            subject_stripes,
        };
        call(
            operation,
            self.0,
            (&request as *const TPtyInteractionCheck) as u64,
            T_PTY_INTERACTION_CHECK_BYTES,
        )
        .map(|_| ())
    }

    pub fn watch(self) -> Result<Watch> {
        let fd = call(T_PTY_INTERACTION_WATCH, self.0, 0, 0)?;
        // The kernel owns the handle range; do not truncate an unexpected result.
        let fd = i32::try_from(fd).map_err(|_| {
            unsafe {
                crate::t_close(fd);
            }
            Error::Io
        })?;
        Ok(Watch {
            file: unsafe { File::from_raw_fd(fd, Rights::READ) },
        })
    }
}

/// A bounded role-owned watcher; closing it does not revoke its binding.
pub struct Watch {
    file: File,
}

impl Watch {
    pub fn as_raw_fd(&self) -> i32 {
        self.file.as_raw_fd()
    }

    /// None is retirement; WouldBlock means no unread revision. Poll before retry.
    pub fn read(&mut self) -> Result<Option<TPtyInteractionState>> {
        let mut state = TPtyInteractionState::default();
        // The ABI asserts all offsets and has no padding or restricted bit patterns.
        let bytes = unsafe {
            core::slice::from_raw_parts_mut(
                (&mut state as *mut TPtyInteractionState).cast::<u8>(),
                T_PTY_INTERACTION_STATE_BYTES as usize,
            )
        };
        match self.file.read(bytes)? {
            0 => Ok(None),
            n if n == T_PTY_INTERACTION_STATE_BYTES as usize => Ok(Some(state)),
            _ => Err(Error::Io),
        }
    }
}

impl crate::poll::AsFd for Watch {
    fn as_raw_fd(&self) -> i32 {
        self.file.as_raw_fd()
    }
}

fn call(operation: u64, a1: u64, a2: u64, a3: u64) -> Result<i64> {
    Error::from_syscall_return(unsafe { crate::t_pty_register(operation, a1, a2, a3) })
}
