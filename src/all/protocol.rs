use std::io::{self, Read, Write};
use std::net::TcpStream;
const MAGIC: [u8; 4] = *b"PDSK";
pub const VERSION: u8 = 15;
pub const FPS: u32 = 60;
const CONFIG: u8 = 0x10;
const AUDIO: u8 = 0x11;
const MOVE: u8 = 0x20;
const BUTTON: u8 = 0x21;
const KEY: u8 = 0x23;
const ZOOM: u8 = 0x24;
const VIDEO: u8 = 0x25;
const WHEEL: u8 = 0x26;
const AUDIO_TOGGLE: u8 = 0x27;
const CAMERA_TOGGLE: u8 = 0x29;
const TOUCH: u8 = 0x2a;
pub const TOUCH_POINTS: usize = 10;
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Video {
    pub width: u16,
    pub height: u16,
}
#[derive(Clone, Copy)]
pub struct Session {
    pub video: Video,
    pub nonce: [u8; 8],
}
#[derive(Clone, Copy)]
pub struct View {
    pub height: u16,
    pub width: u16,
    pub x: u16,
    pub y: u16,
}
#[derive(Clone, Copy, Default)]
pub struct TouchPoint {
    pub id: u8,
    pub phase: u8,
    pub x: u16,
    pub y: u16,
}
pub enum Input {
    Move(i16, i16, u32),
    Button(u8, bool),
    Key(u16, bool),
    Zoom(i16),
    Video(bool),
    Keyframe,
    Wheel(i16),
    Audio(bool),
    Camera(bool),
    Touch {
        points: [TouchPoint; TOUCH_POINTS],
        count: usize,
    },
}
pub fn authenticate_with_magic(
    stream: &mut TcpStream,
    token: &[u8; 32],
    magic: [u8; 4],
) -> io::Result<Session> {
    let mut hello = [0; 41];
    hello[..4].copy_from_slice(&magic);
    stream.read_exact(&mut hello[4..])?;
    if hello[..4] != MAGIC || hello[4] != VERSION || hello[5..37] != *token {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "authentication failed",
        ));
    }
    let video = Video {
        width: u16::from_be_bytes([hello[37], hello[38]]),
        height: u16::from_be_bytes([hello[39], hello[40]]),
    };
    if video.width == 0 || video.height == 0 || video.width > 4096 || video.height > 4096 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid display size",
        ));
    }
    let mut nonce = [0; 8];
    stream.read_exact(&mut nonce)?;
    Ok(Session { video, nonce })
}
pub fn write_config(stream: &mut TcpStream, view: View) -> io::Result<()> {
    let mut packet = [0; 10];
    packet[0] = CONFIG;
    packet[1] = 2;
    packet[2..4].copy_from_slice(&view.x.to_be_bytes());
    packet[4..6].copy_from_slice(&view.y.to_be_bytes());
    packet[6..8].copy_from_slice(&view.width.to_be_bytes());
    packet[8..10].copy_from_slice(&view.height.to_be_bytes());
    stream.write_all(&packet)
}
pub fn write_canvas(stream: &mut TcpStream, canvas: Video, origin: (u16, u16)) -> io::Result<()> {
    let mut packet = [0x12, 1, 0, 0, 0, 0, 0, 0, 0, 0];
    packet[2..4].copy_from_slice(&canvas.height.to_be_bytes());
    packet[4..6].copy_from_slice(&canvas.width.to_be_bytes());
    packet[6..8].copy_from_slice(&origin.0.to_be_bytes());
    packet[8..10].copy_from_slice(&origin.1.to_be_bytes());
    stream.write_all(&packet)
}

pub fn write_pointer(stream: &mut TcpStream, point: (u16, u16), sequence: u32) -> io::Result<()> {
    let mut packet = [0x12, 2, 0, 0, 0, 0, 0, 0, 0, 0];
    packet[2..4].copy_from_slice(&point.0.to_be_bytes());
    packet[4..6].copy_from_slice(&point.1.to_be_bytes());
    packet[6..10].copy_from_slice(&sequence.to_be_bytes());
    stream.write_all(&packet)
}

