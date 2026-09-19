use std::slice;
use windows::Win32::Foundation::E_NOTIMPL;
use windows::Win32::Media::Audio::{
    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, IAudioCaptureClient, IAudioClient,
    IMMDeviceEnumerator, MMDeviceEnumerator, WAVEFORMATEXTENSIBLE, eConsole, eRender,
};
use windows::Win32::Media::Multimedia::{KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, WAVE_FORMAT_IEEE_FLOAT};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx, CoTaskMemFree,
    CoUninitialize,
};
use windows::core::{Error, Result};
const WAVE_FORMAT_EXTENSIBLE: u16 = 0xFFFE;
const AUDCLNT_BUFFERFLAGS_SILENT: u32 = 2;
pub struct Audio {
    client: IAudioClient,
    capture: IAudioCaptureClient,
    channels: usize,
    float: bool,
    pub rate: u32,
}
impl Audio {
    pub fn new() -> Result<Self> {
        unsafe {
            CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
            let enumerator: IMMDeviceEnumerator =
                CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)?;
            let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)?;
            let client: IAudioClient = device.Activate(CLSCTX_ALL, None)?;
            let format = client.GetMixFormat()?;
            let tag = (*format).wFormatTag;
            let channels = usize::from((*format).nChannels);
            let rate = (*format).nSamplesPerSec;
            let bits = (*format).wBitsPerSample;
            let subformat = std::ptr::read_unaligned(
                &raw const (*format.cast::<WAVEFORMATEXTENSIBLE>()).SubFormat,
            );
            let float = tag == WAVE_FORMAT_IEEE_FLOAT as u16
                || (tag == WAVE_FORMAT_EXTENSIBLE && subformat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            if !(1..=2).contains(&channels) || rate == 0 || (!float && bits != 16) {
                CoTaskMemFree(Some(format.cast()));
                return Err(Error::from_hresult(E_NOTIMPL));
            }
            let result = client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                2_000_000,
                0,
                format,
                None,
            );
            CoTaskMemFree(Some(format.cast()));
            result?;
            let capture: IAudioCaptureClient = client.GetService()?;
            client.Start()?;
            Ok(Self {
                client,
                capture,
                channels,
                float,
                rate,
            })
        }
    }
    pub fn channels(&self) -> u8 {
        self.channels as u8
    }
    pub fn read(&mut self, output: &mut Vec<u8>) -> Result<()> {
        output.clear();
        unsafe {
            while self.capture.GetNextPacketSize()? != 0 {
                let mut data = std::ptr::null_mut();
                let mut frames = 0;
                let mut flags = 0;
                self.capture
                    .GetBuffer(&mut data, &mut frames, &mut flags, None, None)?;
                let count = frames as usize * self.channels;
                if flags & AUDCLNT_BUFFERFLAGS_SILENT != 0 {
                    output.resize(output.len() + count * 2, 0);
                } else if self.float {
                    for sample in slice::from_raw_parts(data.cast::<f32>(), count) {
                        let value = (sample.clamp(-1.0, 1.0) * 32767.0) as i16;
                        output.extend_from_slice(&value.to_le_bytes());
                    }
                } else {
                    output.extend_from_slice(slice::from_raw_parts(data, count * 2));
                }
                self.capture.ReleaseBuffer(frames)?;
            }
        }
        Ok(())
    }
}
impl Drop for Audio {
    fn drop(&mut self) {
        unsafe {
            let _ = self.client.Stop();
            CoUninitialize();
        }
    }
}
