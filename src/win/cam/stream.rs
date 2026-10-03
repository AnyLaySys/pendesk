use crate::{Lifetime, attributes::events, shared::Shared};
use std::sync::Mutex;
use windows::Win32::Foundation::{E_NOTIMPL, S_OK};
use windows::Win32::Media::KernelStreaming::PINNAME_VIDEO_CAPTURE;
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::StructuredStorage::PROPVARIANT;
use windows::core::{
    GUID, HRESULT, IUnknown, IUnknownImpl, Interface, Ref, Result, Weak, implement,
};

pub const WIDTH: u32 = 1280;
pub const HEIGHT: u32 = 720;
pub const FPS: u32 = 30;
const PERIOD: i64 = 10_000_000 / FPS as i64;
const IDLE: &[u8] = include_bytes!("idle.jpg");
const GENERATION: GUID = GUID::from_u128(0x7d955882_f062_41ad_8d9a_ed045fd0dd3a);

struct State {
    status: MF_STREAM_STATE,
    generation: u32,
    next: i64,
    shared: Option<Shared>,
    sequence: u64,
    buffer: Option<IMFMediaBuffer>,
    shutdown: bool,
}

#[implement(IMFMediaStream2, IMFAsyncCallback)]
pub struct Stream {
    pub descriptor: IMFStreamDescriptor,
    pub events: IMFMediaEventQueue,
    pub source: Mutex<Weak<IMFMediaSource>>,
    state: Mutex<State>,
    _lifetime: Lifetime,
}

impl Stream {
    pub fn new() -> Result<Self> {
        unsafe {
            let media = MFCreateMediaType()?;
            media.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            media.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_MJPG)?;
            media.SetUINT64(
                &MF_MT_FRAME_SIZE,
                u64::from(WIDTH) << 32 | u64::from(HEIGHT),
            )?;
            media.SetUINT64(&MF_MT_FRAME_RATE, u64::from(FPS) << 32 | 1)?;
            media.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, 1u64 << 32 | 1)?;
            media.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            media.SetUINT32(&MF_MT_ALL_SAMPLES_INDEPENDENT, 1)?;
            media.SetUINT32(&MF_MT_FIXED_SIZE_SAMPLES, 0)?;
            let descriptor = MFCreateStreamDescriptor(0, &[Some(media.clone())])?;
            descriptor
                .GetMediaTypeHandler()?
                .SetCurrentMediaType(&media)?;
            descriptor.SetGUID(&MF_DEVICESTREAM_STREAM_CATEGORY, &PINNAME_VIDEO_CAPTURE)?;
            descriptor.SetUINT32(&MF_DEVICESTREAM_STREAM_ID, 0)?;
            descriptor.SetUINT32(&MF_DEVICESTREAM_FRAMESERVER_SHARED, 1)?;
            descriptor.SetUINT32(
                &MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                MFFrameSourceTypes_Color.0 as u32,
            )?;
            Ok(Self {
                descriptor,
                events: MFCreateEventQueue()?,
                source: Mutex::new(Weak::new()),
                state: Mutex::new(State {
                    status: MF_STREAM_STATE_STOPPED,
                    generation: 0,
                    next: 0,
                    shared: None,
                    sequence: 0,
                    buffer: Some(frame_buffer(IDLE)?),
                    shutdown: false,
                }),
                _lifetime: Lifetime::new(),
            })
        }
    }
    pub fn start(&self) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        if state.shutdown {
            return Err(MF_E_SHUTDOWN.into());
        }
        state.generation = state.generation.wrapping_add(1);
        state.status = MF_STREAM_STATE_RUNNING;
        unsafe {
            state.next = MFGetSystemTime();
            self.events.QueueEventParamVar(
                MEStreamStarted.0 as u32,
                &GUID::zeroed(),
                S_OK,
                &PROPVARIANT::from(state.next),
            )
        }
    }
    pub fn stop(&self) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        if state.shutdown {
            return Err(MF_E_SHUTDOWN.into());
        }
        state.status = MF_STREAM_STATE_STOPPED;
        state.generation = state.generation.wrapping_add(1);
        unsafe {
            self.events.QueueEventParamVar(
                MEStreamStopped.0 as u32,
                &GUID::zeroed(),
                S_OK,
                std::ptr::null(),
            )
        }
    }
    pub fn shutdown(&self) {
        let mut state = self.state.lock().unwrap();
        state.shutdown = true;
        state.buffer = None;
        state.shared = None;
        unsafe {
            let _ = self.events.Shutdown();
        }
        *self.source.lock().unwrap() = Weak::new();
    }
}

