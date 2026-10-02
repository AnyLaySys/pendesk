use crate::all::cam::Receiver as CameraReceiver;
use crate::all::cmd::Config;
use crate::all::media as transport;
use crate::all::protocol;
use crate::encoder::H264;
use crate::gpu::Gpu;
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
    let mut encoder = H264::new(&gpu, screen.dimensions.0, screen.dimensions.1, config.quality).map_err(|error| error.to_string())?;
    protocol::write_config(&mut stream, screen.view).map_err(|error| error.to_string())?;
    {
        let viewport = viewport.lock().unwrap();
        protocol::write_canvas(&mut stream, viewport.canvas(), viewport.origin()).map_err(|error| error.to_string())?;
        protocol::write_pointer(&mut stream, viewport.pointer(), 0).map_err(|error| error.to_string())?;
    }
    let peer = transport::wait_video_peer(video_socket, &config.token, session.nonce)?;
    let mic_socket = MicSocket::new(SOCKET(video_socket.as_raw_socket() as usize))?;
    let active = Arc::new(AtomicBool::new(true));
    let video_active = Arc::new(AtomicBool::new(false));
    let camera_active = Arc::new(AtomicBool::new(false));
    let refresh = Arc::new(AtomicBool::new(true));
    let audio_gate = Arc::new((Mutex::new(false), Condvar::new()));
    let audio_thread = crate::session_audio::spawn(
        Arc::clone(&active),
        Arc::clone(&audio_gate),
        video_socket,
        stream.try_clone().map_err(|error| error.to_string())?,
        peer,
    )?;
    let input_thread = crate::session_input::spawn(
        Arc::clone(&active),
        Arc::clone(&video_active),
        Arc::clone(&refresh),
        Arc::clone(&camera_active),
        Arc::clone(&viewport),
        Arc::clone(&signal),
        Arc::clone(&audio_gate),
        stream.try_clone().map_err(|error| error.to_string())?,
    );
    let interval = Duration::from_secs_f64(1.0 / f64::from(protocol::FPS));
    let keyframe = Duration::from_secs(5);
    let mut frame = Vec::new();
    let mut sequence = 0u32;
    let started = Instant::now();
    let mut last_idr = started - keyframe;
    let mut next_frame = started;
    let mut playback = None;
    let mut camera_receiver = CameraReceiver::default();
    let mut camera_window = None;
    let result = (|| {
        let receive = |timeout, playback: &mut _, receiver: &mut _, window: &mut _| {
            media::wait_socket(&signal, &mic_socket, timeout);
            media::receive_packets(
                video_socket,
                peer,
                playback,
                camera_active.load(Ordering::Relaxed),
                receiver,
                window,
            );
        };
        while active.load(Ordering::Relaxed) {
            if !camera_active.load(Ordering::Relaxed) {
                camera_receiver.reset();
                camera_window = None;
            }
            if !video_active.load(Ordering::Relaxed) {
                next_frame = Instant::now();
                receive(20, &mut playback, &mut camera_receiver, &mut camera_window);
                continue;
            }
            let milliseconds = next_frame.saturating_duration_since(Instant::now()).as_micros().div_ceil(1000) as u32;
            receive(milliseconds, &mut playback, &mut camera_receiver, &mut camera_window);
            if !active.load(Ordering::Relaxed)
                || !video_active.load(Ordering::Relaxed)
                || Instant::now() < next_frame
            {
                continue;
            }
            let canvas = viewport.lock().unwrap().canvas();
            if screen.changed() || screen.dimensions != (u32::from(canvas.width), u32::from(canvas.height)) {
                screen = Screen::new(&gpu, video, &mut viewport.lock().unwrap(), &signal)?;
                protocol::write_config(&mut stream, screen.view)
                    .map_err(|error| error.to_string())?;
                encoder = H264::new(&gpu, screen.dimensions.0, screen.dimensions.1, config.quality)
                    .map_err(|error| error.to_string())?;
                let viewport = viewport.lock().unwrap();
                protocol::write_canvas(&mut stream, viewport.canvas(), viewport.origin()).map_err(|error| error.to_string())?;
                protocol::write_pointer(&mut stream, viewport.pointer(), viewport.input_sequence).map_err(|error| error.to_string())?;
                refresh.store(true, Ordering::Relaxed);
            }
            let now = Instant::now();
            let area = {
                let mut viewport = viewport.lock().unwrap();
                viewport.advance(now);
                viewport.capture_source()
            };
            let captured = match screen.capture(area)? {
                Some(captured) => captured,
                None => {
                    receive(20, &mut playback, &mut camera_receiver, &mut camera_window);
                    continue;
                }
            };
            next_frame += interval;
            if next_frame <= now { next_frame = now + interval; }
            let keyframe = refresh.swap(false, Ordering::Relaxed) || last_idr.elapsed() >= keyframe;
            encoder.encode(&captured, keyframe, &mut frame).map_err(|error| error.to_string())?;
            if keyframe { last_idr = now; }
            if !frame.is_empty() {
                sequence = sequence.wrapping_add(1);
                let timestamp = now.duration_since(started).as_micros() as u64 * 9 / 100;
                transport::send_video_frame(video_socket, peer, sequence, timestamp, &frame);
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
