use std::cmp::Ordering;
use std::ffi::OsStr;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
const MAGIC: [u8; 4] = *b"PDSF";
const VERSION: u8 = 2;
const LIST: u8 = 1;
const DOWNLOAD: u8 = 2;
const UPLOAD: u8 = 3;
const DIRECTORY: u8 = 4;
const MAX_PATH: usize = 4096;
const MAX_NAME: usize = 255;
pub struct Entry {
    pub name: String,
    pub directory: bool,
    pub size: u64,
}
pub trait Storage {
    fn roots(&self) -> Vec<Entry>;
    fn list(&self, path: &str) -> io::Result<Vec<Entry>>;
    fn resolve(&self, path: &str) -> io::Result<PathBuf>;
}
pub fn serve<S: Storage>(
    mut stream: TcpStream,
    token: [u8; 32],
    magic: [u8; 4],
    storage: &S,
) -> io::Result<()> {
    stream.set_nodelay(true)?;
    stream.set_nonblocking(false)?;
    let mut header = [0; 38];
    header[..4].copy_from_slice(&magic);
    stream.read_exact(&mut header[4..])?;
    if header[..4] != MAGIC || header[4] != VERSION || header[5..37] != token {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "authentication failed",
        ));
    }
    match header[37] {
        LIST => list(&mut stream, storage),
        DOWNLOAD => download(&mut stream, storage),
        UPLOAD => upload(&mut stream, storage),
        DIRECTORY => directory(&mut stream, storage),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unknown file request",
        )),
    }
}
fn directory<S: Storage>(stream: &mut TcpStream, storage: &S) -> io::Result<()> {
    let parent = match storage.resolve(&read_path(stream, MAX_PATH)?) {
        Ok(path) if path.is_dir() => path,
        _ => return failure(stream),
    };
    let name = match read_path(stream, MAX_NAME) {
        Ok(name) if safe_name(&name) => name,
        _ => return failure(stream),
    };
    let target = unique(&parent, &name);
    if fs::create_dir(&target).is_err() {
        return failure(stream);
    }
    let name = match target.file_name().and_then(OsStr::to_str) {
        Some(name) => name,
        None => return failure(stream),
    };
    stream.write_all(&[0])?;
    write_text(stream, name)
}
fn list<S: Storage>(stream: &mut TcpStream, storage: &S) -> io::Result<()> {
    let path = read_path(stream, MAX_PATH)?;
    let entries = if path.is_empty() {
        storage.roots()
    } else {
        match storage.list(&path) {
            Ok(entries) => entries,
            Err(_) => return failure(stream),
        }
    };
    write_entries(stream, entries)
}
fn write_entries(stream: &mut TcpStream, mut entries: Vec<Entry>) -> io::Result<()> {
    entries.sort_by(|left, right| match right.directory.cmp(&left.directory) {
        Ordering::Equal => left.name.cmp(&right.name),
        order => order,
    });
    entries.truncate(256);
    stream.write_all(&[0])?;
    write_u16(stream, entries.len() as u16)?;
    for entry in entries {
        stream.write_all(&[u8::from(entry.directory)])?;
        write_text(stream, &entry.name)?;
        write_u64(stream, entry.size)?;
    }
    Ok(())
}
fn download<S: Storage>(stream: &mut TcpStream, storage: &S) -> io::Result<()> {
    let path = match storage.resolve(&read_path(stream, MAX_PATH)?) {
        Ok(path) => path,
        Err(_) => return failure(stream),
    };
    let metadata = match fs::metadata(&path) {
        Ok(metadata) if metadata.is_file() => metadata,
        _ => return failure(stream),
    };
    let name = match path.file_name().and_then(OsStr::to_str) {
        Some(name) if name.len() <= MAX_NAME => name,
        _ => return failure(stream),
    };
    stream.write_all(&[0])?;
    write_text(stream, name)?;
    write_u64(stream, metadata.len())?;
    io::copy(&mut File::open(path)?, stream)?;
    Ok(())
}
fn upload<S: Storage>(stream: &mut TcpStream, storage: &S) -> io::Result<()> {
    loop {
        let directory = match read_path(stream, MAX_PATH) {
            Ok(path) => match storage.resolve(&path) {
                Ok(path) if path.is_dir() => path,
                _ => return failure(stream),
            },
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(_) => return failure(stream),
        };
        let name = match read_path(stream, MAX_NAME) {
            Ok(name) if safe_name(&name) => name,
            _ => return failure(stream),
        };
        let length = read_u64(stream)?;
        let target = unique(&directory, &name);
        let temporary = temporary(&directory, &name)?;
        let mut output = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
        {
            Ok(file) => file,
            Err(_) => return failure(stream),
        };
        stream.write_all(&[0])?;
        let result = copy_exact(stream, &mut output, length).and_then(|_| output.sync_all());
        drop(output);
        if result.is_ok() && fs::rename(&temporary, &target).is_ok() {
            stream.write_all(&[0])?;
        } else {
            let _ = fs::remove_file(temporary);
            return failure(stream);
        }
    }
}
pub fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name != "."
        && name != ".."
        && !name.contains('/')
        && !name.contains('\\')
        && !name.contains(':')
}
fn unique(directory: &Path, name: &str) -> PathBuf {
    let path = directory.join(name);
    if !path.exists() {
        return path;
    }
    let stem = Path::new(name)
        .file_stem()
        .and_then(OsStr::to_str)
        .unwrap_or(name);
    let extension = Path::new(name).extension().and_then(OsStr::to_str);
    for index in 1..10_000 {
        let candidate = match extension {
            Some(extension) => format!("{stem} ({index}).{extension}"),
            None => format!("{stem} ({index})"),
        };
        let path = directory.join(candidate);
        if !path.exists() {
            return path;
        }
    }
    directory.join(format!("{stem} ({})", std::process::id()))
}
fn temporary(directory: &Path, name: &str) -> io::Result<PathBuf> {
    for index in 0..10_000 {
        let path = directory.join(format!(".{name}.pendesk-{}-{index}", std::process::id()));
        if !path.exists() {
            return Ok(path);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "could not create temporary file",
    ))
}
fn read_path(stream: &mut TcpStream, limit: usize) -> io::Result<String> {
    let length = usize::from(read_u16(stream)?);
    if length > limit {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "path too long"));
    }
    let mut bytes = vec![0; length];
    stream.read_exact(&mut bytes)?;
    String::from_utf8(bytes).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid path"))
}
fn write_text(stream: &mut TcpStream, value: &str) -> io::Result<()> {
    let bytes = value.as_bytes();
    let length = u16::try_from(bytes.len())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "text too long"))?;
    write_u16(stream, length)?;
    stream.write_all(bytes)
}
fn read_u16(stream: &mut TcpStream) -> io::Result<u16> {
    let mut bytes = [0; 2];
    stream.read_exact(&mut bytes)?;
    Ok(u16::from_be_bytes(bytes))
}
fn write_u16(stream: &mut TcpStream, value: u16) -> io::Result<()> {
    stream.write_all(&value.to_be_bytes())
}
fn read_u64(stream: &mut TcpStream) -> io::Result<u64> {
    let mut bytes = [0; 8];
    stream.read_exact(&mut bytes)?;
    Ok(u64::from_be_bytes(bytes))
}
fn write_u64(stream: &mut TcpStream, value: u64) -> io::Result<()> {
    stream.write_all(&value.to_be_bytes())
}
fn copy_exact(input: &mut TcpStream, output: &mut File, mut length: u64) -> io::Result<()> {
    let mut buffer = [0; 65_536];
    while length != 0 {
        let count = usize::try_from(length.min(buffer.len() as u64)).unwrap();
        input.read_exact(&mut buffer[..count])?;
        output.write_all(&buffer[..count])?;
        length -= count as u64;
    }
    Ok(())
}
fn failure(stream: &mut TcpStream) -> io::Result<()> {
    stream.write_all(&[1])
}