events!(Stream_Impl);

impl IMFMediaStream_Impl for Stream_Impl {
    fn GetMediaSource(&self) -> Result<IMFMediaSource> {
        self.source
            .lock()
            .unwrap()
            .upgrade()
            .ok_or_else(|| MF_E_SHUTDOWN.into())
    }
    fn GetStreamDescriptor(&self) -> Result<IMFStreamDescriptor> {
        if self.state.lock().unwrap().shutdown {
            Err(MF_E_SHUTDOWN.into())
        } else {
            Ok(self.descriptor.clone())
        }
    }
    fn RequestSample(&self, token: Ref<IUnknown>) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        if state.shutdown {
            return Err(MF_E_SHUTDOWN.into());
        }
        if state.status != MF_STREAM_STATE_RUNNING {
            return Err(MF_E_INVALIDREQUEST.into());
        }
        unsafe {
            let sample = MFCreateSample()?;
            if let Some(token) = token.as_ref() {
                sample.SetUnknown(&MFSampleExtension_Token, token)?;
            }
            sample.SetUINT32(&GENERATION, state.generation)?;
            let now = MFGetSystemTime();
            let due = state.next.max(now);
            state.next = due + PERIOD;
            MFScheduleWorkItem(
                &self.to_interface::<IMFAsyncCallback>(),
                &sample,
                -((due - now + 9999) / 10000).max(1),
                None,
            )
        }
    }
}

impl IMFMediaStream2_Impl for Stream_Impl {
    fn GetStreamState(&self) -> Result<MF_STREAM_STATE> {
        let state = self.state.lock().unwrap();
        if state.shutdown {
            Err(MF_E_SHUTDOWN.into())
        } else {
            Ok(state.status)
        }
    }
    fn SetStreamState(&self, value: MF_STREAM_STATE) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        if state.shutdown {
            return Err(MF_E_SHUTDOWN.into());
        }
        if value != MF_STREAM_STATE_RUNNING
            && value != MF_STREAM_STATE_STOPPED
            && value != MF_STREAM_STATE_PAUSED
        {
            return Err(MF_E_INVALID_STATE_TRANSITION.into());
        }
        if state.status != value {
            state.generation = state.generation.wrapping_add(1);
        }
        state.status = value;
        Ok(())
    }
}

impl IMFAsyncCallback_Impl for Stream_Impl {
    fn GetParameters(&self, _flags: *mut u32, _queue: *mut u32) -> Result<()> {
        Err(E_NOTIMPL.into())
    }
    fn Invoke(&self, result: Ref<IMFAsyncResult>) -> Result<()> {
        unsafe {
            let sample: IMFSample = result.ok()?.GetState()?.cast()?;
            let mut state = self.state.lock().unwrap();
            if state.shutdown
                || state.status != MF_STREAM_STATE_RUNNING
                || sample.GetUINT32(&GENERATION)? != state.generation
            {
                return Ok(());
            }
            if state.shared.is_none() {
                state.shared = Shared::open(false).ok();
            }
            if let Some(shared) = state.shared.take() {
                let received = shared.read(state.sequence, |sequence, jpeg| {
                    state.buffer = Some(frame_buffer(if jpeg.is_empty() { IDLE } else { jpeg })?);
                    state.sequence = sequence;
                    Ok(())
                });
                state.shared = Some(shared);
                received?;
            }
            let buffer = state.buffer.as_ref().ok_or(MF_E_SHUTDOWN)?;
            sample.DeleteItem(&GENERATION)?;
            sample.AddBuffer(buffer)?;
            sample.SetSampleTime(MFGetSystemTime())?;
            sample.SetSampleDuration(PERIOD)?;
            sample.SetUINT32(&MFSampleExtension_CleanPoint, 1)?;
            self.events
                .QueueEventParamUnk(MEMediaSample.0 as u32, &GUID::zeroed(), S_OK, &sample)
        }
    }
}

fn frame_buffer(jpeg: &[u8]) -> Result<IMFMediaBuffer> {
    unsafe {
        let buffer = MFCreateMemoryBuffer(jpeg.len() as u32)?;
        let mut bytes = std::ptr::null_mut();
        buffer.Lock(&mut bytes, None, None)?;
        std::ptr::copy_nonoverlapping(jpeg.as_ptr(), bytes, jpeg.len());
        buffer.Unlock()?;
        buffer.SetCurrentLength(jpeg.len() as u32)?;
        Ok(buffer)
    }
}
