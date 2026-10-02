use crate::all::cmd::Config;
use std::io::{self, Read};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

pub trait Backend: Send + Sync {
    fn tune_video(&self, socket: &UdpSocket);
    fn session(
        &self,
        stream: TcpStream,
        config: &Config,
        video: &UdpSocket,
        magic: [u8; 4],
    ) -> Result<(), String>;
    fn files(&self, stream: TcpStream, token: [u8; 32], magic: [u8; 4]) -> io::Result<()>;
}

pub fn run(
    config: Config,
    address: IpAddr,
    stopped: impl Fn() -> bool,
    backend: Arc<dyn Backend>,
) -> Result<(), String> {
    let listener = TcpListener::bind(SocketAddr::new(address, config.port))
        .map_err(|error| error.to_string())?;
    listener
        .set_nonblocking(true)
        .map_err(|error| error.to_string())?;
    let video = UdpSocket::bind(SocketAddr::new(address, config.port))
        .map_err(|error| error.to_string())?;
    video
        .set_nonblocking(true)
        .map_err(|error| error.to_string())?;
    backend.tune_video(&video);
    while !stopped() {
        match listener.accept() {
            Ok((stream, _)) => {
                let Ok(video) = video.try_clone() else {
                    continue;
                };
                let config = config.clone();
                let backend = Arc::clone(&backend);
                thread::spawn(move || dispatch(stream, config, video, backend));
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(50))
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(())
}

fn dispatch(mut stream: TcpStream, config: Config, video: UdpSocket, backend: Arc<dyn Backend>) {
    let mut magic = [0; 4];
    if stream.read_exact(&mut magic).is_err() {
        return;
    }
    if magic == *b"PDSK" {
        let _ = backend.session(stream, &config, &video, magic);
    } else if magic == *b"PDSF" {
        let _ = backend.files(stream, config.token, magic);
    }
}
