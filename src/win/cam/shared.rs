use windows::Win32::Foundation::{
    HANDLE, HLOCAL, INVALID_HANDLE_VALUE, LocalFree, WAIT_ABANDONED, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
use windows::Win32::Security::{PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES};
use windows::Win32::System::Memory::{
    CreateFileMappingW, FILE_MAP_ALL_ACCESS, FILE_MAP_READ, MEMORY_MAPPED_VIEW_ADDRESS,
    MapViewOfFile, OpenFileMappingW, PAGE_READWRITE, UnmapViewOfFile,
};
use windows::Win32::System::Threading::{
    CreateMutexW, MUTEX_MODIFY_STATE, OpenMutexW, ReleaseMutex, SYNCHRONIZATION_SYNCHRONIZE,
    WaitForSingleObject,
};
use windows::core::{Error, Owned, Result, w};

pub const CLSID: windows::core::GUID =
    windows::core::GUID::from_u128(0xb2dd287f_5f94_4e24_9d90_ca9d107b01f3);
const CAPACITY: usize = 8 * 1024 * 1024;
const SIZE: usize = CAPACITY + size_of::<Header>();

#[repr(C)]
struct Header {
    sequence: u64,
    length: u32,
    reserved: u32,
}

pub struct Shared {
    view: MEMORY_MAPPED_VIEW_ADDRESS,
    mutex: Owned<HANDLE>,
    _mapping: Owned<HANDLE>,
}

unsafe impl Send for Shared {}
unsafe impl Sync for Shared {}

impl Shared {
    pub fn open(create: bool) -> Result<Self> {
        unsafe {
            let (mapping, mutex) = if create {
                let mut descriptor = PSECURITY_DESCRIPTOR::default();
                ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    w!("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)"),
                    1,
                    &mut descriptor,
                    None,
                )?;
                let security = SECURITY_ATTRIBUTES {
                    nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: descriptor.0,
                    bInheritHandle: false.into(),
                };
                let result = (|| {
                    let mapping = Owned::new(CreateFileMappingW(
                        INVALID_HANDLE_VALUE,
                        Some(&security),
                        PAGE_READWRITE,
                        0,
                        SIZE as u32,
                        w!("Global\\PenDeskCameraFrame"),
                    )?);
                    let mutex = Owned::new(CreateMutexW(
                        Some(&security),
                        false,
                        w!("Global\\PenDeskCameraLock"),
                    )?);
                    Ok::<_, Error>((mapping, mutex))
                })();
                let _ = LocalFree(Some(HLOCAL(descriptor.0)));
                result?
            } else {
                (
                    Owned::new(OpenFileMappingW(
                        FILE_MAP_READ.0,
                        false,
                        w!("Global\\PenDeskCameraFrame"),
                    )?),
                    Owned::new(OpenMutexW(
                        MUTEX_MODIFY_STATE | SYNCHRONIZATION_SYNCHRONIZE,
                        false,
                        w!("Global\\PenDeskCameraLock"),
                    )?),
                )
            };
            let view = MapViewOfFile(
                *mapping,
                if create {
                    FILE_MAP_ALL_ACCESS
                } else {
                    FILE_MAP_READ
                },
                0,
                0,
                SIZE,
            );
            if view.Value.is_null() {
                return Err(Error::from_thread());
            }
            Ok(Self {
                view,
                mutex,
                _mapping: mapping,
            })
        }
    }

    fn access(&self, operation: impl FnOnce(*mut Header) -> Result<()>) -> Result<()> {
        unsafe {
            let wait = WaitForSingleObject(*self.mutex, 0);
            if wait == WAIT_TIMEOUT {
                return Ok(());
            }
            if wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED {
                return Err(Error::from_thread());
            }
            let result = operation(self.view.Value.cast());
            let _ = ReleaseMutex(*self.mutex);
            result
        }
    }

    pub fn publish(&self, frame: &[u8]) -> Result<()> {
        if frame.len() > CAPACITY {
            return Err(windows::Win32::Foundation::E_INVALIDARG.into());
        }
        self.access(|header| unsafe {
            std::ptr::copy_nonoverlapping(frame.as_ptr(), header.add(1).cast(), frame.len());
            (*header).length = frame.len() as u32;
            (*header).sequence = (*header).sequence.wrapping_add(1);
            Ok(())
        })
    }

    pub fn read(
        &self,
        previous: u64,
        receive: impl FnOnce(u64, &[u8]) -> Result<()>,
    ) -> Result<()> {
        self.access(|header| unsafe {
            let sequence = (*header).sequence;
            let length = (*header).length as usize;
            if sequence != previous && length <= CAPACITY {
                receive(
                    sequence,
                    std::slice::from_raw_parts(header.add(1).cast(), length),
                )?;
            }
            Ok(())
        })
    }
}

impl Drop for Shared {
    fn drop(&mut self) {
        unsafe {
            let _ = UnmapViewOfFile(self.view);
        }
    }
}
