use std::ffi::{OsStr, OsString};
use std::iter;
use std::mem::size_of;
use std::os::windows::ffi::OsStrExt;
use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::Security::{GetTokenInformation, TOKEN_ELEVATION, TOKEN_QUERY, TokenElevation};
use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
use windows::Win32::UI::Shell::ShellExecuteW;
use windows::Win32::UI::WindowsAndMessaging::SW_HIDE;
use windows::core::PCWSTR;
pub fn ensure() -> Result<bool, String> {
    if elevated()? {
        Ok(true)
    } else {
        launch(std::env::args_os().skip(1))?;
        Ok(false)
    }
}
pub fn launch(arguments: impl IntoIterator<Item = OsString>) -> Result<(), String> {
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let directory = executable
        .parent()
        .unwrap_or_else(|| std::path::Path::new(""));
    let operation = wide(OsStr::new("runas"));
    let executable = wide(executable.as_os_str());
    let directory = wide(directory.as_os_str());
    let parameters = command_line(arguments);
    let result = unsafe {
        ShellExecuteW(
            None,
            PCWSTR(operation.as_ptr()),
            PCWSTR(executable.as_ptr()),
            PCWSTR(parameters.as_ptr()),
            PCWSTR(directory.as_ptr()),
            SW_HIDE,
        )
    };
    if result.0 as usize <= 32 {
        Err("could not start PenDesk as administrator".into())
    } else {
        Ok(())
    }
}
fn elevated() -> Result<bool, String> {
    let mut token = HANDLE::default();
    unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) }
        .map_err(|error| error.to_string())?;
    let mut elevation = TOKEN_ELEVATION::default();
    let mut length = 0;
    let result = unsafe {
        GetTokenInformation(
            token,
            TokenElevation,
            Some((&mut elevation as *mut TOKEN_ELEVATION).cast()),
            size_of::<TOKEN_ELEVATION>() as u32,
            &mut length,
        )
    };
    unsafe {
        let _ = CloseHandle(token);
    }
    result.map_err(|error| error.to_string())?;
    Ok(elevation.TokenIsElevated != 0)
}
fn command_line(arguments: impl IntoIterator<Item = OsString>) -> Vec<u16> {
    let mut command = Vec::new();
    for argument in arguments {
        if !command.is_empty() {
            command.push(b' ' as u16);
        }
        quote(&mut command, argument.as_os_str());
    }
    command.push(0);
    command
}
fn quote(command: &mut Vec<u16>, argument: &OsStr) {
    let argument = argument.encode_wide().collect::<Vec<_>>();
    if !argument.is_empty() && !argument.iter().any(|value| matches!(*value, 9 | 32 | 34)) {
        command.extend(argument);
        return;
    }
    command.push(b'"' as u16);
    let mut slashes = 0;
    for value in argument {
        if value == b'\\' as u16 {
            slashes += 1;
        } else if value == b'"' as u16 {
            command.extend(iter::repeat_n(b'\\' as u16, slashes * 2 + 1));
            command.push(value);
            slashes = 0;
        } else {
            command.extend(iter::repeat_n(b'\\' as u16, slashes));
            command.push(value);
            slashes = 0;
        }
    }
    command.extend(iter::repeat_n(b'\\' as u16, slashes * 2));
    command.push(b'"' as u16);
}
fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(iter::once(0)).collect()
}
