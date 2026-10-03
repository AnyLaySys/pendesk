mod install;
mod shared;

pub use shared::Shared;
use std::sync::Arc;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Media::MediaFoundation::{IMFVirtualCamera, MFShutdown};
use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
use windows::core::{HRESULT, Interface, Owned, PCWSTR, s};

pub struct Camera {
    camera: Option<IMFVirtualCamera>,
    pub frames: Arc<Shared>,
    _library: Owned<HMODULE>,
}

impl Camera {
    pub fn new() -> Result<Self, String> {
        let path = install::register()?;
        let path: Vec<u16> = path
            .as_os_str()
            .to_string_lossy()
            .encode_utf16()
            .chain([0])
            .collect();
        unsafe {
            let library = Owned::new(
                LoadLibraryW(PCWSTR(path.as_ptr()))
                    .map_err(|e| format!("Windows 11 virtual camera: {e}"))?,
            );
            let create = GetProcAddress(*library, s!("CreateCamera"))
                .ok_or("virtual camera entry point is missing")?;
            let create: unsafe extern "system" fn(*mut *mut std::ffi::c_void) -> HRESULT =
                std::mem::transmute(create);
            let shared = Arc::new(Shared::open(true).map_err(|e| e.to_string())?);
            shared.publish(&[]).map_err(|e| e.to_string())?;
            let mut object = std::ptr::null_mut();
            create(&mut object).ok().map_err(|e| e.to_string())?;
            Ok(Self {
                camera: Some(IMFVirtualCamera::from_raw(object)),
                frames: shared,
                _library: library,
            })
        }
    }
}

impl Drop for Camera {
    fn drop(&mut self) {
        let _ = self.frames.publish(&[]);
        unsafe {
            if let Some(camera) = self.camera.take() {
                let _ = camera.Shutdown();
                drop(camera);
            }
            let _ = MFShutdown();
        }
    }
}
