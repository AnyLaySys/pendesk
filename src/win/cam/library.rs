mod attributes;
mod shared;
mod source;
mod stream;

use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};
use windows::Win32::Foundation::{
    CLASS_E_CLASSNOTAVAILABLE, CLASS_E_NOAGGREGATION, E_POINTER, S_FALSE, S_OK,
};
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::{IClassFactory, IClassFactory_Impl};
use windows::core::{BOOL, GUID, HRESULT, IUnknown, Interface, PCWSTR, Ref, Result, implement, w};

static OBJECTS: AtomicUsize = AtomicUsize::new(0);
pub struct Lifetime;
impl Lifetime {
    fn new() -> Self {
        OBJECTS.fetch_add(1, Ordering::Relaxed);
        Self
    }
}
impl Drop for Lifetime {
    fn drop(&mut self) {
        OBJECTS.fetch_sub(1, Ordering::Release);
    }
}

#[implement(IClassFactory)]
struct Factory {
    _lifetime: Lifetime,
}

impl IClassFactory_Impl for Factory_Impl {
    fn CreateInstance(
        &self,
        outer: Ref<IUnknown>,
        iid: *const GUID,
        object: *mut *mut c_void,
    ) -> Result<()> {
        if object.is_null() || iid.is_null() {
            return Err(E_POINTER.into());
        }
        unsafe {
            *object = std::ptr::null_mut();
        }
        if outer.is_some() {
            return Err(CLASS_E_NOAGGREGATION.into());
        }
        let source = source::create()?;
        unsafe { source.query(iid, object).ok() }
    }
    fn LockServer(&self, lock: BOOL) -> Result<()> {
        if lock.as_bool() {
            OBJECTS.fetch_add(1, Ordering::Relaxed);
        } else {
            OBJECTS.fetch_sub(1, Ordering::Release);
        }
        Ok(())
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "system" fn DllGetClassObject(
    clsid: *const GUID,
    iid: *const GUID,
    object: *mut *mut c_void,
) -> HRESULT {
    if clsid.is_null() || iid.is_null() || object.is_null() {
        return E_POINTER;
    }
    unsafe {
        *object = std::ptr::null_mut();
        if *clsid != shared::CLSID {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        let factory: IClassFactory = Factory {
            _lifetime: Lifetime::new(),
        }
        .into();
        factory.query(iid, object)
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn DllCanUnloadNow() -> HRESULT {
    if OBJECTS.load(Ordering::Acquire) == 0 {
        S_OK
    } else {
        S_FALSE
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "system" fn CreateCamera(output: *mut *mut c_void) -> HRESULT {
    if output.is_null() {
        return E_POINTER;
    }
    unsafe {
        *output = std::ptr::null_mut();
    }
    if let Err(error) = unsafe { MFStartup(MF_VERSION, MFSTARTUP_FULL) } {
        return error.code();
    }
    let result = (|| unsafe {
        let id: Vec<u16> = format!("{{{:?}}}\0", shared::CLSID)
            .encode_utf16()
            .collect();
        let camera = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_AllUsers,
            w!("PenDesk"),
            PCWSTR(id.as_ptr()),
            None,
        )?;
        camera.Start(None)?;
        *output = camera.into_raw();
        Ok::<_, windows::core::Error>(())
    })();
    if result.is_err() {
        unsafe {
            let _ = MFShutdown();
        }
    }
    result.map_or_else(|e| e.code(), |_| S_OK)
}
