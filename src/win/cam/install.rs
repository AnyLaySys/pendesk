use super::shared::CLSID;
use std::hash::{DefaultHasher, Hash, Hasher};
use std::path::PathBuf;
use windows::Win32::System::Registry::{
    HKEY, HKEY_LOCAL_MACHINE, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ, RegCreateKeyExW,
    RegSetValueExW,
};
use windows::core::{Owned, PCWSTR};

pub fn register() -> Result<PathBuf, String> {
    let source = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .with_file_name("pendesk_camera.dll");
    let bytes = std::fs::read(&source).map_err(|e| format!("{}: {e}", source.display()))?;
    let mut hash = DefaultHasher::new();
    bytes.hash(&mut hash);
    let directory =
        PathBuf::from(std::env::var_os("ProgramData").ok_or("ProgramData is unavailable")?)
            .join("PenDesk/camera");
    std::fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    let destination = directory.join(format!("camera-{:016x}.dll", hash.finish()));
    if !destination.exists() {
        std::fs::write(&destination, bytes).map_err(|e| e.to_string())?;
    }
    let path: Vec<u16> = format!("Software\\Classes\\CLSID\\{{{CLSID:?}}}\\InprocServer32\0")
        .encode_utf16()
        .collect();
    unsafe {
        let mut key = HKEY::default();
        RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            PCWSTR(path.as_ptr()),
            None,
            None,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            None,
            &mut key,
            None,
        )
        .ok()
        .map_err(|e| e.to_string())?;
        let key = Owned::new(key);
        for (name, value) in [
            ("", destination.to_string_lossy().into_owned()),
            ("ThreadingModel", "Both".into()),
        ] {
            let name: Vec<u16> = name.encode_utf16().chain([0]).collect();
            let value: Vec<u16> = value.encode_utf16().chain([0]).collect();
            let data = std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), value.len() * 2);
            RegSetValueExW(*key, PCWSTR(name.as_ptr()), None, REG_SZ, Some(data))
                .ok()
                .map_err(|e| e.to_string())?;
        }
    }
    Ok(destination)
}
