// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
use super::*;
use std::io::Write;
pub struct CopyWriter {
    output: Option<Output>,
    options: Options,
    columns: usize,
    error: String,
}
#[cfg(unix)]
struct Output {
    file: Option<File>,
    directory: File,
    temp: std::ffi::CString,
    target: std::ffi::CString,
    root: String,
    path: String,
    committed: bool,
}
#[cfg(unix)]
impl Output {
    fn create(root: &str, path: &str) -> Result<Self> {
        use std::{
            ffi::CString,
            os::fd::{AsRawFd, FromRawFd},
            sync::atomic::{AtomicU64, Ordering},
        };
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let (directory, name, _) = secure::parent(root, path)?;
        let target = CString::new(name)?;
        let mut stat: libc::stat = unsafe { std::mem::zeroed() };
        let exists = unsafe {
            libc::fstatat(
                directory.as_raw_fd(),
                target.as_ptr(),
                &mut stat,
                libc::AT_SYMLINK_NOFOLLOW,
            )
        };
        ensure!(
            exists != 0 && std::io::Error::last_os_error().raw_os_error() == Some(libc::ENOENT),
            "COPY target already exists or cannot be inspected"
        );
        let temp = CString::new(format!(
            ".seekdb-copy-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ))?;
        let fd = unsafe {
            libc::openat(
                directory.as_raw_fd(),
                temp.as_ptr(),
                libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL | libc::O_CLOEXEC | libc::O_NOFOLLOW,
                0o600,
            )
        };
        ensure!(
            fd >= 0,
            "cannot create COPY temporary file: {}",
            std::io::Error::last_os_error()
        );
        Ok(Self {
            file: Some(unsafe { File::from_raw_fd(fd) }),
            directory,
            temp,
            target,
            root: root.into(),
            path: path.into(),
            committed: false,
        })
    }
    fn commit(&mut self) -> Result<()> {
        use std::{os::fd::AsRawFd, os::unix::fs::MetadataExt};
        ensure!(!self.committed, "COPY already committed");
        self.file.as_ref().context("COPY closed")?.sync_all()?;
        let (current, _, _) = secure::parent(&self.root, &self.path)?;
        let a = current.metadata()?;
        let b = self.directory.metadata()?;
        ensure!(
            a.dev() == b.dev() && a.ino() == b.ino(),
            "COPY directory changed"
        );
        ensure!(
            unsafe {
                libc::linkat(
                    self.directory.as_raw_fd(),
                    self.temp.as_ptr(),
                    self.directory.as_raw_fd(),
                    self.target.as_ptr(),
                    0,
                )
            } == 0,
            "COPY publish failed (target must not exist): {}",
            std::io::Error::last_os_error()
        );
        self.committed = true;
        self.file.take();
        ensure!(
            unsafe { libc::unlinkat(self.directory.as_raw_fd(), self.temp.as_ptr(), 0) } == 0,
            "COPY temporary unlink failed"
        );
        Ok(())
    }
}
#[cfg(unix)]
impl Drop for Output {
    fn drop(&mut self) {
        use std::os::fd::AsRawFd;
        self.file.take();
        unsafe {
            libc::unlinkat(self.directory.as_raw_fd(), self.temp.as_ptr(), 0);
        }
    }
}
#[cfg(not(unix))]
struct Output {
    file: Option<File>,
}
#[cfg(not(unix))]
impl Output {
    fn create(_: &str, _: &str) -> Result<Self> {
        bail!("COPY unsupported on this platform")
    }
    fn commit(&mut self) -> Result<()> {
        bail!("COPY unsupported on this platform")
    }
}
impl CopyWriter {
    pub(super) fn open(root: &str, path: &str, options: &str, names: &[String]) -> Result<Self> {
        let mut json: Value = serde_json::from_str(options)?;
        let object = json
            .as_object_mut()
            .context("COPY options must be object")?;
        let format_keys: Vec<String> = object
            .keys()
            .filter(|k| k.eq_ignore_ascii_case("format"))
            .cloned()
            .collect();
        ensure!(format_keys.len() <= 1, "duplicate COPY format option");
        if let Some(key) = format_keys.first() {
            let format = object.remove(key).context("missing COPY format")?;
            ensure!(
                format
                    .as_str()
                    .is_some_and(|s| s.eq_ignore_ascii_case("csv")),
                "COPY supports CSV only"
            );
        }
        let options = Options::parse(&json.to_string())?;
        ensure!(
            !options
                .nullstr
                .bytes()
                .any(|b| b == options.delimiter || b == options.quote || b == b'\r' || b == b'\n'),
            "COPY NULL text cannot contain delimiter, quote or newline"
        );
        let header = options.header;
        let output = Output::create(root, path)?;
        let mut writer = Self {
            output: Some(output),
            options,
            columns: names.len(),
            error: String::new(),
        };
        if header {
            writer.row(
                &names
                    .iter()
                    .map(|name| ffi::Cell {
                        text: name.clone(),
                        is_null: false,
                    })
                    .collect::<Vec<_>>(),
            )?;
        }
        Ok(writer)
    }
    pub(super) fn row(&mut self, cells: &[ffi::Cell]) -> Result<()> {
        ensure!(cells.len() == self.columns, "COPY column count mismatch");
        let mut record = Vec::new();
        for (i, cell) in cells.iter().enumerate() {
            if i > 0 {
                record.push(self.options.delimiter)
            }
            let value = if cell.is_null {
                &self.options.nullstr
            } else {
                &cell.text
            };
            ensure!(value.len() <= RECORD_LIMIT, "COPY field limit");
            let quoted = !cell.is_null
                && (value == &self.options.nullstr
                    || value.bytes().any(|c| {
                        [self.options.delimiter, self.options.quote, b'\n', b'\r'].contains(&c)
                    }));
            let escaped = if quoted {
                value
                    .bytes()
                    .filter(|&b| {
                        b == self.options.quote
                            || self.options.escape != self.options.quote && b == self.options.escape
                    })
                    .count()
            } else {
                0
            };
            ensure!(
                record.len() + value.len() + escaped + if quoted { 2 } else { 0 } <= RECORD_LIMIT,
                "COPY record limit"
            );
            if quoted {
                record.push(self.options.quote)
            }
            for byte in value.bytes() {
                if quoted
                    && (byte == self.options.quote
                        || self.options.escape != self.options.quote && byte == self.options.escape)
                {
                    record.push(self.options.escape);
                }
                record.push(byte);
            }
            if quoted {
                record.push(self.options.quote)
            }
            ensure!(record.len() <= RECORD_LIMIT, "COPY record limit");
        }
        record.push(b'\n');
        self.output
            .as_mut()
            .context("COPY not open")?
            .file
            .as_mut()
            .context("COPY closed")?
            .write_all(&record)?;
        Ok(())
    }
    pub(super) fn commit(&mut self) -> Result<()> {
        self.output.as_mut().context("COPY not open")?.commit()
    }
}
pub fn open_copy(root: &str, path: &str, options: &str, names: &[String]) -> Box<CopyWriter> {
    Box::new(
        CopyWriter::open(root, path, options, names).unwrap_or_else(|e| CopyWriter {
            output: None,
            options: Options {
                header: false,
                delimiter: b',',
                quote: b'"',
                escape: b'"',
                nullstr: String::new(),
            },
            columns: 0,
            error: format!("{e:#}"),
        }),
    )
}
pub fn copy_error(writer: &CopyWriter) -> &str {
    &writer.error
}
pub fn copy_row(writer: &mut CopyWriter, cells: &[ffi::Cell]) -> String {
    writer
        .row(cells)
        .err()
        .map_or(String::new(), |e| format!("{e:#}"))
}
pub fn copy_commit(writer: &mut CopyWriter) -> String {
    writer
        .commit()
        .err()
        .map_or(String::new(), |e| format!("{e:#}"))
}
