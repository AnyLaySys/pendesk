use crate::{all::protocol::FPS, gpu::Gpu, screen::Frame};
use std::mem::ManuallyDrop;
use std::time::{Duration, Instant};
use std::sync::{Arc, OnceLock, atomic::{AtomicBool, Ordering}, mpsc};
use windows::Win32::Graphics::Direct3D11::ID3D11Texture2D;
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::Variant::VARIANT;
use windows::core::{implement, Interface, IUnknownImpl, Ref, Result, HRESULT};

#[implement(IMFAsyncCallback)]
struct Events {
    source: IMFMediaEventGenerator,
    sender: mpsc::Sender<std::result::Result<u32, HRESULT>>,
    stopped: Arc<AtomicBool>,
}

impl IMFAsyncCallback_Impl for Events_Impl {
    fn GetParameters(&self, _flags: *mut u32, _queue: *mut u32) -> Result<()> {
        Err(windows::Win32::Foundation::E_NOTIMPL.into())
    }

    fn Invoke(&self, result: Ref<IMFAsyncResult>) -> Result<()> {
        unsafe {
            let event = self.source.EndGetEvent(result.as_ref());
            let value = event.and_then(|event| {
                event.GetStatus()?.ok()?;
                event.GetType()
            }).map_err(|error| error.code());
            let _ = self.sender.send(value);
            if !self.stopped.load(Ordering::Acquire) {
                let callback: IMFAsyncCallback = self.to_interface();
                if let Err(error) = self.source.BeginGetEvent(&callback, None) {
                    let _ = self.sender.send(Err(error.code()));
                }
            }
            Ok(())
        }
    }
}

pub struct H264 {
    transform: IMFTransform,
    events: mpsc::Receiver<std::result::Result<u32, HRESULT>>,
    stopped: Arc<AtomicBool>,
    activation: IMFActivate,
    codec: ICodecAPI,
    _manager: IMFDXGIDeviceManager,
    input: u32,
    output: u32,
    timestamp: i64,
    ready: bool,
    sample: Option<IMFSample>,
    texture: usize,
    output_info: MFT_OUTPUT_STREAM_INFO,
}

