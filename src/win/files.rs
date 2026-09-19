use crate::all::files::{Entry, Storage, safe_name};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use windows::Win32::Storage::FileSystem::GetLogicalDrives;
pub struct Disk;
impl Storage for Disk {
    fn roots(&self) -> Vec<Entry> {
        let mask = unsafe { GetLogicalDrives() };
        let mut entries = Vec::new();
        for index in 0..26u32 {
            if mask & 1 << index != 0 {
                entries.push(Entry {
                    name: format!("{}:", char::from(b'A' + index as u8)),
                    directory: true,
                    size: 0,
                });
            }
        }
        entries
    }
    fn list(&self, value: &str) -> io::Result<Vec<Entry>> {
        let (root, path) = existing(value)?;
        if !path.is_dir() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid path"));
        }
        let entries = fs::read_dir(path)?
            .filter_map(Result::ok)
            .filter_map(|entry| entry_data(&root, entry.path(), entry.file_name()).ok())
            .collect();
        Ok(entries)
    }
    fn resolve(&self, value: &str) -> io::Result<PathBuf> {
        existing(value).map(|(_, path)| path)
    }
}
fn existing(value: &str) -> io::Result<(PathBuf, PathBuf)> {
    let mut parts = value.split('/');
    let drive = parts
        .next()
        .filter(|drive| {
            let bytes = drive.as_bytes();
            bytes.len() == 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':'
        })
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
    let root = PathBuf::from(format!("{drive}\\")).canonicalize()?;
    let mut path = root.clone();
    for name in parts {
        if !safe_name(name) {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid path"));
        }
        path.push(name);
    }
    let path = path.canonicalize()?;
    if path.starts_with(&root) {
        Ok((root, path))
    } else {
        Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "outside root",
        ))
    }
}
fn entry_data(root: &Path, path: PathBuf, name: std::ffi::OsString) -> io::Result<Entry> {
    let metadata = fs::symlink_metadata(&path)?;
    if metadata.file_type().is_symlink() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unsupported entry",
        ));
    }
    let canonical = path.canonicalize()?;
    if !canonical.starts_with(root) {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "outside root",
        ));
    }
    let name = name
        .into_string()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid filename"))?;
    if name.starts_with('.') || name.len() > 255 || (!metadata.is_file() && !metadata.is_dir()) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unsupported entry",
        ));
    }
    Ok(Entry {
        name,
        directory: metadata.is_dir(),
        size: metadata.len(),
    })
}
