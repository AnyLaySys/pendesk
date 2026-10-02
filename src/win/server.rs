use crate::all::cmd::Config;
use crate::all::files as all_files;
use crate::all::server::{self as all_server, Backend};
use crate::files::Disk;
use crate::session;
use std::io;
use std::net::{IpAddr, Ipv4Addr, TcpStream, UdpSocket};
use std::os::windows::io::AsRawSocket;
use std::path::PathBuf;
use std::process::Command;
use std::sync::Arc;
use windows::Win32::Networking::WinSock::{SO_RCVBUF, SO_SNDBUF, SOCKET, SOL_SOCKET, setsockopt};

struct Windows;

impl Backend for Windows {
    fn tune_video(&self, socket: &UdpSocket) {
        let send = (4 * 1024 * 1024_i32).to_ne_bytes();
        let receive = (8 * 1024 * 1024_i32).to_ne_bytes();
        let handle = SOCKET(socket.as_raw_socket() as usize);
        unsafe {
            let _ = setsockopt(handle, SOL_SOCKET, SO_SNDBUF, Some(&send));
            let _ = setsockopt(handle, SOL_SOCKET, SO_RCVBUF, Some(&receive));
        }
    }

    fn session(
        &self,
        stream: TcpStream,
        config: &Config,
        video: &UdpSocket,
        magic: [u8; 4],
    ) -> Result<(), String> {
        session::run(stream, config, video, magic)
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
