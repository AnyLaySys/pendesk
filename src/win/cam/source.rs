use crate::{
    Lifetime,
    attributes::events,
    shared,
    stream::{self, Stream},
};
use std::sync::atomic::{AtomicBool, Ordering};
use windows::Win32::Foundation::{E_INVALIDARG, E_POINTER, ERROR_SET_NOT_FOUND, S_OK};
use windows::Win32::Media::KernelStreaming::{
    IKsControl, IKsControl_Impl, KSCAMERAPROFILE_Legacy, KSIDENTIFIER,
};
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::StructuredStorage::PROPVARIANT;
use windows::core::{
    ComObject, GUID, HRESULT, IUnknown, IUnknownImpl, Interface, Ref, Result, implement, w,
};

#[implement(
    IMFActivate,
    IMFMediaSourceEx,
    IMFGetService,
    IMFSampleAllocatorControl,
    IKsControl
)]
pub struct Source {
    pub attributes: IMFAttributes,
    pub events: IMFMediaEventQueue,
    descriptor: IMFPresentationDescriptor,
    stream: ComObject<Stream>,
    known: AtomicBool,
    shutdown: AtomicBool,
    _lifetime: Lifetime,
}

pub fn create() -> Result<IMFMediaSourceEx> {
    unsafe {
        let stream = ComObject::new(Stream::new()?);
        let mut attributes = None;
        MFCreateAttributes(&mut attributes, 4)?;
        let attributes = attributes.unwrap();
        attributes.SetGUID(&MFT_TRANSFORM_CLSID_Attribute, &shared::CLSID)?;
        let profiles = MFCreateSensorProfileCollection()?;
        let profile = MFCreateSensorProfile(&KSCAMERAPROFILE_Legacy, 0, None)?;
        profile.AddProfileFilter(0, w!("((RES==;FRT<=30,1;SUT==))"))?;
        profiles.AddProfile(&profile)?;
        attributes.SetUnknown(&MF_DEVICEMFT_SENSORPROFILE_COLLECTION, &profiles)?;
        let descriptor = MFCreatePresentationDescriptor(Some(&[Some(stream.descriptor.clone())]))?;
        descriptor.SelectStream(0)?;
        let source: IMFMediaSourceEx = Source {
            attributes,
            events: MFCreateEventQueue()?,
            descriptor,
            stream: stream.clone(),
            known: AtomicBool::new(false),
            shutdown: AtomicBool::new(false),
            _lifetime: Lifetime::new(),
        }
        .into();
        *stream.source.lock().unwrap() = source.cast::<IMFMediaSource>()?.downgrade()?;
        Ok(source)
    }
}

impl Source {
    fn check(&self) -> Result<()> {
        if self.shutdown.load(Ordering::Acquire) {
            Err(MF_E_SHUTDOWN.into())
        } else {
            Ok(())
        }
    }
}

events!(Source_Impl);

impl IMFActivate_Impl for Source_Impl {
    fn ActivateObject(&self, iid: *const GUID, object: *mut *mut std::ffi::c_void) -> Result<()> {
        self.check()?;
        if iid.is_null() || object.is_null() {
            return Err(E_POINTER.into());
        }
        unsafe {
            self.to_interface::<IMFMediaSourceEx>()
                .query(iid, object)
                .ok()
        }
    }
    fn ShutdownObject(&self) -> Result<()> {
        IMFMediaSource_Impl::Shutdown(self)
    }
    fn DetachObject(&self) -> Result<()> {
        Ok(())
    }
}

