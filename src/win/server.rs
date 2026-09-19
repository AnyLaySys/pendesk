use crate::all::args::Config;
use crate::all::files as all_files;
use crate::all::protocol::{self, Input};
use crate::all::server::{self as all_server, Backend};
use crate::audio::Audio;
use crate::encoder::Jpeg;
use crate::files::Disk;
use crate::gpu::Gpu;
use crate::input;
use crate::screen::{Screen, Viewport};
use std::io;
use std::net::{IpAddr, Ipv4Addr, Shutdown, TcpStream, UdpSocket};
use std::os::windows::io::AsRawSocket;
use std::path::PathBuf;
use std::process::Command;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};
use windows::Win32::Networking::WinSock::{SO_SNDBUF, SOCKET, SOL_SOCKET, setsockopt};
struct Windows;
impl Backend for Windows {
    fn tune_video(&self, socket: &UdpSocket) {
        let buffer = (4 * 1024 * 1024_i32).to_ne_bytes();
        unsafe {
            let _ = setsockopt(
                SOCKET(socket.as_raw_socket() as usize),
                SOL_SOCKET,
                SO_SNDBUF,
                Some(&buffer),
            );
        }
    }
    fn session(
        &self,
        stream: TcpStream,
        config: &Config,
        video: &UdpSocket,
        magic: [u8; 4],
    ) -> Result<(), String> {
        session(stream, config, video, magic)
    }
    fn files(&self, stream: TcpStream, token: [u8; 32], magic: [u8; 4]) -> io::Result<()> {
        all_files::serve(stream, token, magic, &Disk)
    }
}
pub fn run(config: Config, stopped: impl Fn() -> bool) -> Result<(), String> {
    all_server::run(config, tailscale()?, stopped, Arc::new(Windows))
}
fn tailscale() -> Result<IpAddr, String> {
    let program = std::env::var_os("TAILSCALE").unwrap_or_else(|| {
        std::env::var_os("ProgramFiles")
            .map(|directory| {
                PathBuf::from(directory)
                    .join("Tailscale")
                    .join("tailscale.exe")
            })
            .filter(|path| path.is_file())
            .map_or_else(|| "tailscale".into(), Into::into)
    });
    let output = Command::new(program)
        .args(["ip", "-4"])
        .output()
        .map_err(|_| "could not run tailscale".to_string())?;
    if !output.status.success() {
        return Err("tailscale did not return an IPv4 address".into());
    }
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .find_map(|line| line.trim().parse::<Ipv4Addr>().ok())
        .filter(|address| address.octets()[0] == 100 && (64..=127).contains(&address.octets()[1]))
        .map(IpAddr::V4)
        .ok_or_else(|| "tailscale did not return a Tailnet IPv4 address".into())
}
fn session(
    mut stream: TcpStream,
    config: &Config,
    video_socket: &UdpSocket,
    magic: [u8; 4],
) -> Result<(), String> {
    stream
        .set_nonblocking(false)
        .map_err(|error| error.to_string())?;
    stream
        .set_nodelay(true)
        .map_err(|error| error.to_string())?;
    let session = protocol::authenticate_with_magic(&mut stream, &config.token, magic)
        .map_err(|error| error.to_string())?;
    let video = session.video;
    let gpu = Gpu::new().map_err(|error| error.to_string())?;
    let viewport = Arc::new(Mutex::new(Viewport::new(video)?));
    let mut screen = Screen::new(&gpu, video, &mut viewport.lock().unwrap())?;
    let mut encoder = Jpeg::new(config.quality).map_err(|error| error.to_string())?;
    protocol::write_config(&mut stream, screen.view).map_err(|error| error.to_string())?;
    let peer = all_server::wait_video_peer(video_socket, &config.token, session.nonce)?;
    let active = Arc::new(AtomicBool::new(true));
    let video_active = Arc::new(AtomicBool::new(true));
    let audio_active = Arc::clone(&active);
    let audio_socket = video_socket.try_clone().map_err(|error| error.to_string())?;
    let mut audio_stream = stream.try_clone().map_err(|error| error.to_string())?;
    let audio_thread = thread::spawn(move || {
        let Ok(mut audio) = Audio::new() else {
            return;
        };
        if protocol::write_audio(&mut audio_stream, audio.rate, audio.channels()).is_err() {
            return;
        }
        let mut samples = Vec::new();
        let mut sequence = 0u32;
        while audio_active.load(Ordering::Relaxed) {
            if audio.read(&mut samples).is_err() {
                break;
            }
            if samples.is_empty() {
                thread::sleep(Duration::from_millis(4));
            } else {
                all_server::send_audio_frame(&audio_socket, peer, &mut sequence, &samples);
            }
        }
    });
    let input_active = Arc::clone(&active);
    let input_video = Arc::clone(&video_active);
    let input_viewport = Arc::clone(&viewport);
    let capture_thread = thread::current();
    let mut input_stream = stream.try_clone().map_err(|error| error.to_string())?;
    let input_thread = thread::spawn(move || {
        let mut pressed_keys = [false; 256];
        let mut pressed_buttons = [false; 4];
        while input_active.load(Ordering::Relaxed) {
            match protocol::read_input(&mut input_stream) {
                Ok(Input::Move(x, y)) => {
                    let (x, y) = input_viewport.lock().unwrap().move_pointer(x, y);
                    let _ = input::move_pointer(x, y);
                }
                Ok(Input::Button(button, down)) if (1..=3).contains(&button) => {
                    if input::button(button, down).is_ok() {
                        pressed_buttons[usize::from(button)] = down;
                    }
                }
                Ok(Input::Key(key, down)) if key < 256 => {
                    if input::key(key, down).is_ok() {
                        pressed_keys[usize::from(key)] = down;
                    }
                }
                Ok(Input::Zoom(delta)) => input_viewport.lock().unwrap().zoom(delta),
                Ok(Input::Video(enabled)) => {
                    input_video.store(enabled, Ordering::Relaxed);
                    capture_thread.unpark();
                }
                Ok(Input::Wheel(delta)) => {
                    let _ = input::wheel(delta);
                }
                _ => break,
            }
        }
        for (key, pressed) in pressed_keys.into_iter().enumerate() {
            if pressed {
                let _ = input::key(key as u16, false);
            }
        }
        for (button, pressed) in pressed_buttons.into_iter().enumerate().skip(1) {
            if pressed {
                let _ = input::button(button as u8, false);
            }
        }
        input_active.store(false, Ordering::Relaxed);
        capture_thread.unpark();
    });
    let delay = Duration::from_secs_f64(1.0 / f64::from(config.fps));
    let mut frame = Vec::new();
    let mut sequence = 0u32;
    let mut last_area = None;
    let mut last_captured = 0u64;
    let result = (|| {
        while active.load(Ordering::Relaxed) {
            if !video_active.load(Ordering::Relaxed) {
                thread::park();
                continue;
            }
            let started = Instant::now();
            if screen.changed() {
                screen = Screen::new(&gpu, video, &mut viewport.lock().unwrap())?;
                protocol::write_config(&mut stream, screen.view)
                    .map_err(|error| error.to_string())?;
            }
            let area = viewport.lock().unwrap().capture();
            let before = last_captured;
            let captured = screen.capture(area)?;
            last_captured = captured.captured;
            if before != captured.captured || last_area != Some(area) {
                last_area = Some(area);
                encoder
                    .encode(&captured, &mut frame)
                    .map_err(|error| error.to_string())?;
                if !frame.is_empty() {
                    sequence = sequence.wrapping_add(1);
                    all_server::send_video_frame(video_socket, peer, sequence, &frame);
                }
            }
            if let Some(remaining) = delay.checked_sub(started.elapsed()) {
                thread::sleep(remaining);
            }
        }
        Ok(())
    })();
    active.store(false, Ordering::Relaxed);
    let _ = stream.shutdown(Shutdown::Both);
    let _ = input_thread.join();
    let _ = audio_thread.join();
    result
}
