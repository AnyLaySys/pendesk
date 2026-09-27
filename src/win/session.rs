use crate::all::args::Config;
use crate::all::cam::Receiver as CameraReceiver;
use crate::all::media as transport;
use crate::all::protocol::{self, Input};
use crate::audio::Audio;
use crate::encoder::Jpeg;
use crate::gpu::Gpu;
use crate::input;
use crate::media::{self, MicSocket};
use crate::screen::Screen;
use crate::signal::Signal;
use crate::viewport::Viewport;
use std::net::{Shutdown, TcpStream, UdpSocket};
use std::os::windows::io::AsRawSocket;
use std::sync::{
    Arc, Condvar, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};
use windows::Win32::Networking::WinSock::SOCKET;

pub fn run(
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
    let signal = Signal::new()?;
    let mut screen = Screen::new(&gpu, video, &mut viewport.lock().unwrap(), &signal)?;
    let mut encoder = Jpeg::new(config.quality).map_err(|error| error.to_string())?;
    protocol::write_config(&mut stream, screen.view).map_err(|error| error.to_string())?;
    let peer = transport::wait_video_peer(video_socket, &config.token, session.nonce)?;
    let mic_socket = MicSocket::new(SOCKET(video_socket.as_raw_socket() as usize))?;
    let active = Arc::new(AtomicBool::new(true));
    let video_active = Arc::new(AtomicBool::new(true));
    let camera_active = Arc::new(AtomicBool::new(false));
    let audio_gate = Arc::new((Mutex::new(false), Condvar::new()));
    let audio_thread = start_audio_thread(
        Arc::clone(&active),
        Arc::clone(&audio_gate),
        video_socket,
        stream.try_clone().map_err(|error| error.to_string())?,
        peer,
    )?;
    let input_thread = start_input_thread(
        Arc::clone(&active),
        Arc::clone(&video_active),
        Arc::clone(&camera_active),
        Arc::clone(&viewport),
        Arc::clone(&signal),
        Arc::clone(&audio_gate),
        stream.try_clone().map_err(|error| error.to_string())?,
    );
    let interval = Duration::from_secs_f64(1.0 / f64::from(config.fps));
    let keyframe = Duration::from_secs(1);
    let mut frame = Vec::new();
    let mut sequence = 0u32;
    let mut last_area = None;
    let mut last_captured = 0u64;
    let mut last_sent = Instant::now() - keyframe;
    let mut playback = None;
    let mut camera_receiver = CameraReceiver::default();
    let mut camera_window = None;
    let result = (|| {
        while active.load(Ordering::Relaxed) {
            if !camera_active.load(Ordering::Relaxed) {
                camera_receiver.reset();
                camera_window = None;
            }
            if !video_active.load(Ordering::Relaxed) {
                media::wait_socket(&signal, &mic_socket, 20);
                media::receive_packets(
                    video_socket,
                    peer,
                    &mut playback,
                    camera_active.load(Ordering::Relaxed),
                    &mut camera_receiver,
                    &mut camera_window,
                );
                continue;
            }
            let since = last_sent.elapsed();
            let milliseconds = if since < keyframe {
                (if since < interval {
                    interval - since
                } else {
                    keyframe - since
                })
                .as_millis() as u32
            } else {
                interval.as_millis() as u32
            };
            media::wait_socket(&signal, &mic_socket, milliseconds);
            media::receive_packets(
                video_socket,
                peer,
                &mut playback,
                camera_active.load(Ordering::Relaxed),
                &mut camera_receiver,
                &mut camera_window,
            );
            if !active.load(Ordering::Relaxed)
                || !video_active.load(Ordering::Relaxed)
                || last_sent.elapsed() < interval
            {
                continue;
            }
            if screen.changed() {
                screen = Screen::new(&gpu, video, &mut viewport.lock().unwrap(), &signal)?;
                protocol::write_config(&mut stream, screen.view)
                    .map_err(|error| error.to_string())?;
            }
            let area = viewport.lock().unwrap().capture();
            let before = last_captured;
            let captured = match screen.capture(area)? {
                Some(captured) => captured,
                None => {
                    media::wait_socket(&signal, &mic_socket, 20);
                    media::receive_packets(
                        video_socket,
                        peer,
                        &mut playback,
                        camera_active.load(Ordering::Relaxed),
                        &mut camera_receiver,
                        &mut camera_window,
                    );
                    continue;
                }
            };
            last_captured = captured.captured;
            if last_sent.elapsed() >= keyframe
                || before != captured.captured
                || last_area != Some(area)
            {
                last_area = Some(area);
                encoder
                    .encode(&captured, &mut frame)
                    .map_err(|error| error.to_string())?;
                if !frame.is_empty() {
                    sequence = sequence.wrapping_add(1);
                    last_sent = Instant::now();
                    transport::send_video_frame(video_socket, peer, sequence, &frame);
                }
            }
        }
        Ok(())
    })();
    active.store(false, Ordering::Relaxed);
    audio_gate.1.notify_one();
    drop(playback);
    let _ = stream.shutdown(Shutdown::Both);
    let _ = input_thread.join();
    let _ = audio_thread.join();
    result
}

