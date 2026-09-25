use crate::all::args::Config;
use crate::all::protocol;
use std::io::{self, Read};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};
const VIDEO_HELLO: [u8; 4] = *b"PDSU";
const VIDEO_FRAME: [u8; 4] = *b"PDSV";
const AUDIO_FRAME: [u8; 4] = *b"PDSA";
const VIDEO_HEADER: usize = 25;
const VIDEO_PAYLOAD: usize = 1150;
const AUDIO_HEADER: usize = 17;
const AUDIO_PAYLOAD: usize = 1152;
pub trait Backend: Send + Sync {
    fn tune_video(&self, socket: &UdpSocket);
    fn session(
        &self,
        stream: TcpStream,
        config: &Config,
        video: &UdpSocket,
        magic: [u8; 4],
    ) -> Result<(), String>;
    fn files(&self, stream: TcpStream, token: [u8; 32], magic: [u8; 4]) -> io::Result<()>;
}
#[derive(Clone, Copy)]
pub struct VideoPeer {
    pub address: SocketAddr,
    pub nonce: [u8; 8],
}
pub fn run(
    config: Config,
    address: IpAddr,
    stopped: impl Fn() -> bool,
    backend: Arc<dyn Backend>,
) -> Result<(), String> {
    let listener = TcpListener::bind(SocketAddr::new(address, config.port))
        .map_err(|error| error.to_string())?;
    listener
        .set_nonblocking(true)
        .map_err(|error| error.to_string())?;
    let video = UdpSocket::bind(SocketAddr::new(address, config.port))
        .map_err(|error| error.to_string())?;
    video
        .set_nonblocking(true)
        .map_err(|error| error.to_string())?;
    backend.tune_video(&video);
    while !stopped() {
        match listener.accept() {
            Ok((stream, _)) => {
                let Ok(video) = video.try_clone() else {
                    continue;
                };
                let config = config.clone();
                let backend = Arc::clone(&backend);
                thread::spawn(move || dispatch(stream, config, video, backend));
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(50))
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(())
}
fn dispatch(mut stream: TcpStream, config: Config, video: UdpSocket, backend: Arc<dyn Backend>) {
    let mut magic = [0; 4];
    if stream.read_exact(&mut magic).is_err() {
        return;
    }
    if magic == *b"PDSK" {
        let _ = backend.session(stream, &config, &video, magic);
    } else if magic == *b"PDSF" {
        let _ = backend.files(stream, config.token, magic);
    }
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
pub fn send_video_frame(socket: &UdpSocket, peer: VideoPeer, sequence: u32, frame: &[u8]) {
    let Ok(length) = u32::try_from(frame.len()) else {
        return;
    };
    let fragments = frame.len().div_ceil(VIDEO_PAYLOAD);
    if fragments == 0 || fragments > usize::from(u16::MAX) {
        return;
    }
    let fragments = fragments as u16;
    let mut packet = [0; VIDEO_HEADER + VIDEO_PAYLOAD];
    packet[..4].copy_from_slice(&VIDEO_FRAME);
    packet[4] = protocol::VERSION;
    packet[5..13].copy_from_slice(&peer.nonce);
    packet[13..17].copy_from_slice(&sequence.to_be_bytes());
    packet[19..21].copy_from_slice(&fragments.to_be_bytes());
    packet[21..25].copy_from_slice(&length.to_be_bytes());
    for index in 0..fragments {
        let start = usize::from(index) * VIDEO_PAYLOAD;
        let end = (start + VIDEO_PAYLOAD).min(frame.len());
        packet[17..19].copy_from_slice(&index.to_be_bytes());
        packet[VIDEO_HEADER..VIDEO_HEADER + end - start].copy_from_slice(&frame[start..end]);
        if socket
            .send_to(&packet[..VIDEO_HEADER + end - start], peer.address)
            .is_err()
        {
            return;
        }
    }
}
pub fn send_audio_frame(socket: &UdpSocket, peer: VideoPeer, sequence: &mut u32, samples: &[u8]) {
    let mut packet = [0; AUDIO_HEADER + AUDIO_PAYLOAD];
    packet[..4].copy_from_slice(&AUDIO_FRAME);
    packet[4] = protocol::VERSION;
    packet[5..13].copy_from_slice(&peer.nonce);
    for chunk in samples.chunks(AUDIO_PAYLOAD) {
        *sequence = sequence.wrapping_add(1);
        packet[13..17].copy_from_slice(&sequence.to_be_bytes());
        packet[AUDIO_HEADER..AUDIO_HEADER + chunk.len()].copy_from_slice(chunk);
        if socket
            .send_to(&packet[..AUDIO_HEADER + chunk.len()], peer.address)
            .is_err()
        {
            return;
        }
    }
}
