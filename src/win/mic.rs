use std::io;
use std::collections::VecDeque;
use opus_decoder::OpusDecoder;
use windows::Win32::Foundation::E_NOTIMPL;
use windows::Win32::Media::Audio::{
    AUDCLNT_SHAREMODE_SHARED, IAudioClient, IAudioRenderClient, IMMDeviceEnumerator,
    MMDeviceEnumerator, WAVEFORMATEXTENSIBLE, eConsole, eRender,
};
use windows::Win32::Media::Multimedia::{KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, WAVE_FORMAT_IEEE_FLOAT};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx, CoTaskMemFree,
    CoUninitialize,
};
use windows::core::Error as WindowsError;

pub struct Playback {
    client: IAudioClient,
    render: IAudioRenderClient,
    channels: usize,
    rate: u32,
    float: bool,
    bytes: usize,
    buffer: u32,
    decoder: OpusDecoder,
    pending: VecDeque<i16>,
}

impl Playback {
    pub fn new() -> io::Result<Self> {
        unsafe {
            CoInitializeEx(None, COINIT_MULTITHREADED)
                .ok()
                .map_err(|error| io::Error::other(error.to_string()))?;
            let result = Self::open();
            if result.is_err() {
                CoUninitialize();
            }
            result
        }
    }

    fn open() -> io::Result<Self> {
        unsafe {
        let enumerator: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)
            .map_err(|error| io::Error::other(error.to_string()))?;
        let device = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)
            .map_err(|error| io::Error::other(error.to_string()))?;
        let client: IAudioClient = device.Activate(CLSCTX_ALL, None)
            .map_err(|error| io::Error::other(error.to_string()))?;
        let format = client.GetMixFormat()
            .map_err(|error| io::Error::other(error.to_string()))?;
        let tag = (*format).wFormatTag;
        let channels = usize::from((*format).nChannels);
        let rate = (*format).nSamplesPerSec;
        let float = tag == WAVE_FORMAT_IEEE_FLOAT as u16
            || (tag == 0xfffe
                && std::ptr::read_unaligned(
                    &raw const (*format.cast::<WAVEFORMATEXTENSIBLE>()).SubFormat,
                ) == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if !(1..=8).contains(&channels) || !(8000..=192000).contains(&rate) {
            CoTaskMemFree(Some(format.cast()));
            return Err(io::Error::other(WindowsError::from_hresult(E_NOTIMPL).to_string()));
        }
        let bytes = usize::from((*format).nBlockAlign) / channels;
        if (float && bytes != 4) || (!float && !matches!(bytes, 2..=4)) {
            CoTaskMemFree(Some(format.cast()));
            return Err(io::Error::other(WindowsError::from_hresult(E_NOTIMPL).to_string()));
        }
        let initialized = client.Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200_000, 0, format, None);
        CoTaskMemFree(Some(format.cast()));
        initialized.map_err(|error| io::Error::other(error.to_string()))?;
        let render: IAudioRenderClient = client.GetService()
            .map_err(|error| io::Error::other(error.to_string()))?;
        let buffer = client.GetBufferSize()
            .map_err(|error| io::Error::other(error.to_string()))?;
        client.Start()
            .map_err(|error| io::Error::other(error.to_string()))?;
        let decoder = OpusDecoder::new(48000, 1)
            .map_err(|error| io::Error::other(error.to_string()))?;
            Ok(Self { client, render, channels, rate, float, bytes, buffer, decoder, pending: VecDeque::new() })
        }
    }

    pub fn packet(&mut self, rtp: &[u8]) -> io::Result<()> {
        let Some(payload) = opus_payload(rtp) else { return Ok(()); };
        let mut pcm = [0i16; 5760];
        let frames = self.decoder.decode(payload, &mut pcm, false)
            .map_err(|error| io::Error::other(error.to_string()))?;
        if frames == 0 { return Ok(()); }
        let output_frames = (frames as u64 * u64::from(self.rate)).div_ceil(48000) as usize;
        for output in 0..output_frames {
            let position = output as u64 * 48000;
            let source = (position / u64::from(self.rate)) as usize;
            let fraction = position % u64::from(self.rate);
            let first = i64::from(pcm[source.min(frames - 1)]);
            let second = i64::from(pcm[(source + 1).min(frames - 1)]);
            let value = (first * (i64::from(self.rate) - fraction as i64)
                + second * fraction as i64) / i64::from(self.rate);
            self.pending.push_back(value as i16);
        }
        while self.pending.len() > self.rate as usize / 5 { self.pending.pop_front(); }
        let padding = unsafe { self.client.GetCurrentPadding() }
            .map_err(|error| io::Error::other(error.to_string()))?;
        let frames = self.pending.len().min(self.buffer.saturating_sub(padding) as usize);
        if frames == 0 { return Ok(()); }
        unsafe {
            let data = self.render.GetBuffer(frames as u32)
                .map_err(|error| io::Error::other(error.to_string()))?;
            if self.float {
                let output = std::slice::from_raw_parts_mut(data.cast::<f32>(), frames * self.channels);
                for frame in 0..frames {
                    let value = f32::from(self.pending.pop_front().unwrap()) / 32768.0;
                    for channel in 0..self.channels { output[frame * self.channels + channel] = value; }
                }
            } else {
                let output = std::slice::from_raw_parts_mut(
                    data.cast::<u8>(),
                    frames * self.channels * self.bytes,
                );
                for frame in 0..frames {
                    let value = i32::from(self.pending.pop_front().unwrap());
                    for channel in 0..self.channels {
                        let offset = (frame * self.channels + channel) * self.bytes;
                        match self.bytes {
                            2 => output[offset..offset + 2]
                                .copy_from_slice(&(value as i16).to_le_bytes()),
                            3 => {
                                let sample = (value << 8).to_le_bytes();
                                output[offset..offset + 3].copy_from_slice(&sample[..3]);
                            }
                            4 => output[offset..offset + 4]
                                .copy_from_slice(&(value << 16).to_le_bytes()),
                            _ => unreachable!(),
                        }
                    }
                }
            }
            self.render.ReleaseBuffer(frames as u32, 0)
                .map_err(|error| io::Error::other(error.to_string()))?;
        }
        Ok(())
    }
}

impl Drop for Playback {
    fn drop(&mut self) {
        unsafe {
            let _ = self.client.Stop();
            CoUninitialize();
        }
    }
}

fn opus_payload(rtp: &[u8]) -> Option<&[u8]> {
    if rtp.len() < 13 || rtp[0] >> 6 != 2 || rtp[1] & 0x7f != 111 { return None; }
    let mut offset = 12 + usize::from(rtp[0] & 15) * 4;
    if rtp[0] & 16 != 0 {
        if rtp.len() < offset + 4 { return None; }
        offset += 4 + usize::from(u16::from_be_bytes([rtp[offset + 2], rtp[offset + 3]])) * 4;
    }
    let mut end = rtp.len();
    if rtp[0] & 32 != 0 {
        let padding = usize::from(*rtp.last()?);
        if padding == 0 || padding > end.saturating_sub(offset) { return None; }
        end -= padding;
    }
    (end > offset).then_some(&rtp[offset..end])
}
