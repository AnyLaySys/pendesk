use std::mem::size_of;
use std::os::windows::ffi::OsStrExt;
use std::slice;
use windows::Win32::Foundation::ERROR_FILE_NOT_FOUND;
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ, RegCloseKey,
    RegCreateKeyExW, RegDeleteValueW, RegOpenKeyExW, RegSetValueExW,
};
use windows::core::{PCWSTR, w};

const RUN: PCWSTR = w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const NAME: PCWSTR = w!("PenDesk");

pub fn set(enabled: bool) -> Result<(), String> {
    if enabled { enable() } else { disable() }
}

fn enable() -> Result<(), String> {
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
    let key = open_key()?;
    let result = unsafe { RegSetValueExW(key, NAME, None, REG_SZ, Some(data)) };
    unsafe {
        let _ = RegCloseKey(key);
    }
    result.ok().map_err(|error| error.to_string())
}

fn disable() -> Result<(), String> {
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

fn open_key() -> Result<HKEY, String> {
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