fn start_audio_thread(
    active: Arc<AtomicBool>,
    gate: Arc<(Mutex<bool>, Condvar)>,
    video_socket: &UdpSocket,
    mut stream: TcpStream,
    peer: transport::VideoPeer,
) -> Result<thread::JoinHandle<()>, String> {
    let socket = video_socket
        .try_clone()
        .map_err(|error| error.to_string())?;
    Ok(thread::spawn(move || {
        let mut samples = Vec::new();
        let mut sequence = 0u32;
        while active.load(Ordering::Relaxed) {
            {
                let (lock, cond) = &*gate;
                let mut on = lock.lock().unwrap();
                while !*on && active.load(Ordering::Relaxed) {
                    on = cond.wait(on).unwrap();
                }
            }
            if !active.load(Ordering::Relaxed) {
                break;
            }
            let Ok(mut audio) = Audio::new() else {
                continue;
            };
            if protocol::write_audio(&mut stream, audio.rate, audio.channels()).is_err() {
                break;
            }
            while active.load(Ordering::Relaxed) && *gate.0.lock().unwrap() {
                if audio.read(&mut samples).is_err() {
                    break;
                }
                if !samples.is_empty() {
                    transport::send_audio_frame(&socket, peer, &mut sequence, &samples);
                }
            }
        }
    }))
}

fn start_input_thread(
    active: Arc<AtomicBool>,
    video_active: Arc<AtomicBool>,
    camera_active: Arc<AtomicBool>,
    viewport: Arc<Mutex<Viewport>>,
    signal: Arc<Signal>,
    audio_gate: Arc<(Mutex<bool>, Condvar)>,
    mut stream: TcpStream,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let mut pressed_keys = [false; 256];
        let mut pressed_buttons = [false; 4];
        while active.load(Ordering::Relaxed) {
            match protocol::read_input(&mut stream) {
                Ok(Input::Move(x, y)) => {
                    let (x, y) = viewport.lock().unwrap().move_pointer(x, y);
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
                Ok(Input::Zoom(delta)) => viewport.lock().unwrap().zoom(delta),
                Ok(Input::Video(enabled)) => {
                    if !enabled {
                        for (key, pressed) in pressed_keys.iter_mut().enumerate() {
                            if *pressed {
                                let _ = input::key(key as u16, false);
                                *pressed = false;
                            }
                        }
                        for (button, pressed) in pressed_buttons.iter_mut().enumerate().skip(1) {
                            if *pressed {
                                let _ = input::button(button as u8, false);
                                *pressed = false;
                            }
                        }
                    }
                    video_active.store(enabled, Ordering::Relaxed);
                    signal.set();
                }
                Ok(Input::Wheel(delta)) => {
                    let _ = input::wheel(delta);
                }
                Ok(Input::Audio(enabled)) => {
                    *audio_gate.0.lock().unwrap() = enabled;
                    audio_gate.1.notify_one();
                }
                Ok(Input::Camera(enabled)) => {
                    camera_active.store(enabled, Ordering::Relaxed);
                    signal.set();
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
        active.store(false, Ordering::Relaxed);
        signal.set();
    })
}
