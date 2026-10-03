use opus::{Channels, Decoder};
use std::collections::VecDeque;
use std::io;
use windows::Win32::Foundation::HANDLE;
use windows::Win32::Media::Audio::{
    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, IAudioClient,
    IAudioRenderClient,
};
use windows::Win32::System::Threading::CreateEventW;
use windows::core::Owned;

pub struct Playback {
    client: IAudioClient,
    render: IAudioRenderClient,
    event: Owned<HANDLE>,
    buffer: u32,
    decoder: Decoder,
    pending: VecDeque<i16>,
    sequence: Option<u16>,
}

impl Playback {
    pub fn new() -> io::Result<Self> {
        let result = (|| unsafe {
            let client = crate::audio::default_render_client()?;
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                200_000,
                0,
                &crate::audio::pcm_format(1),
                None,
            )?;
            let event = Owned::new(CreateEventW(None, false, false, None)?);
            client.SetEventHandle(*event)?;
            let render = client.GetService()?;
            let buffer = client.GetBufferSize()?;
            let decoder = Decoder::new(48_000, Channels::Mono).map_err(|error| {
                windows::core::Error::new(windows::Win32::Foundation::E_FAIL, error.to_string())
            })?;
            client.Start()?;
            Ok::<_, windows::core::Error>(Self {
                client,
                render,
                event,
                buffer,
                decoder,
                pending: VecDeque::new(),
                sequence: None,
            })
        })();
        result.map_err(|error| io::Error::other(error.to_string()))
    }

    pub fn event(&self) -> HANDLE {
        *self.event
    }

    pub fn packet(&mut self, rtp: &[u8]) -> io::Result<()> {
        let Some(payload) = opus_payload(rtp) else {
            return Ok(());
        };
        let sequence = u16::from_be_bytes([rtp[2], rtp[3]]);
        let mut pcm = [0i16; 5760];
        if let Some(previous) = self.sequence {
            let distance = sequence.wrapping_sub(previous);
            if distance == 0 || distance >= 0x8000 {
                return Ok(());
            }
            if distance <= 4 {
                for _ in 1..distance {
                    let frames = self
                        .decoder
                        .decode(&[], &mut pcm[..crate::audio::FRAME_SAMPLES], false)
                        .map_err(io::Error::other)?;
                    self.pending.extend(&pcm[..frames]);
                }
            } else {
                self.decoder.reset_state().map_err(io::Error::other)?;
            }
        }
        self.sequence = Some(sequence);
        let frames = self
            .decoder
            .decode(payload, &mut pcm, false)
            .map_err(io::Error::other)?;
        self.pending.extend(&pcm[..frames]);
        while self.pending.len() > 9600 {
            self.pending.pop_front();
        }
        self.flush()
    }

    pub fn flush(&mut self) -> io::Result<()> {
        (|| unsafe {
            let padding = self.client.GetCurrentPadding()?;
            let count = self
                .pending
                .len()
                .min(self.buffer.saturating_sub(padding) as usize);
            if count != 0 {
                let pointer = self.render.GetBuffer(count as u32)?;
                let output = std::slice::from_raw_parts_mut(pointer.cast::<i16>(), count);
                for value in output {
                    *value = self.pending.pop_front().unwrap();
                }
                self.render.ReleaseBuffer(count as u32, 0)?;
            }
            Ok::<_, windows::core::Error>(())
        })()
        .map_err(|error| io::Error::other(error.to_string()))
    }
}

impl Drop for Playback {
    fn drop(&mut self) {
        unsafe {
            let _ = self.client.Stop();
        }
    }
}

fn opus_payload(rtp: &[u8]) -> Option<&[u8]> {
    if rtp.len() < 13 || rtp[0] >> 6 != 2 || rtp[1] & 0x7f != 111 {
        return None;
    }
    let mut offset = 12 + usize::from(rtp[0] & 15) * 4;
    if rtp[0] & 16 != 0 {
        if rtp.len() < offset + 4 {
            return None;
        }
        offset += 4 + usize::from(u16::from_be_bytes([rtp[offset + 2], rtp[offset + 3]])) * 4;
    }
    let mut end = rtp.len();
    if rtp[0] & 32 != 0 {
        let padding = usize::from(*rtp.last()?);
        if padding == 0 || padding > end.saturating_sub(offset) {
            return None;
        }
        end -= padding;
    }
    (end > offset).then_some(&rtp[offset..end])
}
