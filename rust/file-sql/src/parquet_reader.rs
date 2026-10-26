// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! Parquet codecs are delegated to Apache parquet. Compact headers are preflighted
//! before its allocations, so forged lengths cannot bypass the scan memory budget.
use super::*;
use bytes::Bytes;
use parquet::{
    basic::{Compression, ConvertedType, LogicalType, TimeUnit, Type},
    column::reader::ColumnReader,
    errors::ParquetError,
    file::{
        reader::{ChunkReader, FileReader, Length},
        serialized_reader::SerializedFileReader,
    },
};
use std::{
    io,
    sync::{Arc, Mutex},
};
struct AtReader {
    file: Arc<File>,
    position: u64,
    end: u64,
}
fn read_at(file: &File, buf: &mut [u8], offset: u64) -> io::Result<usize> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileExt;
        file.read_at(buf, offset)
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::FileExt;
        file.seek_read(buf, offset)
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (file, buf, offset);
        Err(io::ErrorKind::Unsupported.into())
    }
}
impl Read for AtReader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = buf
            .len()
            .min(self.end.saturating_sub(self.position) as usize);
        let read = read_at(&self.file, &mut buf[..n], self.position)?;
        self.position += read as u64;
        Ok(read)
    }
}
struct Compact<R> {
    input: R,
    bytes: usize,
    items: usize,
    max_values: usize,
    metadata: bool,
    checking_schema: bool,
    schema_elements: usize,
}
impl<R: Read> Compact<R> {
    fn byte(&mut self) -> Result<u8> {
        self.bytes += 1;
        ensure!(self.bytes <= RECORD_LIMIT, "Parquet header size limit");
        let mut b = [0];
        self.input.read_exact(&mut b)?;
        Ok(b[0])
    }
    fn varint(&mut self) -> Result<u64> {
        let mut value = 0;
        for shift in (0..70).step_by(7) {
            let b = self.byte()?;
            ensure!(shift < 63 || b <= 1, "Parquet varint overflow");
            value |= u64::from(b & 127) << shift;
            if b < 128 {
                return Ok(value);
            }
        }
        bail!("Parquet varint overflow")
    }
    fn integer(&mut self) -> Result<i64> {
        let n = self.varint()?;
        Ok(((n >> 1) as i64) ^ -((n & 1) as i64))
    }
    fn structure(&mut self, depth: usize) -> Result<Vec<(i16, i64)>> {
        ensure!(depth < 32, "Parquet header nesting limit");
        if self.checking_schema && depth == 2 {
            self.schema_elements += 1;
            ensure!(
                self.schema_elements <= COLUMN_LIMIT + 1,
                "Parquet schema size limit"
            );
        }
        let mut previous = 0i16;
        let mut ints = Vec::new();
        let mut seen_fields = HashSet::new();
        loop {
            let header = self.byte()?;
            if header == 0 {
                break;
            }
            self.items += 1;
            ensure!(self.items <= 100_000, "Parquet header item limit");
            let delta = header >> 4;
            let field = if delta == 0 {
                let n = self.integer()?;
                i16::try_from(n)?
            } else {
                previous
                    .checked_add(delta as i16)
                    .context("field overflow")?
            };
            ensure!(seen_fields.insert(field), "duplicate Parquet compact field");
            previous = field;
            let ty = header & 15;
            if self.metadata && depth == 0 && field == 2 {
                ensure!(ty == 9, "Parquet schema must be a LIST");
            }
            if self.checking_schema && depth == 2 {
                let expected = match field {
                    1..=3 | 5..=9 => Some(5),
                    4 => Some(8),
                    10 => Some(12),
                    _ => None,
                };
                ensure!(
                    expected.is_none_or(|t| t == ty),
                    "invalid Parquet schema field type"
                );
            }
            if !self.metadata
                && ((depth == 0 && (field == 2 || field == 3)) || (depth == 1 && field == 1))
            {
                ensure!(ty == 5, "invalid Parquet page size/count type");
            }
            if (4..=6).contains(&ty) {
                let n = self.integer()?;
                if self.checking_schema && field == 5 {
                    ensure!(
                        (0..=COLUMN_LIMIT as i64).contains(&n),
                        "Parquet schema child count limit"
                    );
                    ensure!(
                        self.schema_elements == 1 || n == 0,
                        "nested Parquet schema unsupported"
                    );
                }
                if depth == 0 {
                    ints.push((field, n));
                }
                if depth == 1 && field == 1 {
                    ensure!(
                        (0..=1_000_000).contains(&n),
                        "Parquet page value count limit"
                    );
                    self.max_values = self.max_values.max(n as usize);
                }
            } else {
                let old_schema = self.checking_schema;
                self.checking_schema |= depth == 0 && field == 2 && ty == 9;
                self.skip(ty, depth + 1, false)?;
                self.checking_schema = old_schema;
            }
        }
        Ok(ints)
    }
    fn skip(&mut self, ty: u8, depth: usize, in_collection: bool) -> Result<()> {
        self.items += 1;
        ensure!(
            self.items <= 100_000 && depth < 32,
            "Parquet metadata item limit"
        );
        match ty {
            1 | 2 => {
                if in_collection {
                    self.byte()?;
                }
            }
            3 => {
                self.byte()?;
            }
            4..=6 => {
                self.integer()?;
            }
            7 => {
                for _ in 0..8 {
                    self.byte()?;
                }
            }
            8 => {
                let size = self.varint()?;
                ensure!(size <= RECORD_LIMIT as u64, "Parquet binary size limit");
                for _ in 0..size {
                    self.byte()?;
                }
            }
            9 | 10 => {
                let header = self.byte()?;
                if self.checking_schema && depth == 1 {
                    ensure!(header & 15 == 12, "Parquet schema entries must be STRUCT");
                }
                let mut count = u64::from(header >> 4);
                if count == 15 {
                    count = self.varint()?;
                }
                ensure!(count <= 10000, "Parquet metadata collection limit");
                for _ in 0..count {
                    self.skip(header & 15, depth + 1, true)?;
                }
            }
            11 => {
                let count = self.varint()?;
                ensure!(count <= 10000, "Parquet metadata map limit");
                if count > 0 {
                    let types = self.byte()?;
                    for _ in 0..count {
                        self.skip(types >> 4, depth + 1, true)?;
                        self.skip(types & 15, depth + 1, true)?;
                    }
                }
            }
            12 => {
                self.structure(depth)?;
            }
            _ => bail!("invalid Parquet compact type"),
        }
        Ok(())
    }
}
struct BoundedFile {
    file: Arc<File>,
    len: u64,
    footer: Bytes,
    // Only the projected columns of the current row group are retained.
    page_limits: Arc<Mutex<Vec<(u64, u64, usize)>>>,
}
struct Recording<R> {
    input: R,
    bytes: Vec<u8>,
}
impl<R: Read> Read for Recording<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.input.read(buf)?;
        self.bytes.extend_from_slice(&buf[..n]);
        Ok(n)
    }
}
fn page_header(file: Arc<File>, start: u64, end: u64) -> Result<(Vec<u8>, usize, usize)> {
    let mut compact = Compact {
        input: Recording {
            input: BufReader::new(AtReader {
                file,
                position: start,
                end,
            }),
            bytes: Vec::new(),
        },
        bytes: 0,
        items: 0,
        max_values: 0,
        metadata: false,
        checking_schema: false,
        schema_elements: 0,
    };
    let fields = compact.structure(0)?;
    let compressed = fields
        .iter()
        .find(|(id, _)| *id == 3)
        .context("missing compressed page size")?
        .1;
    let uncompressed = fields
        .iter()
        .find(|(id, _)| *id == 2)
        .context("missing page size")?
        .1;
    ensure!(
        (0..=RECORD_LIMIT as i64).contains(&compressed)
            && (0..=RECORD_LIMIT as i64).contains(&uncompressed),
        "Parquet page size limit"
    );
    ensure!(
        start
            .checked_add(compact.bytes as u64)
            .and_then(|p| p.checked_add(compressed as u64))
            .is_some_and(|p| p <= end),
        "Parquet page beyond chunk"
    );
    let budget = ((uncompressed as usize).saturating_mul(4) + compact.max_values * 32 + 8192) * 3;
    Ok((compact.input.bytes, compressed as usize, budget))
}
impl BoundedFile {
    fn new(file: File) -> Result<Self> {
        let len = file.metadata()?.len();
        ensure!(len >= 12, "truncated Parquet file");
        let file = Arc::new(file);
        let mut tail = [0; 8];
        AtReader {
            file: file.clone(),
            position: len - 8,
            end: len,
        }
        .read_exact(&mut tail)?;
        ensure!(&tail[4..] == b"PAR1", "invalid Parquet footer magic");
        let size = u32::from_le_bytes(tail[..4].try_into()?) as u64;
        ensure!(
            size <= RECORD_LIMIT as u64 && size <= len - 12,
            "Parquet footer size limit"
        );
        // Parse and serve the same bounded footer snapshot, even if the file changes.
        let mut footer = vec![0; size as usize + 8];
        AtReader {
            file: file.clone(),
            position: len - size - 8,
            end: len,
        }
        .read_exact(&mut footer)?;
        ensure!(footer[size as usize..] == tail, "Parquet footer changed");
        let mut compact = Compact {
            input: io::Cursor::new(&footer[..size as usize]),
            bytes: 0,
            items: 0,
            max_values: 0,
            metadata: true,
            checking_schema: false,
            schema_elements: 0,
        };
        compact.structure(0)?;
        Ok(Self {
            file,
            len,
            footer: Bytes::from(footer),
            page_limits: Arc::new(Mutex::new(Vec::new())),
        })
    }
}
impl Length for BoundedFile {
    fn len(&self) -> u64 {
        self.len
    }
}
impl ChunkReader for BoundedFile {
    type T = io::Cursor<Vec<u8>>;
    fn get_read(&self, start: u64) -> parquet::errors::Result<Self::T> {
        if start == self.len - 8 {
            return Ok(io::Cursor::new(
                self.footer[self.footer.len() - 8..].to_vec(),
            ));
        }
        let limits = self
            .page_limits
            .lock()
            .map_err(|_| ParquetError::General("page limits poisoned".into()))?;
        let (_, end, limit) = limits
            .iter()
            .find(|(begin, end, _)| start >= *begin && start < *end)
            .ok_or_else(|| {
                ParquetError::General("read outside validated Parquet columns".into())
            })?;
        let (header, _, budget) = page_header(self.file.clone(), start, *end)
            .map_err(|e| ParquetError::General(e.to_string()))?;
        if budget > *limit {
            return Err(ParquetError::General(
                "Parquet page changed beyond validated budget".into(),
            ));
        }
        // Never let the codec re-read a header after its allocation limits were checked.
        Ok(io::Cursor::new(header))
    }
    fn get_bytes(&self, start: u64, length: usize) -> parquet::errors::Result<Bytes> {
        if length > RECORD_LIMIT
            || start
                .checked_add(length as u64)
                .is_none_or(|end| end > self.len)
        {
            return Err(ParquetError::General("Parquet read budget/bounds".into()));
        }
        let footer_start = self.len - self.footer.len() as u64;
        if start >= footer_start {
            let offset = (start - footer_start) as usize;
            return Ok(self.footer.slice(offset..offset + length));
        }
        let mut bytes = vec![0; length];
        AtReader {
            file: self.file.clone(),
            position: start,
            end: self.len,
        }
        .read_exact(&mut bytes)?;
        Ok(Bytes::from(bytes))
    }
}
fn kind(column: &parquet::schema::types::ColumnDescriptor) -> Result<Kind> {
    ensure!(
        column.max_rep_level() == 0 && column.path().parts().len() == 1,
        "nested Parquet columns not supported"
    );
    let logical = column.logical_type_ref();
    Ok(match column.physical_type() {
        Type::BOOLEAN => Kind::Bool,
        Type::INT32 if column.converted_type() == ConvertedType::DATE => Kind::Date,
        Type::INT64 if matches!(logical, Some(LogicalType::Timestamp(_))) => {
            if let Some(LogicalType::Timestamp(t)) = logical {
                ensure!(
                    !t.is_adjusted_to_u_t_c,
                    "timezone Parquet timestamp unsupported"
                );
                ensure!(
                    !matches!(t.unit, TimeUnit::NANOS),
                    "nanosecond timestamps unsupported"
                );
            }
            Kind::DateTime
        }
        Type::INT64 if matches!(logical,Some(LogicalType::Integer(i)) if !i.is_signed) => {
            Kind::Text
        }
        Type::INT32 | Type::INT64 => {
            ensure!(
                logical.is_none() || matches!(logical, Some(LogicalType::Integer(_))),
                "unsupported Parquet logical integer type"
            );
            Kind::Int
        }
        Type::FLOAT | Type::DOUBLE => Kind::Double,
        Type::BYTE_ARRAY if column.converted_type() == ConvertedType::UTF8 => Kind::Text,
        _ => bail!("unsupported Parquet type {}", column.physical_type()),
    })
}
fn validate_column_range(column: &parquet::file::metadata::ColumnChunkMetaData) -> Result<()> {
    ensure!(
        column.compressed_size() >= 0
            && column.uncompressed_size() >= 0
            && column.data_page_offset() >= 0
            && column.dictionary_page_offset().is_none_or(|v| v >= 0),
        "negative Parquet column range"
    );
    Ok(())
}
fn columns(reader: &SerializedFileReader<BoundedFile>) -> Result<Vec<Column>> {
    let metadata = reader.metadata();
    let mut rows = 0i64;
    for group in metadata.row_groups() {
        ensure!(group.num_rows() >= 0, "negative Parquet row count");
        rows = rows
            .checked_add(group.num_rows())
            .context("Parquet row count overflow")?;
        for column in group.columns() {
            validate_column_range(column)?;
            ensure!(
                column.num_values() == group.num_rows(),
                "Parquet flat column row count mismatch"
            );
        }
    }
    ensure!(
        rows == metadata.file_metadata().num_rows(),
        "Parquet total row count mismatch"
    );
    let desc = reader.metadata().file_metadata().schema_descr();
    ensure!(desc.num_columns() <= COLUMN_LIMIT, "Parquet column limit");
    desc.columns()
        .iter()
        .map(|col| {
            Ok(Column {
                name: col.name().into(),
                kind: kind(col)?,
                exact_int: true,
            })
        })
        .collect()
}
pub fn schema(file: File) -> Result<(Vec<Column>, u64)> {
    let reader = SerializedFileReader::new(BoundedFile::new(file)?)?;
    Ok((
        columns(&reader)?,
        reader.metadata().file_metadata().num_rows().try_into()?,
    ))
}
fn validate_pages(file: Arc<File>, start: u64, length: u64) -> Result<usize> {
    let end = start
        .checked_add(length)
        .context("Parquet chunk overflow")?;
    ensure!(end <= file.metadata()?.len(), "Parquet chunk beyond file");
    let mut pos = start;
    let mut budget = 0usize;
    while pos < end {
        let (header, compressed, current_budget) = page_header(file.clone(), pos, end)?;
        budget = budget.max(current_budget);
        pos += header.len() as u64 + compressed as u64;
    }
    Ok(budget)
}
pub struct Reader {
    reader: SerializedFileReader<BoundedFile>,
    file: Arc<File>,
    page_limits: Arc<Mutex<Vec<(u64, u64, usize)>>>,
    projection: Vec<usize>,
    group: usize,
    remaining: usize,
    columns: Vec<ColumnReader>,
    kinds: Vec<Kind>,
}
impl Reader {
    pub fn new(file: File, projection: &[usize]) -> Result<Self> {
        let bounded = BoundedFile::new(file)?;
        let file = bounded.file.clone();
        let page_limits = bounded.page_limits.clone();
        let reader = SerializedFileReader::new(bounded)?;
        let schema = columns(&reader)?;
        ensure!(
            projection.iter().all(|&i| i < schema.len()),
            "Parquet projection outside current schema"
        );
        let kinds = projection.iter().map(|&i| schema[i].kind).collect();
        Ok(Self {
            reader,
            file,
            page_limits,
            projection: projection.to_vec(),
            group: 0,
            remaining: 0,
            columns: Vec::new(),
            kinds,
        })
    }
    pub fn row(&mut self) -> Result<Option<Vec<Scalar>>> {
        while self.remaining == 0 {
            if self.group == self.reader.num_row_groups() {
                return Ok(None);
            }
            self.columns.clear();
            self.page_limits
                .lock()
                .map_err(|_| anyhow::anyhow!("page limits poisoned"))?
                .clear();
            let group = self.reader.get_row_group(self.group)?;
            self.group += 1;
            self.remaining = group.metadata().num_rows().try_into()?;
            let mut budget = 0usize;
            for &index in &self.projection {
                let meta = group.metadata().column(index);
                ensure!(
                    matches!(
                        meta.compression(),
                        Compression::UNCOMPRESSED | Compression::SNAPPY
                    ),
                    "unsupported Parquet codec"
                );
                let (start, size) = meta.byte_range();
                let column_budget = validate_pages(self.file.clone(), start, size)?;
                self.page_limits
                    .lock()
                    .map_err(|_| anyhow::anyhow!("page limits poisoned"))?
                    .push((start, start + size, column_budget));
                budget = budget
                    .checked_add(column_budget)
                    .context("page budget overflow")?;
                ensure!(
                    budget <= MEMORY_LIMIT,
                    "projected Parquet pages exceed memory budget"
                );
                self.columns.push(group.get_column_reader(index)?);
            }
        }
        self.remaining -= 1;
        let mut row = Vec::new();
        for (index, column) in self.columns.iter_mut().enumerate() {
            let mut defs = Vec::with_capacity(1);
            macro_rules! read {
                ($r:expr,$convert:expr) => {{
                    let mut values = Vec::with_capacity(1);
                    let (records, count, _) =
                        $r.read_records(1, Some(&mut defs), None, &mut values)?;
                    ensure!(records == 1, "Parquet column ended early");
                    if count == 0 {
                        None
                    } else {
                        Some($convert(&values[0])?)
                    }
                }};
            }
            let text: Option<String> = match column {
                ColumnReader::BoolColumnReader(r) => {
                    read!(r, |v: &bool| -> Result<String> { Ok(v.to_string()) })
                }
                ColumnReader::Int32ColumnReader(r) => read!(r, |v: &i32| -> Result<String> {
                    if self.kinds[index] == Kind::Date {
                        let date = chrono::NaiveDate::from_ymd_opt(1970, 1, 1)
                            .unwrap()
                            .checked_add_signed(chrono::Duration::days(i64::from(*v)))
                            .context("Parquet DATE overflow")?;
                        Ok(date.to_string())
                    } else {
                        let descriptor = self
                            .reader
                            .metadata()
                            .file_metadata()
                            .schema_descr()
                            .column(self.projection[index]);
                        if matches!(descriptor.logical_type_ref(),Some(LogicalType::Integer(i)) if !i.is_signed)
                        {
                            Ok((*v as u32).to_string())
                        } else {
                            Ok(v.to_string())
                        }
                    }
                }),
                ColumnReader::Int64ColumnReader(r) => read!(r, |v: &i64| -> Result<String> {
                    if self.kinds[index] == Kind::DateTime {
                        let descriptor = self
                            .reader
                            .metadata()
                            .file_metadata()
                            .schema_descr()
                            .column(self.projection[index]);
                        let millis = matches!(descriptor.logical_type_ref(),Some(LogicalType::Timestamp(t)) if matches!(t.unit,TimeUnit::MILLIS));
                        let micros = if millis {
                            v.checked_mul(1000).context("timestamp overflow")?
                        } else {
                            *v
                        };
                        let dt = chrono::DateTime::from_timestamp_micros(micros)
                            .context("timestamp out of range")?;
                        Ok(dt.naive_utc().format("%Y-%m-%d %H:%M:%S%.6f").to_string())
                    } else if self.kinds[index] == Kind::Text {
                        Ok((*v as u64).to_string())
                    } else {
                        Ok(v.to_string())
                    }
                }),
                ColumnReader::FloatColumnReader(r) => read!(r, |v: &f32| -> Result<String> {
                    ensure!(v.is_finite(), "non-finite Parquet float");
                    Ok(f64::from(*v).to_string())
                }),
                ColumnReader::DoubleColumnReader(r) => read!(r, |v: &f64| -> Result<String> {
                    ensure!(v.is_finite(), "non-finite Parquet double");
                    Ok(v.to_string())
                }),
                ColumnReader::ByteArrayColumnReader(r) => {
                    read!(r, |v: &parquet::data_type::ByteArray| -> Result<String> {
                        ensure!(v.len() <= RECORD_LIMIT, "Parquet string limit");
                        Ok(v.as_utf8()?.to_owned())
                    })
                }
                _ => bail!("unsupported Parquet column"),
            };
            row.push(match text {
                Some(text) => Scalar {
                    text,
                    kind: self.kinds[index],
                    exact: true,
                },
                None => Scalar {
                    text: String::new(),
                    kind: Kind::Null,
                    exact: true,
                },
            });
        }
        Ok(Some(row))
    }
}

