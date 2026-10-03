use opus::{Application, Bandwidth, Channels, Encoder};
use std::slice;
use windows::Win32::Foundation::HANDLE;
use windows::Win32::Media::Audio::{
    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, AUDCLNT_STREAMFLAGS_LOOPBACK,
    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, IAudioCaptureClient, IAudioClient,
    IMMDeviceEnumerator, MMDeviceEnumerator, WAVE_FORMAT_PCM, WAVEFORMATEX, eConsole, eRender,
};
use windows::Win32::System::Com::{CLSCTX_ALL, CoCreateInstance};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
use windows::core::{Owned, Result};
const AUDCLNT_BUFFERFLAGS_SILENT: u32 = 2;
pub const FRAME_SAMPLES: usize = 960;

pub(crate) fn pcm_format(channels: u16) -> WAVEFORMATEX {
    WAVEFORMATEX {
        wFormatTag: WAVE_FORMAT_PCM as u16,
        nChannels: channels,
        nSamplesPerSec: 48_000,
        nAvgBytesPerSec: 48_000 * u32::from(channels) * 2,
        nBlockAlign: channels * 2,
        wBitsPerSample: 16,
        cbSize: 0,
    }
}

pub(crate) fn default_render_client() -> Result<IAudioClient> {
    unsafe {
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
        let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)?;
        device.Activate(CLSCTX_ALL, None)
    }
}

pub struct Audio {
    client: IAudioClient,
    capture: IAudioCaptureClient,
    event: Owned<HANDLE>,
}

impl Audio {
    pub fn new() -> Result<Self> {
        unsafe {
            let client = default_render_client()?;
            let format = pcm_format(2);
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK
                    | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                2_000_000,
                0,
                &format,
                None,
            )?;
            let event = Owned::new(CreateEventW(None, false, false, None)?);
            client.SetEventHandle(*event)?;
            let capture: IAudioCaptureClient = client.GetService()?;
            client.Start()?;
            Ok(Self {
                client,
                capture,
                event,
            })
        }
    }

    pub fn read(&mut self, output: &mut Vec<i16>) -> Result<()> {
        output.clear();
        unsafe {
            WaitForSingleObject(*self.event, 200);
            while self.capture.GetNextPacketSize()? != 0 {
                let mut data = std::ptr::null_mut();
                let mut frames = 0;
                let mut flags = 0;
                self.capture
                    .GetBuffer(&mut data, &mut frames, &mut flags, None, None)?;
                let count = frames as usize * 2;
                if flags & AUDCLNT_BUFFERFLAGS_SILENT != 0 {
                    output.resize(output.len() + count, 0);
                } else {
                    output.extend_from_slice(slice::from_raw_parts(data.cast::<i16>(), count));
                }
                self.capture.ReleaseBuffer(frames)?;
            }
        }
        Ok(())
    }
}

pub fn encoder() -> opus::Result<Encoder> {
    let mut encoder = Encoder::new(48_000, Channels::Stereo, Application::Audio)?;
    encoder.set_vbr(true)?;
    encoder.set_complexity(10)?;
    encoder.set_bandwidth(Bandwidth::Fullband)?;
    Ok(encoder)
}

impl Drop for Audio {
    fn drop(&mut self) {
        unsafe {
            let _ = self.client.Stop();
        }
    }
}