impl H264 {
    pub fn new(gpu: &Gpu, width: u32, height: u32, quality: u32) -> Result<Self> {
        unsafe {
            static STARTUP: OnceLock<HRESULT> = OnceLock::new();
            STARTUP.get_or_init(|| MFStartup(MF_VERSION, MFSTARTUP_FULL)
                .map(|_| HRESULT(0)).unwrap_or_else(|error| error.code())).ok()?;
            let info = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_H264 };
            let mut activations = std::ptr::null_mut();
            let mut count = 0;
            MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                None, Some(&info), &mut activations, &mut count)?;
            let mut selected = None;
            for index in 0..count as usize {
                if let Some(activation) = (*activations.add(index)).take() {
                    if selected.is_none() {
                        selected = activation.ActivateObject::<IMFTransform>().ok().map(|transform| (transform, activation));
                    }
                }
            }
            CoTaskMemFree(Some(activations.cast()));
            let (transform, activation) = selected.ok_or_else(|| windows::core::Error::new(
                MF_E_TOPO_CODEC_NOT_FOUND, "No hardware H.264 encoder is available"))?;
            let attributes = transform.GetAttributes()?;
            attributes.SetUINT32(&MF_TRANSFORM_ASYNC_UNLOCK, 1)?;
            attributes.SetUINT32(&MF_LOW_LATENCY, 1)?;
            let mut token = 0;
            let mut manager = None;
            MFCreateDXGIDeviceManager(&mut token, &mut manager)?;
            let manager = manager.unwrap();
            manager.ResetDevice(&gpu.device, token)?;
            transform.ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, manager.as_raw() as usize)?;
            let codec: ICodecAPI = transform.cast()?;
            codec.SetValue(&CODECAPI_AVLowLatencyMode, &VARIANT::from(true))?;
            let _ = codec.SetValue(&CODECAPI_AVEncCommonRealTime, &VARIANT::from(true));
            let _ = codec.SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &VARIANT::from(0u32));
            let bitrate = (width * height * FPS / 8).clamp(1_000_000, 8_000_000) * (50 + quality) / 100;
            let _ = codec.SetValue(&CODECAPI_AVEncCommonRateControlMode, &VARIANT::from(0u32));
            let _ = codec.SetValue(&CODECAPI_AVEncCommonMeanBitRate, &VARIANT::from(bitrate));
            let mut input = [0];
            let mut output = [0];
            let _ = transform.GetStreamIDs(&mut input, &mut output);
            let output_type = Self::media_type(MFVideoFormat_H264, width, height)?;
            output_type.SetUINT32(&MF_MT_AVG_BITRATE, bitrate)?;
            output_type.SetUINT32(&MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main.0 as u32)?;
            transform.SetOutputType(output[0], &output_type, 0)?;
            let input_type = Self::media_type(MFVideoFormat_NV12, width, height)?;
            transform.SetInputType(input[0], &input_type, 0)?;
            let _ = codec.SetValue(&CODECAPI_AVEncMPVGOPSize, &VARIANT::from(FPS * 5));
            let _ = codec.SetValue(&CODECAPI_AVEncCommonBufferSize, &VARIANT::from(bitrate / 20));
            let _ = codec.SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &VARIANT::from(25u32));
            transform.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)?;
            transform.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)?;
            let source: IMFMediaEventGenerator = transform.cast()?;
            let stopped = Arc::new(AtomicBool::new(false));
            let (sender, events) = mpsc::channel();
            let callback: IMFAsyncCallback = Events { source: source.clone(), sender, stopped: stopped.clone() }.into();
            let output_info = transform.GetOutputStreamInfo(output[0])?;
            source.BeginGetEvent(&callback, None)?;
            Ok(Self { transform, events, stopped, activation, codec, _manager: manager, input: input[0], output: output[0],
                timestamp: 0, ready: false, sample: None, texture: 0, output_info })
        }
    }

    fn media_type(subtype: windows::core::GUID, width: u32, height: u32) -> Result<IMFMediaType> {
        unsafe {
            let media = MFCreateMediaType()?;
            media.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            media.SetGUID(&MF_MT_SUBTYPE, &subtype)?;
            media.SetUINT64(&MF_MT_FRAME_SIZE, (u64::from(width) << 32) | u64::from(height))?;
            media.SetUINT64(&MF_MT_FRAME_RATE, (u64::from(FPS) << 32) | 1)?;
            media.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, (1u64 << 32) | 1)?;
            media.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            Ok(media)
        }
    }

    pub fn encode(&mut self, frame: &Frame, keyframe: bool, output: &mut Vec<u8>) -> Result<()> {
        output.clear();
        let deadline = Instant::now() + Duration::from_secs(2);
        let mut submitted = false;
        unsafe {
            loop {
                if self.ready && !submitted {
                    if keyframe {
                        self.codec.SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &VARIANT::from(1u32))?;
                    }
                    let texture = frame.texture.as_raw() as usize;
                    if self.texture != texture {
                        let sample = MFCreateSample()?;
                        let buffer = MFCreateDXGISurfaceBuffer(&ID3D11Texture2D::IID, frame.texture, 0, false)?;
                        sample.AddBuffer(&buffer)?;
                        self.sample = Some(sample);
                        self.texture = texture;
                    }
                    let sample = self.sample.as_ref().unwrap();
                    sample.SetSampleTime(self.timestamp)?;
                    sample.SetSampleDuration(10_000_000 / i64::from(FPS))?;
                    self.transform.ProcessInput(self.input, sample, 0)?;
                    self.timestamp += 10_000_000 / i64::from(FPS);
                    self.ready = false;
                    submitted = true;
                }
                match self.events.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                    Ok(Ok(kind)) => {
                        if kind == METransformNeedInput.0 as u32 { self.ready = true; }
                        if kind == METransformHaveOutput.0 as u32 {
                            self.read_output(output)?;
                            if !output.is_empty() { return Ok(()); }
                        }
                    }
                    Ok(Err(error)) => return Err(error.into()),
                    Err(_) => return Err(windows::core::Error::new(MF_E_NOTACCEPTING, "Hardware encoder timed out")),
                }
            }
        }
    }

    fn read_output(&self, output: &mut Vec<u8>) -> Result<()> {
        unsafe {
            let info = self.output_info;
            let sample = if info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES.0 as u32 == 0 {
                let sample = MFCreateSample()?;
                sample.AddBuffer(&MFCreateMemoryBuffer(info.cbSize.max(1024 * 1024))?)?;
                Some(sample)
            } else { None };
            let mut buffers = [MFT_OUTPUT_DATA_BUFFER { dwStreamID: self.output,
                pSample: ManuallyDrop::new(sample), ..Default::default() }];
            let mut status = 0;
            let result = self.transform.ProcessOutput(0, &mut buffers, &mut status);
            let sample = ManuallyDrop::take(&mut buffers[0].pSample);
            ManuallyDrop::drop(&mut buffers[0].pEvents);
            result?;
            if let Some(sample) = sample {
                let buffer = sample.ConvertToContiguousBuffer()?;
                let mut bytes = std::ptr::null_mut();
                let mut length = 0;
                buffer.Lock(&mut bytes, None, Some(&mut length))?;
                output.extend_from_slice(std::slice::from_raw_parts(bytes, length as usize));
                buffer.Unlock()?;
            }
            Ok(())
        }
    }
}

impl Drop for H264 {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        unsafe {
            let _ = self.transform.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            let _ = self.transform.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            let _ = self.transform.ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            let _ = self.activation.ShutdownObject();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{all::protocol::Video, screen::Screen, signal::Signal, viewport::Viewport};

    #[test]
    #[ignore]
    fn hardware_capture_encode() {
        unsafe {
            windows::Win32::System::Com::CoInitializeEx(None,
                windows::Win32::System::Com::COINIT_MULTITHREADED).unwrap();
        }
        let gpu = Gpu::new().unwrap();
        let video = Video { width: 280, height: 936 };
        let mut viewport = Viewport::new(video).unwrap();
        let signal = Signal::new().unwrap();
        let mut screen = Screen::new(&gpu, video, &mut viewport, &signal).unwrap();
        let mut encoder = H264::new(&gpu, screen.dimensions.0, screen.dimensions.1, 70).unwrap();
        let mut stream = Vec::new();
        let mut output = Vec::new();
        let start = Instant::now();
        let mut count = 0;
        while count < 120 {
            if let Some(frame) = screen.capture(viewport.capture_source()).unwrap() {
                encoder.encode(&frame, count == 0 || count == 37, &mut output).unwrap();
                assert!(!output.is_empty());
                if count == 0 || count == 37 {
                    assert!(output.windows(4).any(|v| v[..3] == [0, 0, 1] && v[3] & 31 == 5));
                }
                stream.extend_from_slice(&output);
                count += 1;
            } else {
                assert!(start.elapsed() < Duration::from_secs(10));
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        eprintln!("120 frames encoded in {:?}, {} bytes", start.elapsed(), stream.len());
        if let Some(path) = std::env::var_os("PENDESK_TEST_H264") { std::fs::write(path, stream).unwrap(); }
    }
}
