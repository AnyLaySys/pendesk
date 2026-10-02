use std::sync::Arc;
use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::{timeBeginPeriod, timeEndPeriod};
use windows::Win32::Networking::WinSock::{
    FD_READ, SOCKET, WSAEVENT, WSAEnumNetworkEvents, WSANETWORKEVENTS,
};
use windows::Win32::System::Threading::{CreateEventW, SetEvent, WaitForMultipleObjects};

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

    pub fn wait_socket(&self, socket: SOCKET, event: WSAEVENT, milliseconds: u32) -> bool {
        let handles = [self.0, HANDLE(event.0 as *mut core::ffi::c_void)];
        let result = unsafe { WaitForMultipleObjects(&handles, false, milliseconds) };
        if result.0 != WAIT_OBJECT_0.0 + 1 {
            return false;
        }
        let mut events = WSANETWORKEVENTS::default();
        unsafe {
            WSAEnumNetworkEvents(socket, event, &mut events) == 0
                && events.lNetworkEvents & FD_READ as i32 != 0
        }
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
