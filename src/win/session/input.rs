use crate::all::protocol::{self, Input};
use crate::input;
use crate::signal::Signal;
use crate::viewport::Viewport;
use std::net::TcpStream;
use std::sync::{
    Arc, Condvar, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;

fn release_all(keys: &mut [bool; 256], buttons: &mut [bool; 4]) {
    for (key, pressed) in keys.iter_mut().enumerate() {
        if *pressed {
            let _ = input::key(key as u16, false);
            *pressed = false;
        }
    }
    for (button, pressed) in buttons.iter_mut().enumerate().skip(1) {
        if *pressed {
            let _ = input::button(button as u8, false);
            *pressed = false;
        }
    }
}

pub fn spawn(
    active: Arc<AtomicBool>,
    video_active: Arc<AtomicBool>,
    refresh: Arc<AtomicBool>,
    camera_active: Arc<AtomicBool>,
    viewport: Arc<Mutex<Viewport>>,
    signal: Arc<Signal>,
    audio_gate: Arc<(Mutex<bool>, Condvar)>,
    mut stream: TcpStream,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let mut pressed_keys = [false; 256];
        let mut pressed_buttons = [false; 4];
        let mut active_touches = [None; 10];
        while active.load(Ordering::Relaxed) {
            match protocol::read_input(&mut stream) {
                Ok(Input::Move(x, y, sequence)) => {
                    let point = {
                        let mut viewport = viewport.lock().unwrap();
                        viewport.input_sequence = sequence;
                        viewport.move_pointer(x, y)
                    };
                    let _ = input::move_pointer(point.0, point.1);
                    if protocol::write_pointer(&mut stream, point, sequence).is_err() {
                        break;
                    }
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
                Ok(Input::Keyframe) => {
                    refresh.store(true, Ordering::Relaxed);
                    signal.set();
                }
                Ok(Input::Video(enabled)) => {
                    if enabled {
                        refresh.store(true, Ordering::Relaxed);
                    } else {
                        release_all(&mut pressed_keys, &mut pressed_buttons);
                    }
                    video_active.store(enabled, Ordering::Relaxed);
                    signal.set();
                }
                Ok(Input::Wheel(delta)) => {
                    let _ = input::wheel(delta);
                }
                Ok(Input::Touch(points)) => {
                    let view = viewport.lock().unwrap();
                    let contacts = points
                        .iter()
                        .map(|point| {
                            let (x, y) = view.touch_point(point.x, point.y);
                            input::TouchContact {
                                id: point.id,
                                phase: point.phase,
                                x,
                                y,
                            }
                        })
                        .collect::<Vec<_>>();
                    drop(view);
                    if input::touch(&contacts).is_ok() {
                        for contact in contacts {
                            active_touches[usize::from(contact.id)] = if contact.phase == 3 {
                                None
                            } else {
                                Some(contact)
                            };
                        }
                    }
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
        let releases = active_touches
            .into_iter()
            .flatten()
            .map(|mut contact| {
                contact.phase = 3;
                contact
            })
            .collect::<Vec<_>>();
        let _ = input::touch(&releases);
        release_all(&mut pressed_keys, &mut pressed_buttons);
        active.store(false, Ordering::Relaxed);
        signal.set();
    })
}