impl IMFMediaSource_Impl for Source_Impl {
    fn GetCharacteristics(&self) -> Result<u32> {
        self.check()?;
        Ok(MFMEDIASOURCE_IS_LIVE.0 as u32)
    }
    fn CreatePresentationDescriptor(&self) -> Result<IMFPresentationDescriptor> {
        self.check()?;
        unsafe { self.descriptor.Clone() }
    }
    fn Start(
        &self,
        descriptor: Ref<IMFPresentationDescriptor>,
        time: *const GUID,
        position: *const PROPVARIANT,
    ) -> Result<()> {
        self.check()?;
        if position.is_null() {
            return Err(E_POINTER.into());
        }
        unsafe {
            if !time.is_null() && *time != GUID::zeroed() {
                return Err(MF_E_UNSUPPORTED_TIME_FORMAT.into());
            }
            let descriptor = descriptor.ok()?;
            if descriptor.GetStreamDescriptorCount()? != 1 {
                return Err(E_INVALIDARG.into());
            }
            let mut selected = false.into();
            let mut stream = None;
            descriptor.GetStreamDescriptorByIndex(0, &mut selected, &mut stream)?;
            if !selected.as_bool() {
                return Err(E_INVALIDARG.into());
            }
            let stream = stream.unwrap();
            let media = stream.GetMediaTypeHandler()?.GetCurrentMediaType()?;
            if stream.GetStreamIdentifier()? != 0
                || media.GetGUID(&MF_MT_SUBTYPE)? != MFVideoFormat_MJPG
                || media.GetUINT64(&MF_MT_FRAME_SIZE)?
                    != (u64::from(stream::WIDTH) << 32 | u64::from(stream::HEIGHT))
            {
                return Err(MF_E_INVALIDMEDIATYPE.into());
            }
            let event = if self.known.swap(true, Ordering::AcqRel) {
                MEUpdatedStream
            } else {
                MENewStream
            };
            self.events.QueueEventParamUnk(
                event.0 as u32,
                &GUID::zeroed(),
                S_OK,
                &self.stream.to_interface::<IMFMediaStream2>(),
            )?;
            self.stream.start()?;
            self.events.QueueEventParamVar(
                MESourceStarted.0 as u32,
                &GUID::zeroed(),
                S_OK,
                &PROPVARIANT::from(MFGetSystemTime()),
            )
        }
    }
    fn Stop(&self) -> Result<()> {
        self.check()?;
        self.stream.stop()?;
        unsafe {
            self.events.QueueEventParamVar(
                MESourceStopped.0 as u32,
                &GUID::zeroed(),
                S_OK,
                std::ptr::null(),
            )
        }
    }
    fn Pause(&self) -> Result<()> {
        Err(MF_E_INVALID_STATE_TRANSITION.into())
    }
    fn Shutdown(&self) -> Result<()> {
        if !self.shutdown.swap(true, Ordering::AcqRel) {
            self.stream.shutdown();
            unsafe {
                self.events.Shutdown()?;
            }
        }
        Ok(())
    }
}

impl IMFMediaSourceEx_Impl for Source_Impl {
    fn GetSourceAttributes(&self) -> Result<IMFAttributes> {
        self.check()?;
        Ok(self.attributes.clone())
    }
    fn GetStreamAttributes(&self, stream: u32) -> Result<IMFAttributes> {
        self.check()?;
        if stream != 0 {
            return Err(MF_E_INVALIDSTREAMNUMBER.into());
        }
        self.stream.descriptor.cast()
    }
    fn SetD3DManager(&self, _manager: Ref<IUnknown>) -> Result<()> {
        self.check()
    }
}

impl IMFGetService_Impl for Source_Impl {
    fn GetService(
        &self,
        _service: *const GUID,
        _iid: *const GUID,
        object: *mut *mut std::ffi::c_void,
    ) -> Result<()> {
        if object.is_null() {
            return Err(E_POINTER.into());
        }
        unsafe {
            *object = std::ptr::null_mut();
        }
        Err(MF_E_UNSUPPORTED_SERVICE.into())
    }
}

impl IMFSampleAllocatorControl_Impl for Source_Impl {
    fn SetDefaultAllocator(&self, stream: u32, _allocator: Ref<IUnknown>) -> Result<()> {
        if stream == 0 {
            self.check()
        } else {
            Err(MF_E_INVALIDSTREAMNUMBER.into())
        }
    }
    fn GetAllocatorUsage(
        &self,
        stream: u32,
        input: *mut u32,
        usage: *mut MFSampleAllocatorUsage,
    ) -> Result<()> {
        if input.is_null() || usage.is_null() {
            return Err(E_POINTER.into());
        }
        if stream != 0 {
            return Err(MF_E_INVALIDSTREAMNUMBER.into());
        }
        unsafe {
            *input = 0;
            *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
        }
        self.check()
    }
}

impl IKsControl_Impl for Source_Impl {
    fn KsProperty(
        &self,
        _property: *const KSIDENTIFIER,
        _length: u32,
        _data: *mut std::ffi::c_void,
        _size: u32,
        returned: *mut u32,
    ) -> Result<()> {
        unsupported(returned)
    }
    fn KsMethod(
        &self,
        _method: *const KSIDENTIFIER,
        _length: u32,
        _data: *mut std::ffi::c_void,
        _size: u32,
        returned: *mut u32,
    ) -> Result<()> {
        unsupported(returned)
    }
    fn KsEvent(
        &self,
        _event: *const KSIDENTIFIER,
        _length: u32,
        _data: *mut std::ffi::c_void,
        _size: u32,
        returned: *mut u32,
    ) -> Result<()> {
        unsupported(returned)
    }
}

fn unsupported(returned: *mut u32) -> Result<()> {
    if returned.is_null() {
        return Err(E_POINTER.into());
    }
    unsafe {
        *returned = 0;
    }
    Err(HRESULT::from_win32(ERROR_SET_NOT_FOUND.0).into())
}
