use crate::all::{cam::Receiver as CameraReceiver, protocol};
use crate::{cam::Shared, mic::Playback, transport::VideoPeer};
use std::net::UdpSocket;
use std::os::windows::io::AsRawSocket;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use windows::Win32::Foundation::HANDLE;
use windows::Win32::Networking::WinSock::{
    FD_READ, SOCKET, WSACloseEvent, WSACreateEvent, WSAEVENT, WSAEnumNetworkEvents, WSAEventSelect,
    WSANETWORKEVENTS,
};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
use windows::Win32::System::Threading::WaitForMultipleObjects;

struct SocketEvent {
    socket: SOCKET,
    event: WSAEVENT,
}
impl SocketEvent {
    fn new(socket: SOCKET) -> Result<Self, String> {
        let event = unsafe { WSACreateEvent() }.map_err(|error| error.to_string())?;
        if unsafe { WSAEventSelect(socket, Some(event), FD_READ as i32) } != 0 {
            unsafe {
                let _ = WSACloseEvent(event);
            }
            return Err("could not watch camera and microphone packets".into());
        }
        Ok(Self { socket, event })
    }
    fn wait(&self, audio: Option<HANDLE>, timeout: u32) {
        let mut handles = [HANDLE(self.event.0 as *mut _), HANDLE::default()];
        let count = if let Some(event) = audio {
            handles[1] = event;
            2
        } else {
            1
        };
        unsafe {
            WaitForMultipleObjects(&handles[..count], false, timeout);
            let mut events = WSANETWORKEVENTS::default();
            WSAEnumNetworkEvents(self.socket, self.event, &mut events);
        }
    }
}
impl Drop for SocketEvent {
    fn drop(&mut self) {
        unsafe {
            let _ = WSAEventSelect(self.socket, None, 0);
            let _ = WSACloseEvent(self.event);
        }
    }
}

pub fn spawn(
    active: Arc<AtomicBool>,
    camera_active: Arc<AtomicBool>,
    socket: &UdpSocket,
    peer: VideoPeer,
    camera: Option<Arc<Shared>>,
) -> Result<thread::JoinHandle<()>, String> {
    let socket = socket.try_clone().map_err(|error| error.to_string())?;
    let event = SocketEvent::new(SOCKET(socket.as_raw_socket() as usize))?;
    Ok(thread::spawn(move || {
        if unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) }.is_err() {
            return;
        }
        let mut playback: Option<Playback> = None;
        let mut receiver = CameraReceiver::default();
        let mut was_camera_on = false;
        let mut drained = true;
        while active.load(Ordering::Relaxed) {
            event.wait(
                playback.as_ref().map(Playback::event),
                if drained { 100 } else { 0 },
            );
            let camera_on = camera_active.load(Ordering::Relaxed);
            if !camera_on && was_camera_on {
                receiver.reset();
                if let Some(camera) = &camera {
                    let _ = camera.publish(&[]);
                }
            }
            was_camera_on = camera_on;
            drained = receive_packets(
                &socket,
                peer,
                &mut playback,
                camera_on,
                &mut receiver,
                camera.as_deref(),
            );
            if let Some(player) = playback.as_mut() {
                if let Err(error) = player.flush() {
                    eprintln!("Microphone: {error}");
                    playback = None;
                }
            }
        }
        drop(playback);
        if let Some(camera) = &camera {
            let _ = camera.publish(&[]);
        }
        drop(camera);
        unsafe {
            CoUninitialize();
        }
    }))
}

fn receive_packets(
    socket: &UdpSocket,
    peer: VideoPeer,
    playback: &mut Option<Playback>,
    camera_active: bool,
    camera_receiver: &mut CameraReceiver,
    camera: Option<&Shared>,
) -> bool {
    let mut packet = [0; 1400];
    for _ in 0..64 {
        match socket.recv_from(&mut packet) {
            Ok((length, address))
                if address == peer.address
                    && length >= 14
                    && packet[..4] == *b"PDSM"
                    && packet[4] == protocol::VERSION
                    && packet[5..13] == peer.nonce =>
            {
                match packet[13] {
                    0 => *playback = None,
                    1 => {
                        if playback.is_none() {
                            *playback = Playback::new()
                                .map_err(|error| eprintln!("Microphone: {error}"))
                                .ok();
                        }
                        if length > 14 {
                            if let Some(player) = playback.as_mut() {
                                if let Err(error) = player.packet(&packet[14..length]) {
                                    eprintln!("Microphone: {error}");
                                    *playback = None;
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
            Ok((length, address))
                if address == peer.address
                    && camera_active
                    && length >= 33
                    && (packet[..4] == *b"PDSC" || packet[..4] == *b"PDCP") =>
            {
                if let (Some(frame), Some(camera)) =
                    (camera_receiver.push(&packet[..length], peer.nonce), camera)
                {
                    if let Err(error) = camera.publish(frame) {
                        eprintln!("Camera: {error}");
                    }
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => return true,
            Err(error) => {
                eprintln!("Media: {error}");
                return true;
            }
        }
    }
    false
}
