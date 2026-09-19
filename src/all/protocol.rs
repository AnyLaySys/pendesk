use std::io::{self, Read, Write};
use std::net::TcpStream;
const MAGIC: [u8; 4] = *b"PDSK";
pub const VERSION: u8 = 7;
const CONFIG: u8 = 0x10;
const AUDIO: u8 = 0x11;
const MOVE: u8 = 0x20;
const BUTTON: u8 = 0x21;
const KEY: u8 = 0x23;
const ZOOM: u8 = 0x24;
const VIDEO: u8 = 0x25;
const WHEEL: u8 = 0x26;
#[derive(Clone, Copy)]
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
pub enum Input {
    Move(i16, i16),
    Button(u8, bool),
    Key(u16, bool),
    Zoom(i16),
    Video(bool),
    Wheel(i16),
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
pub fn write_audio(stream: &mut TcpStream, rate: u32, channels: u8) -> io::Result<()> {
    let mut packet = [0; 10];
    packet[0] = AUDIO;
    packet[1] = 5;
    packet[2..6].copy_from_slice(&rate.to_be_bytes());
    packet[6] = channels;
    stream.write_all(&packet)
}
pub fn read_input(stream: &mut TcpStream) -> io::Result<Input> {
    let mut kind = [0];
    stream.read_exact(&mut kind)?;
    match kind[0] {
        MOVE => Ok(Input::Move(read_i16(stream)?, read_i16(stream)?)),
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
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unknown input packet",
        )),
    }
}
fn read_u16(stream: &mut TcpStream) -> io::Result<u16> {
    let mut value = [0; 2];
    stream.read_exact(&mut value)?;
    Ok(u16::from_be_bytes(value))
}
fn read_i16(stream: &mut TcpStream) -> io::Result<i16> {
    Ok(read_u16(stream)? as i16)
}
