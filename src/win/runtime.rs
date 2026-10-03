use crate::all::cmd::{self as cmd, Command, Config};
use crate::cfg;
use crate::elevation;
use crate::server;
use crate::startup;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{
    CloseHandle, ERROR_ALREADY_EXISTS, GetLastError, HANDLE, WAIT_OBJECT_0,
};
use windows::Win32::System::Threading::{
    CreateEventW, CreateMutexW, EVENT_MODIFY_STATE, OpenEventW, ResetEvent, SetEvent,
    WaitForSingleObject,
};
use windows::core::w;
const HOST: windows::core::PCWSTR = w!("Local\\PenDeskHost");
const STOP: windows::core::PCWSTR = w!("Local\\PenDeskStop");
pub fn dispatch() -> Result<(), String> {
    match cmd::parse()? {
        Command::Run { config, wait } => run(config, wait),
        Command::Install { serial } => crate::setup::install(serial),
        Command::Configure(setup) => crate::setup::configure(setup),
        Command::Restart => restart(),
        Command::Stop => stop(),
        Command::Startup(enabled) => startup::set(enabled),
        Command::Help => {
            println!("{}", cmd::usage());
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
    let camera = crate::cam::Camera::new()
        .map_err(|error| eprintln!("Camera: {error}"))
        .ok();
    while !stop.signaled() {
        let _ = server::run(
            config.clone(),
            || stop.signaled(),
            camera.as_ref().map(|camera| Arc::clone(&camera.frames)),
        );
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
