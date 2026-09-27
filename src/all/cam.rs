use crate::all::protocol::VERSION;

const HEADER: usize = 25;
const PAYLOAD: usize = 1150;
const MAX_FRAME: usize = 8 * 1024 * 1024;

pub struct Frame {
    pub jpeg: Vec<u8>,
}

#[derive(Default)]
pub struct Receiver {
    sequence: Option<u32>,
    length: usize,
    frame: Vec<u8>,
    received: Vec<bool>,
    count: usize,
}

impl Receiver {
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn push(&mut self, packet: &[u8], nonce: [u8; 8]) -> Option<Frame> {
        if packet.len() < HEADER
            || packet[..4] != *b"PDSC"
            || packet[4] != VERSION
            || packet[5..13] != nonce
        {
            return None;
        }
        let sequence = u32::from_be_bytes(packet[13..17].try_into().ok()?);
        let fragment = usize::from(u16::from_be_bytes(packet[17..19].try_into().ok()?));
        let fragments = usize::from(u16::from_be_bytes(packet[19..21].try_into().ok()?));
        let length = u32::from_be_bytes(packet[21..25].try_into().ok()?) as usize;
        if length == 0
            || length > MAX_FRAME
            || fragments == 0
            || fragment >= fragments
            || fragments != length.div_ceil(PAYLOAD)
        {
            return None;
        }
        if self.sequence.is_none_or(|previous| {
            sequence != previous && sequence.wrapping_sub(previous) < 0x8000_0000
        }) {
            self.sequence = Some(sequence);
            self.length = length;
            self.frame.resize(length, 0);
            self.received.clear();
            self.received.resize(fragments, false);
            self.count = 0;
        } else if self.sequence != Some(sequence)
            || self.length != length
            || self.received.len() != fragments
        {
            return None;
        }
        let start = fragment * PAYLOAD;
        let end = (start + PAYLOAD).min(length);
        if packet.len() != HEADER + end - start {
            return None;
        }
        if !self.received[fragment] {
            self.frame[start..end].copy_from_slice(&packet[HEADER..]);
            self.received[fragment] = true;
            self.count += 1;
        }
        if self.count != fragments {
            return None;
        }
        self.count = 0;
        self.received.fill(false);
        Some(Frame {
            jpeg: self.frame.clone(),
        })
    }
}
