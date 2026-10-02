use crate::all::media as transport;
use crate::all::protocol;
use crate::audio::{self, Audio};
use std::net::{TcpStream, UdpSocket};
use std::sync::{
    Arc, Condvar, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;

pub fn spawn(
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
        let mut frame = Vec::with_capacity(audio::FRAME_SAMPLES * 2);
        let mut encoded = [0; 1152];
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
            let Ok(mut encoder) = audio::encoder() else {
                break;
            };
            if protocol::write_audio(&mut stream).is_err() {
                break;
            }
            while active.load(Ordering::Relaxed) && *gate.0.lock().unwrap() {
                if audio.read(&mut samples).is_err() {
                    break;
                }
                frame.extend_from_slice(&samples);
                while frame.len() >= audio::FRAME_SAMPLES * 2 {
                    let Ok(length) =
                        encoder.encode(&frame[..audio::FRAME_SAMPLES * 2], &mut encoded)
                    else {
                        break;
                    };
                    transport::send_audio_frame(&socket, peer, &mut sequence, &encoded[..length]);
                    frame.drain(..audio::FRAME_SAMPLES * 2);
                }
            }
            frame.clear();
            if protocol::stop_audio(&mut stream).is_err() {
                break;
            }
        }
    }))
}
