use std::sync::Arc;
use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::{timeBeginPeriod, timeEndPeriod};
use windows::Win32::System::Threading::{CreateEventW, SetEvent, WaitForSingleObject};

pub struct Signal(HANDLE, bool);
unsafe impl Send for Signal {}
unsafe impl Sync for Signal {}

impl Signal {
    pub fn new() -> Result<Arc<Self>, String> {
        let handle =
            unsafe { CreateEventW(None, false, false, None) }.map_err(|error| error.to_string())?;
        Ok(Arc::new(Self(handle, unsafe { timeBeginPeriod(1) == 0 })))
    }

    pub fn set(&self) {
        unsafe {
            let _ = SetEvent(self.0);
        }
    }

    pub fn wait(&self, milliseconds: u32) -> bool {
        unsafe { WaitForSingleObject(self.0, milliseconds) == WAIT_OBJECT_0 }
    }
}

impl Drop for Signal {
    fn drop(&mut self) {
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.0);
            if self.1 {
                timeEndPeriod(1);
            }
        }
    }
}
