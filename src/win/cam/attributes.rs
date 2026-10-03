use crate::source::Source_Impl;
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::StructuredStorage::PROPVARIANT;
use windows::core::{BOOL, GUID, IUnknown, Interface, PCWSTR, PWSTR, Ref, Result};

impl IMFAttributes_Impl for Source_Impl {
    fn GetItem(&self, key: *const GUID, value: *mut PROPVARIANT) -> Result<()> {
        unsafe { self.attributes.GetItem(key, Some(value)) }
    }
    fn GetItemType(&self, key: *const GUID) -> Result<MF_ATTRIBUTE_TYPE> {
        unsafe { self.attributes.GetItemType(key) }
    }
    fn CompareItem(&self, key: *const GUID, value: *const PROPVARIANT) -> Result<BOOL> {
        unsafe { self.attributes.CompareItem(key, value) }
    }
    fn Compare(&self, other: Ref<IMFAttributes>, kind: MF_ATTRIBUTES_MATCH_TYPE) -> Result<BOOL> {
        unsafe { self.attributes.Compare(other.as_ref(), kind) }
    }
    fn GetUINT32(&self, key: *const GUID) -> Result<u32> {
        unsafe { self.attributes.GetUINT32(key) }
    }
    fn GetUINT64(&self, key: *const GUID) -> Result<u64> {
        unsafe { self.attributes.GetUINT64(key) }
    }
    fn GetDouble(&self, key: *const GUID) -> Result<f64> {
        unsafe { self.attributes.GetDouble(key) }
    }
    fn GetGUID(&self, key: *const GUID) -> Result<GUID> {
        unsafe { self.attributes.GetGUID(key) }
    }
    fn GetStringLength(&self, key: *const GUID) -> Result<u32> {
        unsafe { self.attributes.GetStringLength(key) }
    }
    fn GetString(&self, key: *const GUID, value: PWSTR, size: u32, length: *mut u32) -> Result<()> {
        unsafe {
            (self.attributes.vtable().GetString)(self.attributes.as_raw(), key, value, size, length)
                .ok()
        }
    }
    fn GetAllocatedString(
        &self,
        key: *const GUID,
        value: *mut PWSTR,
        length: *mut u32,
    ) -> Result<()> {
        unsafe { self.attributes.GetAllocatedString(key, value, length) }
    }
    fn GetBlobSize(&self, key: *const GUID) -> Result<u32> {
        unsafe { self.attributes.GetBlobSize(key) }
    }
    fn GetBlob(&self, key: *const GUID, value: *mut u8, size: u32, length: *mut u32) -> Result<()> {
        unsafe {
            (self.attributes.vtable().GetBlob)(self.attributes.as_raw(), key, value, size, length)
                .ok()
        }
    }
    fn GetAllocatedBlob(
        &self,
        key: *const GUID,
        value: *mut *mut u8,
        size: *mut u32,
    ) -> Result<()> {
        unsafe { self.attributes.GetAllocatedBlob(key, value, size) }
    }
    fn GetUnknown(
        &self,
        key: *const GUID,
        iid: *const GUID,
        object: *mut *mut std::ffi::c_void,
    ) -> Result<()> {
        unsafe {
            (self.attributes.vtable().GetUnknown)(self.attributes.as_raw(), key, iid, object).ok()
        }
    }
    fn SetItem(&self, key: *const GUID, value: *const PROPVARIANT) -> Result<()> {
        unsafe { self.attributes.SetItem(key, value) }
    }
    fn DeleteItem(&self, key: *const GUID) -> Result<()> {
        unsafe { self.attributes.DeleteItem(key) }
    }
    fn DeleteAllItems(&self) -> Result<()> {
        unsafe { self.attributes.DeleteAllItems() }
    }
    fn SetUINT32(&self, key: *const GUID, value: u32) -> Result<()> {
        unsafe { self.attributes.SetUINT32(key, value) }
    }
    fn SetUINT64(&self, key: *const GUID, value: u64) -> Result<()> {
        unsafe { self.attributes.SetUINT64(key, value) }
    }
    fn SetDouble(&self, key: *const GUID, value: f64) -> Result<()> {
        unsafe { self.attributes.SetDouble(key, value) }
    }
    fn SetGUID(&self, key: *const GUID, value: *const GUID) -> Result<()> {
        unsafe { self.attributes.SetGUID(key, value) }
    }
    fn SetString(&self, key: *const GUID, value: &PCWSTR) -> Result<()> {
        unsafe { self.attributes.SetString(key, *value) }
    }
    fn SetBlob(&self, key: *const GUID, value: *const u8, size: u32) -> Result<()> {
        unsafe {
            (self.attributes.vtable().SetBlob)(self.attributes.as_raw(), key, value, size).ok()
        }
    }
    fn SetUnknown(&self, key: *const GUID, value: Ref<IUnknown>) -> Result<()> {
        unsafe { self.attributes.SetUnknown(key, value.as_ref()) }
    }
    fn LockStore(&self) -> Result<()> {
        unsafe { self.attributes.LockStore() }
    }
    fn UnlockStore(&self) -> Result<()> {
        unsafe { self.attributes.UnlockStore() }
    }
    fn GetCount(&self) -> Result<u32> {
        unsafe { self.attributes.GetCount() }
    }
    fn GetItemByIndex(&self, index: u32, key: *mut GUID, value: *mut PROPVARIANT) -> Result<()> {
        unsafe { self.attributes.GetItemByIndex(index, key, Some(value)) }
    }
    fn CopyAllItems(&self, other: Ref<IMFAttributes>) -> Result<()> {
        unsafe { self.attributes.CopyAllItems(other.as_ref()) }
    }
}

macro_rules! events {
    ($implementation:ty) => {
        impl IMFMediaEventGenerator_Impl for $implementation {
            fn GetEvent(
                &self,
                flags: MEDIA_EVENT_GENERATOR_GET_EVENT_FLAGS,
            ) -> Result<IMFMediaEvent> {
                unsafe { self.events.GetEvent(flags.0 as u32) }
            }
            fn BeginGetEvent(
                &self,
                callback: Ref<IMFAsyncCallback>,
                state: Ref<IUnknown>,
            ) -> Result<()> {
                unsafe { self.events.BeginGetEvent(callback.as_ref(), state.as_ref()) }
            }
            fn EndGetEvent(&self, result: Ref<IMFAsyncResult>) -> Result<IMFMediaEvent> {
                unsafe { self.events.EndGetEvent(result.as_ref()) }
            }
            fn QueueEvent(
                &self,
                kind: u32,
                extended: *const GUID,
                status: HRESULT,
                value: *const PROPVARIANT,
            ) -> Result<()> {
                unsafe {
                    self.events
                        .QueueEventParamVar(kind, extended, status, value)
                }
            }
        }
    };
}
pub(crate) use events;
