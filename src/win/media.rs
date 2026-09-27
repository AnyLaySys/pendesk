use crate::all::cam::Receiver as CameraReceiver;
use crate::all::media::VideoPeer;
use crate::all::protocol;
use crate::cam::Window as CameraWindow;
use crate::mic::Playback;
use crate::signal::Signal;
use std::io;
use std::net::UdpSocket;
use windows::Win32::Networking::WinSock::{
    FD_READ, SOCKET, WSACloseEvent, WSACreateEvent, WSAEVENT, WSAEventSelect,
};

pub struct MicSocket {
    socket: SOCKET,
    event: WSAEVENT,
}

impl MicSocket {
    pub fn new(socket: SOCKET) -> Result<Self, String> {
        let event = unsafe { WSACreateEvent() }.map_err(|error| error.to_string())?;
        if unsafe { WSAEventSelect(socket, Some(event), FD_READ as i32) } != 0 {
            unsafe {
                let _ = WSACloseEvent(event);
            }
            return Err("could not watch microphone packets".into());
        }
        Ok(Self { socket, event })
    }
}

impl Drop for MicSocket {
    fn drop(&mut self) {
        unsafe {
            let _ = WSAEventSelect(self.socket, None, 0);
            let _ = WSACloseEvent(self.event);
        }
    }
}

pub fn wait_socket(signal: &Signal, socket: &MicSocket, milliseconds: u32) -> bool {
    signal.wait_socket(socket.socket, socket.event, milliseconds)
}

pub fn receive_packets(
    socket: &UdpSocket,
    peer: VideoPeer,
    playback: &mut Option<Playback>,
    camera_active: bool,
    camera_receiver: &mut CameraReceiver,
    camera_window: &mut Option<CameraWindow>,
) {
    let mut packet = [0; 1400];
    loop {
        match socket.recv_from(&mut packet) {
            Ok((length, _address))
                if length >= 14
                    && packet[..4] == *b"PDSM"
                    && packet[4] == protocol::VERSION
                    && packet[5..13] == peer.nonce =>
            {
                match packet[13] {
                    0 => *playback = None,
                    1 => {
                        if playback.is_none() {
                            *playback = match Playback::new() {
                                Ok(playback) => Some(playback),
                                Err(error) => {
                                    eprintln!("{error}");
                                    None
                                }
                            };
                        }
                        if length > 14
                            && let Some(player) = playback.as_mut()
                            && let Err(error) = player.packet(&packet[14..length])
                        {
                            eprintln!("{error}");
                            *playback = None;
                        }
                    }
                    _ => {}
                }
            }
            Ok((length, _address)) if camera_active && length >= 25 && packet[..4] == *b"PDSC" => {
                if let Some(frame) = camera_receiver.push(&packet[..length], peer.nonce) {
                    if camera_window.is_none() {
                        *camera_window = CameraWindow::new().ok();
                    }
                    if camera_window
                        .as_ref()
                        .is_some_and(|window| window.frame(frame))
                    {
                        continue;
                    }
                    *camera_window = None;
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
            Err(error) => {
                eprintln!("{error}");
                break;
            }
        }
    }
}