#[cfg(test)]
mod guard_tests {
    use super::*;
    #[test]
    fn negative_column_ranges_rejected_before_byte_range() {
        use parquet::{
            file::metadata::ColumnChunkMetaData,
            schema::{parser::parse_message_type, types::SchemaDescriptor},
        };
        let schema = SchemaDescriptor::new(Arc::new(
            parse_message_type("message t { OPTIONAL INT64 id; }").unwrap(),
        ));
        for (size, data, dictionary) in [(-1, 4, None), (1, -1, None), (1, 4, Some(-1))] {
            let column = ColumnChunkMetaData::builder(schema.column(0))
                .set_total_compressed_size(size)
                .set_data_page_offset(data)
                .set_dictionary_page_offset(dictionary)
                .build()
                .unwrap();
            assert!(validate_column_range(&column).is_err());
        }
    }
    #[test]
    fn malformed_schema_counts_rejected_before_codec() {
        let path = std::env::temp_dir().join(format!("file-sql-footer-{}", std::process::id()));
        for footer in [
            // schema=[{num_children: i32::MAX}]
            vec![0x29, 0x1c, 0x55, 0xfe, 0xff, 0xff, 0xff, 0x0f, 0, 0],
            // schema=[{num_children:1},{num_children:1}] (nested)
            vec![0x29, 0x2c, 0x55, 2, 0, 0x55, 2, 0, 0],
            vec![0x2a, 0x1c, 0x55, 0xfe, 0xff, 0xff, 0xff, 0x0f, 0, 0],
            vec![0x29, 0x2c, 0x55, 2, 0, 0x53, 2, 0, 0],
            vec![0x29, 0x1b, 0, 0],
        ] {
            let mut data = b"PAR1".to_vec();
            data.extend_from_slice(&footer);
            data.extend_from_slice(&(footer.len() as u32).to_le_bytes());
            data.extend_from_slice(b"PAR1");
            std::fs::write(&path, data).unwrap();
            assert!(BoundedFile::new(File::open(&path).unwrap()).is_err());
        }
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn validated_headers_and_footer_are_not_reread() {
        use std::io::{Seek, SeekFrom, Write};
        let path = std::env::temp_dir().join(format!("file-sql-page-{}", std::process::id()));
        // Minimal compact page: type=0, uncompressed=1, compressed=1, STOP.
        let header = [0x15, 0, 0x15, 2, 0x15, 2, 0];
        let mut data = b"PAR1".to_vec();
        data.extend_from_slice(&header);
        data.push(0);
        data.push(0); // empty metadata struct (only the guard is tested)
        data.extend_from_slice(&1u32.to_le_bytes());
        data.extend_from_slice(b"PAR1");
        std::fs::write(&path, &data).unwrap();
        let bounded = BoundedFile::new(File::open(&path).unwrap()).unwrap();
        let budget = validate_pages(bounded.file.clone(), 4, 8).unwrap();
        bounded.page_limits.lock().unwrap().push((4, 12, budget));
        let mut checked = bounded.get_read(4).unwrap();
        let mut writer = std::fs::OpenOptions::new().write(true).open(&path).unwrap();
        writer.seek(SeekFrom::Start(7)).unwrap();
        writer.write_all(&[100]).unwrap(); // grow uncompressed allocation
        assert!(bounded.get_read(4).is_err());
        let mut bytes = Vec::new();
        checked.read_to_end(&mut bytes).unwrap();
        assert_eq!(bytes, header); // codec consumes the checked snapshot
        writer.seek(SeekFrom::End(-9)).unwrap();
        writer.write_all(&[255]).unwrap();
        assert_eq!(bounded.get_bytes(data.len() as u64 - 9, 1).unwrap()[0], 0);
        std::fs::remove_file(path).unwrap();
    }
}
