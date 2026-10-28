// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! Bounded local file codecs. This module never parses or executes SQL.
use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    collections::HashSet,
    fs::File,
    io::{BufRead, BufReader, Read},
};
mod copy_writer;
mod parquet_reader;
mod secure;
pub use copy_writer::{copy_commit, copy_error, copy_row, open_copy, CopyWriter};
const RECORD_LIMIT: usize = 16 * 1024 * 1024;
const MEMORY_LIMIT: usize = 64 * 1024 * 1024;
const COLUMN_LIMIT: usize = 4096;

#[cxx::bridge(namespace = "oceanbase::file_sql")]
pub mod ffi {
    struct Binding {
        spec: String,
        names: Vec<String>,
        kinds: Vec<u8>,
        nullable: Vec<u8>,
        scales: Vec<u8>,
        row_count: u64,
        error: String,
    }
    struct Cell {
        text: String,
        is_null: bool,
    }
    struct Batch {
        cells: Vec<Cell>,
        rows: usize,
        eof: bool,
        error: String,
    }
    extern "Rust" {
        type FileScan;
        type CopyWriter;
        fn open_copy(root: &str, path: &str, options: &str, names: &[String]) -> Box<CopyWriter>;
        fn copy_error(writer: &CopyWriter) -> &str;
        fn copy_row(writer: &mut CopyWriter, cells: &[Cell]) -> String;
        fn copy_commit(writer: &mut CopyWriter) -> String;
        fn file_bind_deadline(
            root: &str,
            path: &str,
            format: &str,
            options: &str,
            budget_us: u64,
        ) -> Binding;
        fn file_bind(root: &str, path: &str, format: &str, options: &str) -> Binding;
        fn open_file_scan(root: &str, spec: &str, columns: &[usize]) -> Box<FileScan>;
        fn scan_error(scan: &FileScan) -> &str;
        fn scan_next(scan: &mut FileScan, max_rows: usize) -> Batch;
        fn scan_rescan(scan: &mut FileScan) -> String;
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct Options {
    header: bool,
    delimiter: u8,
    quote: u8,
    escape: u8,
    nullstr: String,
}
impl Options {
    fn parse(text: &str) -> Result<Self> {
        let value: Value = serde_json::from_str(text)?;
        let object = value.as_object().context("options must be an object")?;
        let mut options = Self {
            header: true,
            delimiter: b',',
            quote: b'"',
            escape: b'"',
            nullstr: String::new(),
        };
        let mut seen = HashSet::new();
        for (name, value) in object {
            let key = match name.to_ascii_lowercase().as_str() {
                "delim" => "delimiter".to_owned(),
                "null" => "nullstr".to_owned(),
                other => other.to_owned(),
            };
            ensure!(seen.insert(key.clone()), "duplicate option {key}");
            match key.as_str() {
                "format" => ensure!(
                    value.as_str() == Some("newline_delimited"),
                    "JSON format only supports newline_delimited"
                ),
                "header" => options.header = value.as_bool().context("header must be boolean")?,
                "sample_size" => {
                    ensure!(value.as_i64() == Some(-1), "sample_size only supports -1")
                }
                "nullstr" => {
                    options.nullstr = value.as_str().context("nullstr must be text")?.to_owned()
                }
                "delimiter" | "quote" | "escape" => {
                    let s = value
                        .as_str()
                        .context("CSV character option must be text")?;
                    ensure!(
                        s.len() == 1
                            && s.as_bytes()[0] != 0
                            && s.as_bytes()[0] != b'\n'
                            && s.as_bytes()[0] != b'\r',
                        "CSV option must be one non-newline byte"
                    );
                    match key.as_str() {
                        "delimiter" => options.delimiter = s.as_bytes()[0],
                        "quote" => options.quote = s.as_bytes()[0],
                        _ => options.escape = s.as_bytes()[0],
                    }
                }
                _ => bail!("unknown file option {name}"),
            }
        }
        ensure!(
            options.delimiter != options.quote && options.delimiter != options.escape,
            "CSV delimiter conflicts with quote/escape"
        );
        Ok(options)
    }
}
// Kind discriminants are part of the C++ bridge contract, not SQL persisted metadata.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
enum Kind {
    Null = 0,
    Bool = 1,
    Int = 2,
    Double = 3,
    Date = 4,
    DateTime = 5,
    Text = 6,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct Column {
    name: String,
    kind: Kind,
    exact_int: bool,
    nullable: bool,
    scale: u8,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
struct Spec {
    path: String,
    format: String,
    options: Options,
    identity: secure::Identity,
    columns: Vec<Column>,
    rows: u64,
}
#[derive(Clone)]
struct Scalar {
    text: String,
    kind: Kind,
    exact: bool,
}
fn date_kind(s: &str) -> Option<Kind> {
    if s.len() == 10 && chrono::NaiveDate::parse_from_str(s, "%Y-%m-%d").is_ok() {
        Some(Kind::Date)
    } else if s.len() >= 19
        && ["%Y-%m-%d %H:%M:%S%.f", "%Y-%m-%dT%H:%M:%S%.f"]
            .iter()
            .any(|fmt| chrono::NaiveDateTime::parse_from_str(s, fmt).is_ok())
    {
        Some(Kind::DateTime)
    } else {
        None
    }
}
fn classify(s: String) -> Scalar {
    let mut exact = true;
    let kind = if s.eq_ignore_ascii_case("true") || s.eq_ignore_ascii_case("false") {
        Kind::Bool
    } else if !s.is_empty()
        && s.bytes()
            .enumerate()
            .all(|(i, b)| b.is_ascii_digit() || i == 0 && (b == b'-' || b == b'+'))
    {
        match s.parse::<i64>() {
            Ok(v) => {
                exact = (v as f64) as i128 == v as i128;
                Kind::Int
            }
            Err(_) => Kind::Text,
        }
    } else if s.contains(['.', 'e', 'E'])
        && s.trim() == s
        && s.parse::<f64>().is_ok_and(f64::is_finite)
    {
        Kind::Double
    } else {
        date_kind(&s).unwrap_or(Kind::Text)
    };
    Scalar {
        text: s,
        kind,
        exact,
    }
}
fn merge(column: &mut Column, value: &Scalar) {
    use Kind::*;
    column.nullable |= value.kind == Kind::Null;
    if value.kind == Kind::DateTime {
        // DATETIME supports up to microseconds. Infer across every row, including
        // Date -> DateTime promotion and fractional values beyond the first row.
        let scale = value.text.split_once('.').map_or(0, |(_, fraction)| {
            fraction
                .bytes()
                .take_while(u8::is_ascii_digit)
                .count()
                .min(6) as u8
        });
        column.scale = column.scale.max(scale);
    }
    column.exact_int &= value.exact;
    column.kind = match (column.kind, value.kind) {
        (Null, t) | (t, Null) => t,
        (a, b) if a == b => a,
        (Int, Double) | (Double, Int) => Double,
        (Date, DateTime) | (DateTime, Date) => DateTime,
        _ => Text,
    };
    if column.kind == Double && !column.exact_int {
        column.kind = Text;
    }
}
struct CsvReader {
    input: BufReader<File>,
    options: Options,
    line: u64,
    first: bool,
}
impl CsvReader {
    fn new(file: File, options: Options) -> Self {
        Self {
            input: BufReader::with_capacity(64 * 1024, file),
            options,
            line: 0,
            first: true,
        }
    }
    fn byte(&mut self) -> Result<Option<u8>> {
        let mut b = [0];
        Ok(if self.input.read(&mut b)? == 0 {
            None
        } else {
            Some(b[0])
        })
    }
    fn peek(&mut self) -> Result<Option<u8>> {
        Ok(self.input.fill_buf()?.first().copied())
    }
    fn record(&mut self) -> Result<Option<Vec<Scalar>>> {
        if self.first {
            use std::io::{Seek, SeekFrom};
            self.first = false;
            let mut prefix = [0; 3];
            let length = self.input.read(&mut prefix)?;
            if length != 3 || prefix != [0xef, 0xbb, 0xbf] {
                self.input.seek(SeekFrom::Start(0))?;
            }
        }
        let mut result = Vec::new();
        let mut bytes = Vec::new();
        let mut quoted = false;
        let mut inside = false;
        let mut closed = false;
        let mut any = false;
        let mut total = 0;
        loop {
            let b = self.byte()?;
            if b.is_none() && !any {
                return Ok(None);
            }
            if b.is_none() {
                ensure!(!inside, "CSV record {}: unterminated quote", self.line + 1);
            }
            if let Some(c) = b {
                any = true;
                total += 1;
                ensure!(
                    total <= RECORD_LIMIT,
                    "CSV record {} exceeds byte limit",
                    self.line + 1
                );
                if inside {
                    if c == self.options.escape && self.options.escape != self.options.quote {
                        let next = self.byte()?.context("CSV truncated escape")?;
                        bytes.push(next);
                        total += 1;
                        continue;
                    }
                    if c == self.options.quote {
                        if self.options.escape == self.options.quote && self.peek()? == Some(c) {
                            self.byte()?;
                            bytes.push(c);
                            total += 1;
                            continue;
                        }
                        inside = false;
                        closed = true;
                        continue;
                    }
                    bytes.push(c);
                    continue;
                }
                if !closed && bytes.is_empty() && c == self.options.quote {
                    quoted = true;
                    inside = true;
                    continue;
                }
                if c != self.options.delimiter && c != b'\n' && c != b'\r' {
                    ensure!(
                        !closed,
                        "CSV record {}: data after closing quote",
                        self.line + 1
                    );
                    bytes.push(c);
                    continue;
                }
            }
            let text =
                String::from_utf8(std::mem::take(&mut bytes)).context("CSV invalid UTF-8")?;
            let value = if !quoted && text == self.options.nullstr {
                Scalar {
                    text,
                    kind: Kind::Null,
                    exact: true,
                }
            } else {
                classify(text)
            };
            result.push(value);
            ensure!(result.len() <= COLUMN_LIMIT, "too many CSV columns");
            quoted = false;
            closed = false;
            if b != Some(self.options.delimiter) {
                if b == Some(b'\r') && self.peek()? == Some(b'\n') {
                    self.byte()?;
                }
                self.line += 1;
                return Ok(Some(result));
            }
        }
    }
}
fn json_record(
    input: &mut BufReader<File>,
    line: &mut u64,
) -> Result<Option<Vec<(String, Scalar)>>> {
    loop {
        let mut data = Vec::new();
        let mut limited = std::io::Read::by_ref(input).take((RECORD_LIMIT + 1) as u64);
        let count = limited.read_until(b'\n', &mut data)?;
        if count == 0 {
            return Ok(None);
        }
        *line += 1;
        ensure!(
            count <= RECORD_LIMIT,
            "JSONL line {} exceeds byte limit",
            line
        );
        let text = std::str::from_utf8(&data).context("JSONL invalid UTF-8")?;
        if text.trim().is_empty() {
            continue;
        }
        // Detect duplicate keys before Value's map can overwrite them.
        struct ObjectVisitor;
        impl<'de> serde::de::Visitor<'de> for ObjectVisitor {
            type Value = Vec<(String, Box<serde_json::value::RawValue>)>;
            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                f.write_str("a flat JSON object")
            }
            fn visit_map<M: serde::de::MapAccess<'de>>(
                self,
                mut map: M,
            ) -> std::result::Result<Self::Value, M::Error> {
                let mut result = Vec::new();
                let mut seen = HashSet::new();
                while let Some((key, value)) =
                    map.next_entry::<String, Box<serde_json::value::RawValue>>()?
                {
                    if !seen.insert(key.clone()) {
                        return Err(serde::de::Error::custom("duplicate JSON key"));
                    }
                    if result.len() >= COLUMN_LIMIT {
                        return Err(serde::de::Error::custom("too many JSON columns"));
                    }
                    result.push((key, value));
                }
                Ok(result)
            }
        }
        use serde::Deserializer;
        let mut decoder = serde_json::Deserializer::from_str(text);
        let object = decoder
            .deserialize_map(ObjectVisitor)
            .with_context(|| format!("JSONL line {line}"))?;
        decoder.end()?;
        drop(decoder);
        drop(data);
        let mut result = Vec::new();
        for (key, raw) in object {
            ensure!(key.len() <= 1024, "JSONL column name limit");
            ensure!(
                !raw.get().trim_start().starts_with(['{', '[']),
                "JSONL line {line}, field {key}: nested values not supported"
            );
            let value: Value = serde_json::from_str(raw.get())?;
            let scalar = match value {
                Value::Null => Scalar {
                    text: String::new(),
                    kind: Kind::Null,
                    exact: true,
                },
                Value::Bool(v) => Scalar {
                    text: v.to_string(),
                    kind: Kind::Bool,
                    exact: true,
                },
                Value::String(v) => Scalar {
                    text: v,
                    kind: Kind::Text,
                    exact: true,
                },
                Value::Number(n) => classify(n.to_string()),
                _ => bail!("JSONL line {line}, field {key}: nested values not supported"),
            };
            result.push((key, scalar));
        }
        return Ok(Some(result));
    }
}
fn unique_names(names: Vec<String>) -> Vec<String> {
    let mut seen = HashSet::new();
    names
        .into_iter()
        .enumerate()
        .map(|(i, name)| {
            let base = if name.is_empty() {
                format!("column{}", i + 1)
            } else {
                name
            };
            let mut candidate = base.clone();
            let mut suffix = 2;
            while !seen.insert(candidate.to_lowercase()) {
                candidate = format!("{base}__{suffix}");
                suffix += 1;
            }
            candidate
        })
        .collect()
}
fn bind_file(root: &str, path: &str, format: &str, options: &str) -> Result<ffi::Binding> {
    bind_file_budget(root, path, format, options, 60_000_000)
}
fn bind_file_budget(
    root: &str,
    path: &str,
    format: &str,
    options: &str,
    budget_us: u64,
) -> Result<ffi::Binding> {
    let started = std::time::Instant::now();
    let check_time = || -> Result<()> {
        ensure!(
            started.elapsed().as_micros() < u128::from(budget_us),
            "file schema inference deadline exceeded"
        );
        Ok(())
    };
    check_time()?;
    let options = Options::parse(options)?;
    let format = format.to_ascii_lowercase();
    if format == "glob" {
        let paths = secure::discover(root, path)?;
        let spec = Spec {
            path: path.to_owned(),
            format,
            options,
            identity: secure::Identity::default(),
            columns: vec![Column {
                name: "file".into(),
                kind: Kind::Text,
                exact_int: true,
                nullable: false,
                scale: 0,
            }],
            rows: paths.len() as u64,
        };
        return binding(spec);
    }
    let (file, identity) = secure::open(root, path)?;
    let mut spec = Spec {
        path: path.into(),
        format: format.clone(),
        options: options.clone(),
        identity,
        columns: Vec::new(),
        rows: 0,
    };
    match format.as_str() {
        "csv" => {
            let mut reader = CsvReader::new(file, options);
            let first = reader.record()?.context("empty file has no schema")?;
            let names: Vec<String> = if spec.options.header {
                first.iter().map(|v| v.text.clone()).collect()
            } else {
                (0..first.len())
                    .map(|i| format!("column{}", i + 1))
                    .collect()
            };
            ensure!(
                names.iter().all(|name: &String| name.len() <= 1024),
                "CSV column name limit"
            );
            spec.columns = unique_names(names)
                .into_iter()
                .map(|name| Column {
                    name,
                    kind: Kind::Null,
                    exact_int: true,
                    nullable: false,
                    scale: 0,
                })
                .collect();
            if !spec.options.header {
                for (c, v) in spec.columns.iter_mut().zip(&first) {
                    merge(c, v)
                }
                spec.rows += 1;
            }
            while let Some(row) = reader.record()? {
                check_time()?;
                ensure!(
                    row.len() == spec.columns.len(),
                    "CSV record {}: expected {} fields, got {}",
                    reader.line,
                    spec.columns.len(),
                    row.len()
                );
                for (c, v) in spec.columns.iter_mut().zip(&row) {
                    merge(c, v)
                }
                spec.rows += 1;
            }
        }
        "json" | "ndjson" => {
            let mut input = BufReader::new(file);
            let mut line = 0;
            while let Some(row) = json_record(&mut input, &mut line)? {
                check_time()?;
                for column in &mut spec.columns {
                    if !row.iter().any(|(name, _)| name == &column.name) {
                        column.nullable = true;
                    }
                }
                for (name, value) in row {
                    let index = if let Some(i) = spec.columns.iter().position(|c| c.name == name) {
                        i
                    } else {
                        ensure!(spec.columns.len() < COLUMN_LIMIT, "too many JSON columns");
                        spec.columns.push(Column {
                            name,
                            kind: Kind::Null,
                            exact_int: true,
                            nullable: spec.rows > 0,
                            scale: 0,
                        });
                        spec.columns.len() - 1
                    };
                    merge(&mut spec.columns[index], &value);
                }
                spec.rows += 1;
            }
            ensure!(!spec.columns.is_empty(), "JSONL has no columns");
            let mut seen = HashSet::new();
            for col in &spec.columns {
                ensure!(
                    seen.insert(col.name.to_lowercase()),
                    "JSON keys conflict ignoring case"
                );
            }
        }
        "parquet" => {
            let (columns, rows) = parquet_reader::schema(file)?;
            spec.columns = columns;
            spec.rows = rows;
        }
        _ => bail!("unknown file format {format}"),
    }
    check_time()?;
    secure::verify(root, path, &spec.identity)?;
    binding(spec)
}
fn binding(spec: Spec) -> Result<ffi::Binding> {
    Ok(ffi::Binding {
        names: spec.columns.iter().map(|c| c.name.clone()).collect(),
        kinds: spec.columns.iter().map(|c| c.kind as u8).collect(),
        nullable: spec.columns.iter().map(|c| u8::from(c.nullable)).collect(),
        scales: spec.columns.iter().map(|c| c.scale).collect(),
        row_count: spec.rows,
        spec: serde_json::to_string(&spec)?,
        error: String::new(),
    })
}
pub fn file_bind_deadline(
    root: &str,
    path: &str,
    format: &str,
    options: &str,
    budget_us: u64,
) -> ffi::Binding {
    bind_file_budget(root, path, format, options, budget_us).unwrap_or_else(|e| ffi::Binding {
        spec: String::new(),
        names: Vec::new(),
        kinds: Vec::new(),
        nullable: Vec::new(),
        scales: Vec::new(),
        row_count: 0,
        error: format!("{path}: {e:#}"),
    })
}
pub fn file_bind(root: &str, path: &str, format: &str, options: &str) -> ffi::Binding {
    bind_file(root, path, format, options).unwrap_or_else(|e| ffi::Binding {
        spec: String::new(),
        names: Vec::new(),
        kinds: Vec::new(),
        nullable: Vec::new(),
        scales: Vec::new(),
        row_count: 0,
        error: format!("{path}: {e:#}"),
    })
}
enum Reader {
    Csv(CsvReader),
    Json(BufReader<File>, u64),
    Parquet(parquet_reader::Reader),
    Glob(Vec<String>, usize),
}
pub struct FileScan {
    root: String,
    spec: Option<Spec>,
    columns: Vec<usize>,
    reader: Option<Reader>,
    error: String,
}
fn open_scan(root: &str, spec: &str, columns: &[usize]) -> Result<Box<FileScan>> {
    let spec: Spec = serde_json::from_str(spec)?;
    ensure!(
        columns.iter().all(|&i| i < spec.columns.len()),
        "invalid file column index"
    );
    let mut scan = Box::new(FileScan {
        root: root.into(),
        spec: Some(spec),
        columns: columns.to_vec(),
        reader: None,
        error: String::new(),
    });
    scan.rescan()?;
    Ok(scan)
}
pub fn open_file_scan(root: &str, spec: &str, columns: &[usize]) -> Box<FileScan> {
    open_scan(root, spec, columns).unwrap_or_else(|e| {
        Box::new(FileScan {
            root: root.into(),
            spec: None,
            columns: Vec::new(),
            reader: None,
            error: format!("{e:#}"),
        })
    })
}
pub fn scan_error(scan: &FileScan) -> &str {
    &scan.error
}
pub fn scan_rescan(scan: &mut FileScan) -> String {
    scan.rescan()
        .err()
        .map_or(String::new(), |e| format!("{e:#}"))
}
pub fn scan_next(scan: &mut FileScan, max_rows: usize) -> ffi::Batch {
    scan.next_batch(max_rows).unwrap_or_else(|e| ffi::Batch {
        cells: Vec::new(),
        rows: 0,
        eof: false,
        error: format!("{e:#}"),
    })
}
impl FileScan {
    fn rescan(&mut self) -> Result<()> {
        let spec = self.spec.as_ref().context("scan not initialized")?;
        if spec.format == "glob" {
            self.reader = Some(Reader::Glob(secure::discover(&self.root, &spec.path)?, 0));
            return Ok(());
        }
        let (file, identity) = secure::open(&self.root, &spec.path)?;
        ensure!(
            identity == spec.identity,
            "file changed since schema inference"
        );
        self.reader = Some(match spec.format.as_str() {
            "csv" => {
                let mut reader = CsvReader::new(file, spec.options.clone());
                if spec.options.header {
                    reader.record()?;
                }
                Reader::Csv(reader)
            }
            "json" | "ndjson" => Reader::Json(BufReader::new(file), 0),
            "parquet" => Reader::Parquet(parquet_reader::Reader::new(file, &self.columns)?),
            _ => bail!("unknown format"),
        });
        Ok(())
    }
    fn next_batch(&mut self, max_rows: usize) -> Result<ffi::Batch> {
        ensure!(max_rows > 0 && max_rows <= 65536, "invalid batch size");
        let spec = self.spec.as_ref().context("scan not initialized")?;
        if spec.format != "glob" {
            secure::verify(&self.root, &spec.path, &spec.identity)?;
        }
        let mut batch = ffi::Batch {
            cells: Vec::new(),
            rows: 0,
            eof: false,
            error: String::new(),
        };
        let mut bytes = 0;
        for _ in 0..max_rows {
            let row = match self.reader.as_mut().context("reader not open")? {
                Reader::Csv(reader) => reader.record()?,
                Reader::Json(input, line) => json_record(input, line)?.map(|values| {
                    spec.columns
                        .iter()
                        .map(|col| {
                            values.iter().find(|(k, _)| k == &col.name).map_or(
                                Scalar {
                                    text: String::new(),
                                    kind: Kind::Null,
                                    exact: true,
                                },
                                |(_, v)| v.clone(),
                            )
                        })
                        .collect()
                }),
                Reader::Parquet(reader) => reader.row()?,
                Reader::Glob(paths, pos) => {
                    if *pos == paths.len() {
                        None
                    } else {
                        let text = paths[*pos].clone();
                        *pos += 1;
                        Some(vec![Scalar {
                            text,
                            kind: Kind::Text,
                            exact: true,
                        }])
                    }
                }
            };
            let Some(row) = row else {
                batch.eof = true;
                break;
            };
            if spec.format == "parquet" {
                for (index, value) in row.into_iter().enumerate() {
                    bytes += value.text.len();
                    ensure!(bytes <= MEMORY_LIMIT, "file batch exceeds memory budget");
                    batch
                        .cells
                        .push(convert(value, spec.columns[self.columns[index]].kind));
                }
            } else {
                ensure!(
                    row.len() == spec.columns.len(),
                    "record column count changed"
                );
                for &col in &self.columns {
                    let value = row[col].clone();
                    bytes += value.text.len();
                    ensure!(bytes <= MEMORY_LIMIT, "file batch exceeds memory budget");
                    batch.cells.push(convert(value, spec.columns[col].kind));
                }
            }
            batch.rows += 1;
            if bytes >= MEMORY_LIMIT / 2 {
                break;
            }
        }
        if spec.format != "glob" {
            secure::verify(&self.root, &spec.path, &spec.identity)?;
        }
        Ok(batch)
    }
}
fn convert(value: Scalar, kind: Kind) -> ffi::Cell {
    let text = match kind {
        Kind::Bool => {
            if value.text.eq_ignore_ascii_case("true") {
                "1".into()
            } else {
                "0".into()
            }
        }
        Kind::DateTime if value.kind == Kind::Date => format!("{} 00:00:00", value.text),
        Kind::DateTime => value.text.replace('T', " "),
        _ => value.text,
    };
    ffi::Cell {
        text,
        is_null: value.kind == Kind::Null,
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    #[test]
    fn official_format_and_duplicate_headers() {
        assert!(Options::parse(r#"{"format":"newline_delimited"}"#).is_ok());
        assert_eq!(
            unique_names(vec!["id".into(), "id".into(), "ID".into()]),
            vec!["id", "id__2", "ID__3"]
        );
    }
    #[test]
    fn csv_describe_metadata_and_null_conventions() {
        let dir = std::env::temp_dir().join(format!("file-sql-contract-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("schema.csv"),
            "id,flag,day,created,text,id\n1,true,2026-08-01,2026-08-01T10:11:12,NULL,11\n2,false,,,\"\",12\n").unwrap();
        let binding = bind_file(dir.to_str().unwrap(), "schema.csv", "csv", "{}").unwrap();
        assert_eq!(
            binding.names,
            vec!["id", "flag", "day", "created", "text", "id__2"]
        );
        assert_eq!(binding.kinds, vec![2, 1, 4, 5, 6, 2]);
        assert_eq!(binding.nullable, vec![0, 0, 1, 1, 0, 0]);
        let mut scan = open_scan(dir.to_str().unwrap(), &binding.spec, &[4]).unwrap();
        let batch = scan.next_batch(10).unwrap();
        assert!(!batch.cells[0].is_null);
        assert_eq!(batch.cells[0].text, "NULL");
        assert!(!batch.cells[1].is_null);
        assert_eq!(batch.cells[1].text, "");
        let binding = bind_file(
            dir.to_str().unwrap(),
            "schema.csv",
            "csv",
            r#"{"nullstr":"NULL"}"#,
        )
        .unwrap();
        assert_eq!(binding.nullable[4], 1);
        fs::write(dir.join("schema.jsonl"), "{\"a\":null}\n{\"b\":true}\n").unwrap();
        let binding = bind_file(
            dir.to_str().unwrap(),
            "schema.jsonl",
            "json",
            r#"{"format":"newline_delimited"}"#,
        )
        .unwrap();
        assert_eq!(binding.nullable, vec![1, 1]);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn timestamp_fractional_precision() {
        let dir = std::env::temp_dir().join(format!("file-sql-scales-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("times.csv"),
            "seconds,mixed,micros\n2026-08-01 10:11:12,2026-08-01,2026-08-01 10:11:12.123456\n2026-08-02T10:11:12,2026-08-02T10:11:12.12,\n2026-08-03 10:11:12,2026-08-03 10:11:12.123,\n").unwrap();
        let bound = bind_file(dir.to_str().unwrap(), "times.csv", "csv", "{}").unwrap();
        assert_eq!(bound.scales, vec![0, 3, 6]);
        let spec: Value = serde_json::from_str(&bound.spec).unwrap();
        assert_eq!(spec["columns"][0]["scale"], 0);
        assert_eq!(spec["columns"][1]["scale"], 3);
        assert_eq!(spec["columns"][2]["scale"], 6);
        let mut scan = open_scan(dir.to_str().unwrap(), &bound.spec, &[2]).unwrap();
        assert_eq!(
            scan.next_batch(1).unwrap().cells[0].text,
            "2026-08-01 10:11:12.123456"
        );
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn csv_schema_and_streaming() {
        let dir = std::env::temp_dir().join(format!("file-sql-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("a.csv"),
            b"id,name,flag\r\n1,\"hello\nworld\",true\r\n2,,false\n",
        )
        .unwrap();
        let binding = bind_file(dir.to_str().unwrap(), "a.csv", "csv", "{}").unwrap();
        assert_eq!(binding.names, vec!["id", "name", "flag"]);
        assert_eq!(binding.kinds, vec![2, 6, 1]);
        assert_eq!(binding.nullable, vec![0, 1, 0]);
        let mut reader = open_scan(dir.to_str().unwrap(), &binding.spec, &[0, 1, 2]).unwrap();
        let batch = reader.next_batch(1).unwrap();
        assert_eq!(batch.rows, 1);
        assert_eq!(batch.cells[1].text, "hello\nworld");
        let batch = reader.next_batch(1).unwrap();
        assert!(batch.cells[1].is_null);
        reader.rescan().unwrap();
        assert_eq!(reader.next_batch(10).unwrap().rows, 2);
        fs::write(dir.join("a.csv"), "changed").unwrap();
        assert!(reader.rescan().is_err());
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn security_and_json_union() {
        let dir = std::env::temp_dir().join(format!("file-sql-json-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("a.jsonl"), "{\"a\":1}\n{\"b\":true,\"a\":1.5}\n").unwrap();
        let binding = bind_file(dir.to_str().unwrap(), "a.jsonl", "json", "{}").unwrap();
        assert_eq!(binding.names, vec!["a", "b"]);
        assert_eq!(binding.kinds, vec![3, 1]);
        assert_eq!(binding.nullable, vec![0, 1]);
        assert!(bind_file(dir.to_str().unwrap(), "../etc/passwd", "csv", "{}").is_err());
        assert!(bind_file("", "a.csv", "csv", "{}").is_err());
        fs::write(dir.join("bad.jsonl"), "{\"a\":[]}\n").unwrap();
        assert!(bind_file(dir.to_str().unwrap(), "bad.jsonl", "json", "{}").is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
#[cfg(test)]
mod parquet_tests {
    use super::*;
    #[test]
    fn timestamp_units_preserve_fractional_precision() {
        use parquet::{
            data_type::Int64Type, file::writer::SerializedFileWriter,
            schema::parser::parse_message_type,
        };
        use std::sync::Arc;
        let dir =
            std::env::temp_dir().join(format!("file-sql-parquet-scales-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let schema = Arc::new(parse_message_type(
            "message test { REQUIRED INT64 ms (TIMESTAMP(MILLIS,false)); REQUIRED INT64 us (TIMESTAMP(MICROS,false)); }").unwrap());
        let mut writer = SerializedFileWriter::new(
            File::create(dir.join("times.parquet")).unwrap(),
            schema,
            Default::default(),
        )
        .unwrap();
        let mut group = writer.next_row_group().unwrap();
        for value in [123_i64, 123456_i64] {
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(&[value], None, None)
                .unwrap();
            column.close().unwrap();
        }
        group.close().unwrap();
        writer.close().unwrap();
        let bound = bind_file(dir.to_str().unwrap(), "times.parquet", "parquet", "{}").unwrap();
        assert_eq!(bound.scales, vec![3, 6]);
        let mut scan = open_scan(dir.to_str().unwrap(), &bound.spec, &[0, 1]).unwrap();
        let row = scan.next_batch(1).unwrap();
        assert_eq!(row.cells[0].text, "1970-01-01 00:00:00.123000");
        assert_eq!(row.cells[1].text, "1970-01-01 00:00:00.123456");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn row_groups_nulls_snappy_and_projection() {
        use parquet::{
            basic::Compression,
            data_type::Int64Type,
            file::{properties::WriterProperties, writer::SerializedFileWriter},
            schema::parser::parse_message_type,
        };
        use std::sync::Arc;
        let dir = std::env::temp_dir().join(format!("file-sql-parquet-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let schema = Arc::new(parse_message_type("message test { OPTIONAL INT64 id; }").unwrap());
        let file = File::create(dir.join("a.parquet")).unwrap();
        let props = Arc::new(
            WriterProperties::builder()
                .set_compression(Compression::SNAPPY)
                .build(),
        );
        let mut writer = SerializedFileWriter::new(file, schema, props).unwrap();
        for value in [1, 3] {
            let mut group = writer.next_row_group().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(&[value], Some(&[1, 0]), None)
                .unwrap();
            column.close().unwrap();
            group.close().unwrap();
        }
        writer.close().unwrap();
        assert!(
            parquet_reader::Reader::new(File::open(dir.join("a.parquet")).unwrap(), &[1]).is_err()
        );
        let binding = bind_file(dir.to_str().unwrap(), "a.parquet", "parquet", "{}").unwrap();
        assert_eq!(binding.kinds, vec![2]);
        assert_eq!(binding.row_count, 4);
        let mut scan = open_scan(dir.to_str().unwrap(), &binding.spec, &[0]).unwrap();
        let batch = scan.next_batch(10).unwrap();
        assert_eq!(batch.rows, 4);
        assert_eq!(batch.cells[0].text, "1");
        assert!(batch.cells[1].is_null);
        assert_eq!(batch.cells[2].text, "3");
        let mut count = open_scan(dir.to_str().unwrap(), &binding.spec, &[]).unwrap();
        assert_eq!(count.next_batch(10).unwrap().rows, 4);
        std::fs::remove_dir_all(dir).unwrap();
    }
}
#[cfg(test)]
mod copy_tests {
    use super::*;
    #[test]
    fn atomic_copy_and_roundtrip() {
        let dir = std::env::temp_dir().join(format!("file-sql-copy-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let names = vec!["id".to_owned(), "text".to_owned()];
        for marker in ["a,b", "a\nb", "a\rb", "a\"b"] {
            let options = serde_json::json!({"nullstr": marker}).to_string();
            assert!(copy_writer::CopyWriter::open(
                dir.to_str().unwrap(),
                "invalid.csv",
                &options,
                &names
            )
            .is_err());
            assert!(!dir.join("invalid.csv").exists());
        }

        let mut writer =
            copy_writer::CopyWriter::open(dir.to_str().unwrap(), "out.csv", "{}", &names).unwrap();
        assert!(!dir.join("out.csv").exists());
        writer
            .row(&[
                ffi::Cell {
                    text: "1".into(),
                    is_null: false,
                },
                ffi::Cell {
                    text: "a,\"b\n".into(),
                    is_null: false,
                },
            ])
            .unwrap();
        writer
            .row(&[
                ffi::Cell {
                    text: "2".into(),
                    is_null: false,
                },
                ffi::Cell {
                    text: String::new(),
                    is_null: true,
                },
            ])
            .unwrap();
        writer.commit().unwrap();
        assert!(
            copy_writer::CopyWriter::open(dir.to_str().unwrap(), "out.csv", "{}", &names).is_err()
        );
        let binding = bind_file(dir.to_str().unwrap(), "out.csv", "csv", "{}").unwrap();
        let mut scan = open_scan(dir.to_str().unwrap(), &binding.spec, &[1]).unwrap();
        let batch = scan.next_batch(10).unwrap();
        assert_eq!(batch.cells[0].text, "a,\"b\n");
        assert!(batch.cells[1].is_null);
        {
            let _unfinished =
                copy_writer::CopyWriter::open(dir.to_str().unwrap(), "failed.csv", "{}", &names)
                    .unwrap();
        }
        assert!(!dir.join("failed.csv").exists());
        assert_eq!(std::fs::read_dir(&dir).unwrap().count(), 1);
        std::fs::remove_dir_all(dir).unwrap();
    }
}
#[cfg(test)]
mod boundary_tests {
    use super::*;
    #[test]
    fn deterministic_numeric_and_date_inference() {
        let values = [
            "9223372036854775807",
            "1.5",
            "",
            "2024-02-29",
            "2023-02-29",
            "true",
        ];
        assert_eq!(classify(values[0].into()).kind, Kind::Int);
        assert_eq!(classify("9223372036854775808".into()).kind, Kind::Text);
        assert_eq!(classify(values[3].into()).kind, Kind::Date);
        assert_eq!(classify(values[4].into()).kind, Kind::Text);
        let mut a = Column {
            name: "v".into(),
            kind: Kind::Null,
            exact_int: true,
            nullable: false,
            scale: 0,
        };
        let mut b = a.clone();
        for value in &values[..2] {
            merge(&mut a, &classify((*value).into()));
        }
        for value in values[..2].iter().rev() {
            merge(&mut b, &classify((*value).into()));
        }
        assert_eq!(a.kind, b.kind);
        assert_eq!(a.kind, Kind::Text);
    }
    #[test]
    fn csv_bom_before_quoted_fields() {
        let dir = std::env::temp_dir().join(format!("file-sql-bom-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("bom.csv"), "\u{feff}\"a,b\",c\n1,2\n").unwrap();
        let binding = bind_file(dir.to_str().unwrap(), "bom.csv", "csv", "{}").unwrap();
        assert_eq!(binding.names, vec!["a,b", "c"]);
        let binding = bind_file(
            dir.to_str().unwrap(),
            "bom.csv",
            "csv",
            r#"{"header":false}"#,
        )
        .unwrap();
        let mut scan = open_scan(dir.to_str().unwrap(), &binding.spec, &[0]).unwrap();
        assert_eq!(scan.next_batch(10).unwrap().cells[0].text, "a,b");
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn invalid_options_and_data() {
        for options in [
            "{\"sample_size\":10}",
            "{\"header\":1}",
            "{\"delimiter\":\"||\"}",
            "{\"unknown\":true}",
            "{\"delimiter\":\"|\",\"delim\":\",\"}",
        ] {
            assert!(Options::parse(options).is_err(), "{options}");
        }
        let dir = std::env::temp_dir().join(format!("file-sql-errors-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for (name, text, format) in [
            ("bad.csv", "a,b\n1,2,3\n", "csv"),
            ("quote.csv", "a\n\"not closed", "csv"),
            ("duplicate.jsonl", "{\"a\":1,\"a\":2}\n", "json"),
            ("array.jsonl", "[1,2]\n", "json"),
            ("nested.jsonl", "{\"a\":{\"b\":1}}\n", "json"),
        ] {
            std::fs::write(dir.join(name), text).unwrap();
            assert!(
                bind_file(dir.to_str().unwrap(), name, format, "{}").is_err(),
                "{name}"
            );
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
