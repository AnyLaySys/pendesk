use crate::all::args::{self, Command, Config};
use crate::cfg;
use crate::elevation;
use crate::server;
use std::mem::size_of;
use std::os::windows::ffi::OsStrExt;
use std::slice;
use std::thread;
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{
    CloseHandle, ERROR_ALREADY_EXISTS, ERROR_FILE_NOT_FOUND, GetLastError, HANDLE, WAIT_OBJECT_0,
};
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ, RegCloseKey,
    RegCreateKeyExW, RegDeleteValueW, RegOpenKeyExW, RegSetValueExW,
};
use windows::Win32::System::Threading::{
    CreateEventW, CreateMutexW, EVENT_MODIFY_STATE, OpenEventW, ResetEvent, SetEvent,
    WaitForSingleObject,
};
use windows::core::{PCWSTR, w};
const HOST: windows::core::PCWSTR = w!("Local\\PenDeskHost");
const STOP: windows::core::PCWSTR = w!("Local\\PenDeskStop");
const RUN: windows::core::PCWSTR = w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const NAME: windows::core::PCWSTR = w!("PenDesk");
pub fn dispatch() -> Result<(), String> {
    match args::parse()? {
        Command::Run { config, wait } => run(config, wait),
        Command::Restart => restart(),
        Command::Stop => stop(),
        Command::Startup(enabled) => startup(enabled),
        Command::Help => {
            println!("{}", args::usage());
            Ok(())
        }
    }
}
pub fn run(config: Option<Config>, wait: bool) -> Result<(), String> {
    let config = config.map_or_else(cfg::stored, Ok)?;
    if !elevation::ensure()? {
        return Ok(());
    }
    let Some(_instance) = Instance::acquire(wait)? else {
        return Ok(());
    };
    let stop = Stop::new()?;
    while !stop.signaled() {
        let _ = server::run(config.clone(), || stop.signaled());
        if stop.wait(5_000) {
            break;
        }
    }
    Ok(())
}
pub fn restart() -> Result<(), String> {
    let _ = cfg::stored()?;
    stop()?;
    elevation::launch(["run".into()])
}
pub fn stop() -> Result<(), String> {
    let Ok(event) = (unsafe { OpenEventW(EVENT_MODIFY_STATE, false, STOP) }) else {
        return Ok(());
    };
    let result = unsafe { SetEvent(event) };
    unsafe {
        let _ = CloseHandle(event);
    }
    result.map_err(|error| error.to_string())
}
pub fn startup(enabled: bool) -> Result<(), String> {
    if enabled {
        enable_startup()
    } else {
        disable_startup()
    }
}
struct Instance(HANDLE);
impl Instance {
    fn acquire(wait: bool) -> Result<Option<Self>, String> {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let handle =
                unsafe { CreateMutexW(None, false, HOST) }.map_err(|error| error.to_string())?;
            if unsafe { GetLastError() } != ERROR_ALREADY_EXISTS {
                return Ok(Some(Self(handle)));
            }
            unsafe {
                let _ = CloseHandle(handle);
            }
            if !wait || Instant::now() >= deadline {
                return Ok(None);
            }
            thread::sleep(Duration::from_millis(100));
        }
    }
}
impl Drop for Instance {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}
struct Stop(HANDLE);
impl Stop {
    fn new() -> Result<Self, String> {
        let event =
            unsafe { CreateEventW(None, true, false, STOP) }.map_err(|error| error.to_string())?;
        unsafe { ResetEvent(event) }.map_err(|error| error.to_string())?;
        Ok(Self(event))
    }
    fn signaled(&self) -> bool {
        (unsafe { WaitForSingleObject(self.0, 0) }) == WAIT_OBJECT_0
    }
    fn wait(&self, milliseconds: u32) -> bool {
        (unsafe { WaitForSingleObject(self.0, milliseconds) }) == WAIT_OBJECT_0
    }
}
impl Drop for Stop {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}
fn enable_startup() -> Result<(), String> {
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let mut command = vec![b'"' as u16];
    command.extend(executable.as_os_str().encode_wide());
    command.extend("\" run".encode_utf16());
    command.push(0);
    let data = unsafe {
        slice::from_raw_parts(
            command.as_ptr().cast::<u8>(),
            command.len() * size_of::<u16>(),
        )
    };
    let key = open_run_key()?;
    let result = unsafe { RegSetValueExW(key, NAME, None, REG_SZ, Some(data)) };
    unsafe {
        let _ = RegCloseKey(key);
    }
    result.ok().map_err(|error| error.to_string())
}
fn disable_startup() -> Result<(), String> {
    let mut key = HKEY::default();
    let result = unsafe { RegOpenKeyExW(HKEY_CURRENT_USER, RUN, None, KEY_SET_VALUE, &mut key) };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(());
    }
    result.ok().map_err(|error| error.to_string())?;
    let result = unsafe { RegDeleteValueW(key, NAME) };
    unsafe {
        let _ = RegCloseKey(key);
    }
    if result == ERROR_FILE_NOT_FOUND {
        Ok(())
    } else {
        result.ok().map_err(|error| error.to_string())
    }
}
fn open_run_key() -> Result<HKEY, String> {
    let mut key = HKEY::default();
    unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            RUN,
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            None,
            &mut key,
            None,
        )
    }
    .ok()
    .map_err(|error| error.to_string())?;
    Ok(key)
}