pub fn write_audio(stream: &mut TcpStream) -> io::Result<()> {
    stream.write_all(&[AUDIO, 1, 0, 0, 0, 0, 0, 0, 0, 0])
}
pub fn stop_audio(stream: &mut TcpStream) -> io::Result<()> {
    stream.write_all(&[AUDIO, 0, 0, 0, 0, 0, 0, 0, 0, 0])
}
pub fn read_input(stream: &mut TcpStream) -> io::Result<Input> {
    let mut kind = [0];
    stream.read_exact(&mut kind)?;
    match kind[0] {
        MOVE => {
            let x = read_i16(stream)?;
            let y = read_i16(stream)?;
            let mut sequence = [0; 4];
            stream.read_exact(&mut sequence)?;
            Ok(Input::Move(x, y, u32::from_be_bytes(sequence)))
        }
        BUTTON => {
            let mut packet = [0; 2];
            stream.read_exact(&mut packet)?;
            Ok(Input::Button(packet[0], packet[1] != 0))
        }
        KEY => {
            let key = read_u16(stream)?;
            let mut state = [0];
            stream.read_exact(&mut state)?;
            Ok(Input::Key(key, state[0] != 0))
        }
        ZOOM => Ok(Input::Zoom(read_i16(stream)?)),
        0x2b => Ok(Input::Keyframe),
        VIDEO => {
            let mut state = [0];
            stream.read_exact(&mut state)?;
            match state[0] {
                0 => Ok(Input::Video(false)),
                1 => Ok(Input::Video(true)),
                _ => Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid video state",
                )),
            }
        }
        WHEEL => Ok(Input::Wheel(read_i16(stream)?)),
        AUDIO_TOGGLE => {
            let mut state = [0];
            stream.read_exact(&mut state)?;
            Ok(Input::Audio(state[0] != 0))
        }
        CAMERA_TOGGLE => {
            let mut state = [0];
            stream.read_exact(&mut state)?;
            match state[0] {
                0 => Ok(Input::Camera(false)),
                1 => Ok(Input::Camera(true)),
                _ => Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid camera state",
                )),
            }
        }
        TOUCH => read_touch(stream),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unknown input packet",
        )),
    }
}
fn read_touch(stream: &mut TcpStream) -> io::Result<Input> {
    let mut count = [0];
    stream.read_exact(&mut count)?;
    let count = usize::from(count[0]);
    if count > TOUCH_POINTS {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "too many touch points",
        ));
    }
    let mut points = [TouchPoint::default(); TOUCH_POINTS];
    let mut packet = [0u8; TOUCH_POINTS * 6];
    stream.read_exact(&mut packet[..count * 6])?;
    let mut ids = 0u16;
    for (point, bytes) in points.iter_mut().zip(packet[..count * 6].chunks_exact(6)) {
        if usize::from(bytes[0]) >= TOUCH_POINTS || !(1..=4).contains(&bytes[1]) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid touch point",
            ));
        }
        let bit = 1u16 << bytes[0];
        if ids & bit != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "duplicate touch point",
            ));
        }
        ids |= bit;
        *point = TouchPoint {
            id: bytes[0],
            phase: bytes[1],
            x: u16::from_be_bytes([bytes[2], bytes[3]]),
            y: u16::from_be_bytes([bytes[4], bytes[5]]),
        };
    }
    Ok(Input::Touch { points, count })
}
fn read_u16(stream: &mut TcpStream) -> io::Result<u16> {
    let mut value = [0; 2];
    stream.read_exact(&mut value)?;
    Ok(u16::from_be_bytes(value))
}
fn read_i16(stream: &mut TcpStream) -> io::Result<i16> {
    Ok(read_u16(stream)? as i16)
}
