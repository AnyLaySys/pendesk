use crate::all::media::{VIDEO_GROUP as GROUP, VIDEO_HEADER as HEADER, VIDEO_PAYLOAD as PAYLOAD};
use crate::all::protocol::VERSION;

const MAX_FRAME: usize = 8_388_608;

#[derive(Default)]
struct Frame {
    sequence: Option<u32>,
    length: usize,
    data: Vec<u8>,
    parity: Vec<u8>,
    received: Vec<bool>,
    count: usize,
}

#[derive(Default)]
pub struct Receiver {
    frames: [Frame; 4],
    newest: Option<u32>,
    delivered: Option<u32>,
}

impl Receiver {
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn push(&mut self, packet: &[u8], nonce: [u8; 8]) -> Option<&[u8]> {
        if packet.len() < HEADER || packet[4] != VERSION || packet[5..13] != nonce {
            return None;
        }
        let parity = match &packet[..4] {
            b"PDSC" => false,
            b"PDCP" => true,
            _ => return None,
        };
        let sequence = u32::from_be_bytes(packet[13..17].try_into().ok()?);
        let index = usize::from(u16::from_be_bytes(packet[17..19].try_into().ok()?));
        let fragments = usize::from(u16::from_be_bytes(packet[19..21].try_into().ok()?));
        let length = u32::from_be_bytes(packet[21..25].try_into().ok()?) as usize;
        if length == 0
            || length > MAX_FRAME
            || fragments != length.div_ceil(PAYLOAD)
            || index >= fragments
            || (parity && index % GROUP != 0)
        {
            return None;
        }
        if self.delivered.is_some_and(|last| {
            sequence.wrapping_sub(last) == 0 || sequence.wrapping_sub(last) >= 0x8000_0000
        }) {
            return None;
        }
        if let Some(newest) = self.newest {
            let ahead = sequence.wrapping_sub(newest);
            if ahead < 0x8000_0000 {
                self.newest = Some(sequence);
            } else if newest.wrapping_sub(sequence) >= self.frames.len() as u32 {
                return None;
            }
        } else {
            self.newest = Some(sequence);
        }
        let start = index * PAYLOAD;
        let amount = (length - start).min(PAYLOAD);
        if packet.len() != HEADER + amount {
            return None;
        }
        let slot = sequence as usize % self.frames.len();
        let frame = &mut self.frames[slot];
        let groups = fragments.div_ceil(GROUP);
        if frame.sequence != Some(sequence) {
            frame.sequence = Some(sequence);
            frame.length = length;
            frame.data.resize(length, 0);
            frame.parity.resize(groups * PAYLOAD, 0);
            frame.parity.fill(0);
            frame.received.resize(fragments + groups, false);
            frame.received.fill(false);
            frame.count = 0;
        } else if frame.length != length {
            return None;
        }
        let group = index / GROUP;
        let marker = if parity { fragments + group } else { index };
        if frame.received[marker] {
            return None;
        }
        frame.received[marker] = true;
        let accumulator = &mut frame.parity[group * PAYLOAD..(group + 1) * PAYLOAD];
        for (target, byte) in accumulator.iter_mut().zip(&packet[HEADER..]) {
            *target ^= byte;
        }
        if !parity {
            frame.data[start..start + amount].copy_from_slice(&packet[HEADER..]);
            frame.count += 1;
        }
        if frame.received[fragments + group] {
            let mut missing = (group * GROUP..((group + 1) * GROUP).min(fragments))
                .filter(|&i| !frame.received[i]);
            if let Some(absent) = missing.next().filter(|_| missing.next().is_none()) {
                let start = absent * PAYLOAD;
                let amount = (length - start).min(PAYLOAD);
                frame.data[start..start + amount].copy_from_slice(&accumulator[..amount]);
                frame.received[absent] = true;
                frame.count += 1;
            }
        }
        if frame.count != fragments
            || !frame.data.starts_with(&[0xff, 0xd8])
            || !frame.data.ends_with(&[0xff, 0xd9])
        {
            return None;
        }
        self.delivered = Some(sequence);
        Some(&frame.data)
    }
}
