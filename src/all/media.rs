use crate::all::protocol;
use std::io;
use std::net::{SocketAddr, UdpSocket};
use std::thread;
use std::time::{Duration, Instant};

const VIDEO_HELLO: [u8; 4] = *b"PDSU";
const VIDEO_ACK: [u8; 4] = *b"PDSH";
const VIDEO_FRAME: [u8; 4] = *b"PDSV";
const AUDIO_FRAME: [u8; 4] = *b"PDSA";
const VIDEO_HEADER: usize = 33;
const VIDEO_PAYLOAD: usize = 1150;
const AUDIO_HEADER: usize = 17;
const AUDIO_PAYLOAD: usize = 1152;

#[derive(Clone, Copy)]
pub struct VideoPeer {
    pub address: SocketAddr,
    pub nonce: [u8; 8],
}

pub fn wait_video_peer(
    socket: &UdpSocket,
    token: &[u8; 32],
    nonce: [u8; 8],
) -> Result<VideoPeer, String> {
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut packet = [0; 45];
    loop {
        match socket.recv_from(&mut packet) {
            Ok((length, address))
                if length == packet.len()
                    && packet[..4] == VIDEO_HELLO
                    && packet[4] == protocol::VERSION
                    && packet[5..37] == token[..]
                    && packet[37..] == nonce =>
            {
                let mut ack = [0; 13];
                ack[..4].copy_from_slice(&VIDEO_ACK);
                ack[4] = protocol::VERSION;
                ack[5..].copy_from_slice(&nonce);
                socket
                    .send_to(&ack, address)
                    .map_err(|error| error.to_string())?;
                return Ok(VideoPeer { address, nonce });
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                if Instant::now() >= deadline {
                    return Err("could not establish the video channel".into());
                }
                thread::sleep(Duration::from_millis(10));
            }
            Err(error) => return Err(error.to_string()),
        }
    }
}

pub fn send_video_frame(socket: &UdpSocket, peer: VideoPeer, sequence: u32, timestamp: u64, frame: &[u8]) {
    let Ok(length) = u32::try_from(frame.len()) else {
        return;
    };
    let fragments = frame.len().div_ceil(VIDEO_PAYLOAD);
    if fragments == 0 || fragments > usize::from(u16::MAX) {
        return;
    }
    let fragments = fragments as u16;
    let mut packet = [0; VIDEO_HEADER + VIDEO_PAYLOAD];
    packet[4] = protocol::VERSION;
    packet[5..13].copy_from_slice(&peer.nonce);
    packet[13..17].copy_from_slice(&sequence.to_be_bytes());
    packet[19..21].copy_from_slice(&fragments.to_be_bytes());
    packet[21..25].copy_from_slice(&length.to_be_bytes());
    packet[25..33].copy_from_slice(&timestamp.to_be_bytes());
    let started = Instant::now();
    let spread = Duration::from_micros((u64::from(fragments) * 150).min(8000));
    let mut parity = [0u8; VIDEO_PAYLOAD];
    for index in 0..fragments {
        if index > 0 && index % 4 == 0 {
            let due = spread * u32::from(index) / u32::from(fragments);
            if let Some(wait) = due.checked_sub(started.elapsed()) { thread::sleep(wait); }
        }
        packet[..4].copy_from_slice(&VIDEO_FRAME);
        let start = usize::from(index) * VIDEO_PAYLOAD;
        let end = (start + VIDEO_PAYLOAD).min(frame.len());
        packet[17..19].copy_from_slice(&index.to_be_bytes());
        for (accumulator, value) in parity.iter_mut().zip(&frame[start..end]) { *accumulator ^= *value; }
        packet[VIDEO_HEADER..VIDEO_HEADER + end - start].copy_from_slice(&frame[start..end]);
        if socket
            .send_to(&packet[..VIDEO_HEADER + end - start], peer.address)
            .is_err()
        {
            return;
        }
        if index % 8 == 7 || index + 1 == fragments {
            packet[..4].copy_from_slice(b"PDSF");
            packet[17..19].copy_from_slice(&(index / 8 * 8).to_be_bytes());
            packet[VIDEO_HEADER..].copy_from_slice(&parity);
            let length = (frame.len() - usize::from(index / 8 * 8) * VIDEO_PAYLOAD).min(VIDEO_PAYLOAD);
            let _ = socket.send_to(&packet[..VIDEO_HEADER + length], peer.address);
            parity.fill(0);
        }
    }
}

pub fn send_audio_frame(socket: &UdpSocket, peer: VideoPeer, sequence: &mut u32, frame: &[u8]) {
    if frame.is_empty() || frame.len() > AUDIO_PAYLOAD {
        return;
    }
    let mut packet = [0; AUDIO_HEADER + AUDIO_PAYLOAD];
    packet[..4].copy_from_slice(&AUDIO_FRAME);
    packet[4] = protocol::VERSION;
    packet[5..13].copy_from_slice(&peer.nonce);
    *sequence = sequence.wrapping_add(1);
    packet[13..17].copy_from_slice(&sequence.to_be_bytes());
    packet[AUDIO_HEADER..AUDIO_HEADER + frame.len()].copy_from_slice(frame);
    let _ = socket.send_to(&packet[..AUDIO_HEADER + frame.len()], peer.address);
}
