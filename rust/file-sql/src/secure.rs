// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs::File,
    path::{Component, Path},
};
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Identity {
    dev: u64,
    ino: u64,
    size: u64,
    mtime: i64,
    mtime_ns: i64,
    ctime: i64,
    ctime_ns: i64,
}
#[cfg(unix)]
fn identity(file: &File) -> Result<Identity> {
    use std::os::unix::fs::MetadataExt;
    let m = file.metadata()?;
    ensure!(m.is_file(), "only regular files are supported");
    Ok(Identity {
        dev: m.dev(),
        ino: m.ino(),
        size: m.len(),
        mtime: m.mtime(),
        mtime_ns: m.mtime_nsec(),
        ctime: m.ctime(),
        ctime_ns: m.ctime_nsec(),
    })
}
fn relative(root: &str, path: &str) -> Result<(String, Vec<String>)> {
    ensure!(
        !root.is_empty() && !root.eq_ignore_ascii_case("null"),
        "secure_file_priv denies access"
    );
    ensure!(
        !root.contains('\0') && !path.contains('\0'),
        "NUL in file path"
    );
    ensure!(
        Path::new(root).is_absolute(),
        "secure_file_priv must be absolute"
    );
    let mut root_parts = Vec::new();
    for component in Path::new(root).components() {
        match component {
            Component::RootDir => (),
            Component::Normal(c) => root_parts.push(c.to_str().context("invalid path UTF8")?),
            _ => bail!("invalid secure root"),
        }
    }
    let normalized = format!("/{}", root_parts.join("/"));
    let relative = if Path::new(path).is_absolute() {
        Path::new(path)
            .strip_prefix(&normalized)
            .context("path outside secure_file_priv")?
    } else {
        Path::new(path)
    };
    let mut parts = Vec::new();
    for component in relative.components() {
        match component {
            Component::Normal(c) => parts.push(c.to_str().context("invalid path UTF8")?.to_owned()),
            _ => bail!("unsafe file path component"),
        }
    }
    ensure!(!parts.is_empty(), "file name required");
    Ok((normalized, parts))
}
#[cfg(unix)]
pub fn parent(root: &str, path: &str) -> Result<(File, String, String)> {
    use std::{
        ffi::CString,
        os::fd::{AsRawFd, FromRawFd},
    };
    let (root, mut parts) = relative(root, path)?;
    let mut directory = File::open("/")?;
    for part in Path::new(&root)
        .components()
        .filter_map(|c| {
            if let Component::Normal(v) = c {
                v.to_str()
            } else {
                None
            }
        })
        .chain(parts[..parts.len() - 1].iter().map(String::as_str))
    {
        let name = CString::new(part)?;
        let fd = unsafe {
            libc::openat(
                directory.as_raw_fd(),
                name.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        ensure!(
            fd >= 0,
            "cannot securely open directory: {}",
            std::io::Error::last_os_error()
        );
        directory = unsafe { File::from_raw_fd(fd) };
    }
    let name = parts.pop().context("file name required")?;
    let full = format!(
        "{}/{}",
        root.trim_end_matches('/'),
        parts
            .iter()
            .chain(std::iter::once(&name))
            .cloned()
            .collect::<Vec<_>>()
            .join("/")
    );
    Ok((directory, name, full))
}
#[cfg(unix)]
pub fn open(root: &str, path: &str) -> Result<(File, Identity)> {
    use std::{
        ffi::CString,
        os::fd::{AsRawFd, FromRawFd},
    };
    let (parent, name, _) = parent(root, path)?;
    let name = CString::new(name)?;
    let fd = unsafe {
        libc::openat(
            parent.as_raw_fd(),
            name.as_ptr(),
            libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
        )
    };
    ensure!(
        fd >= 0,
        "cannot securely open file: {}",
        std::io::Error::last_os_error()
    );
    let file = unsafe { File::from_raw_fd(fd) };
    let identity = identity(&file)?;
    Ok((file, identity))
}
pub fn verify(root: &str, path: &str, expected: &Identity) -> Result<()> {
    let (_, actual) = open(root, path)?;
    ensure!(&actual == expected, "file changed since schema inference");
    Ok(())
}
#[cfg(unix)]
pub fn discover(root: &str, pattern: &str) -> Result<Vec<String>> {
    use std::{
        ffi::{CStr, CString},
        os::fd::AsRawFd,
    };
    let (dir, mask, full) = parent(root, pattern)?;
    ensure!(!mask.contains("**"), "recursive glob not supported");
    let pattern = glob::Pattern::new(&mask)?;
    let fd = unsafe { libc::dup(dir.as_raw_fd()) };
    ensure!(fd >= 0, "cannot duplicate directory");
    let stream = unsafe { libc::fdopendir(fd) };
    if stream.is_null() {
        unsafe { libc::close(fd) };
        bail!("cannot enumerate directory")
    }
    struct Dir(*mut libc::DIR);
    impl Drop for Dir {
        fn drop(&mut self) {
            unsafe {
                libc::closedir(self.0);
            }
        }
    }
    let guard = Dir(stream);
    let mut paths = Vec::new();
    loop {
        let entry = unsafe { libc::readdir(guard.0) };
        if entry.is_null() {
            break;
        }
        let name = unsafe { CStr::from_ptr((*entry).d_name.as_ptr()) }
            .to_str()?
            .to_owned();
        if name == "." || name == ".." || !pattern.matches(&name) {
            continue;
        }
        let cname = CString::new(name.clone())?;
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        ensure!(
            unsafe {
                libc::fstatat(
                    dir.as_raw_fd(),
                    cname.as_ptr(),
                    &mut st,
                    libc::AT_SYMLINK_NOFOLLOW,
                )
            } == 0,
            "file changed during glob"
        );
        ensure!(
            st.st_mode & libc::S_IFMT != libc::S_IFLNK,
            "symbolic links not allowed"
        );
        if st.st_mode & libc::S_IFMT == libc::S_IFREG {
            ensure!(paths.len() < 10000, "glob result limit exceeded");
            paths.push(format!(
                "{}/{}",
                Path::new(&full)
                    .parent()
                    .context("invalid glob parent")?
                    .display(),
                name
            ));
        }
    }
    paths.sort();
    Ok(paths)
}
#[cfg(not(unix))]
pub fn open(_: &str, _: &str) -> Result<(File, Identity)> {
    bail!("File SQL is not supported on this platform")
}
#[cfg(not(unix))]
pub fn discover(_: &str, _: &str) -> Result<Vec<String>> {
    bail!("File SQL is not supported on this platform")
}
