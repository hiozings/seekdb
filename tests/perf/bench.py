#!/usr/bin/env python3
"""Load and query benchmark mirroring the official performance profile.

Defaults follow the task statement: 10,000 rows, 128 dimensions, 1,000 Top-K
queries with K=10, half of them carrying an equality filter. Writes go in
batches of 100 and commit per batch.

Reports wall time, the engine's own phase breakdown, peak RSS and block I/O
(from the process itself; the platform uses cgroup counters).

Usage:
  bench.py --rows 10000 --dim 128 --queries 1000 --mode ann
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"

STAT_PATTERNS = {
    "peak_rss_kb": re.compile(r"peak_rss_kb=\s*(\d+)"),
    "read_bytes": re.compile(r"read_bytes=(\d+)"),
    "write_bytes": re.compile(r"write_bytes=(\d+)"),
    "select_scan_ms": re.compile(r"select_scan_ms=([\d.]+)"),
    "select_ann_ms": re.compile(r"select_ann_ms=([\d.]+)"),
    "commit_ms": re.compile(r"commit_ms=([\d.]+)"),
    "index_build_ms": re.compile(r"index_build_ms=([\d.]+)"),
    "rows_scanned": re.compile(r"rows_scanned=(\d+)"),
    "avg_ef": re.compile(r"avg_ef=([\d.]+)"),
    "ann_queries": re.compile(r"ann_queries=(\d+)"),
    "insert_encode_ms": re.compile(r"insert_encode_ms=([\d.]+)"),
    "insert_heap_ms": re.compile(r"insert_heap_ms=([\d.]+)"),
    "insert_index_ms": re.compile(r"insert_index_ms=([\d.]+)"),
}


def run(binary: Path, data_dir: Path, sql_path: Path, output_format: str, timeout: int = 3600):
    env = os.environ.copy()
    env["MVDB_FORMAT"] = output_format
    with sql_path.open("rb") as fin:
        proc = subprocess.run(
            [str(binary), "--data-dir", str(data_dir), "--stats"],
            stdin=fin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            env=env,
        )
    if proc.returncode != 0:
        raise SystemExit(f"exit {proc.returncode}: {proc.stderr.decode('utf-8', 'replace')[:2000]}")
    stats = {}
    text = proc.stderr.decode("utf-8", "replace")
    for name, pattern in STAT_PATTERNS.items():
        match = pattern.search(text)
        if match:
            value = match.group(1)
            stats[name] = float(value) if "." in value else int(value)
    return proc.stdout.decode("utf-8", "replace"), stats


def vector_literal(vec: np.ndarray) -> str:
    return "[" + ",".join(f"{float(x):.9g}" for x in vec) + "]"


def write_load_sql(path: Path, matrix: np.ndarray, categories: np.ndarray, batch: int,
                   create_index: bool) -> None:
    dim = matrix.shape[1]
    with path.open("w", encoding="utf-8") as out:
        out.write(f"CREATE TABLE items (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR({dim}));\n")
        rows = len(matrix)
        for start in range(0, rows, batch):
            tuples = [
                f"({i + 1}, '{categories[i]}', '{vector_literal(matrix[i])}')"
                for i in range(start, min(start + batch, rows))
            ]
            out.write("INSERT INTO items VALUES " + ", ".join(tuples) + ";\n")
        if create_index:
            out.write("CREATE VECTOR INDEX items_v ON items (v);\n")


def write_query_sql(path: Path, queries: np.ndarray, categories: list[str], filtered: list[bool],
                    k: int, mode: str) -> None:
    with path.open("w", encoding="utf-8") as out:
        out.write(f"SET mode = '{mode}';\n")
        for i, query in enumerate(queries):
            literal = vector_literal(query)
            where = f" WHERE category = '{categories[i]}'" if filtered[i] else ""
            out.write(
                f"SELECT id, L2_DISTANCE(v, '{literal}') AS d FROM items{where} ORDER BY d LIMIT {k};\n"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--categories", type=int, default=10)
    parser.add_argument("--mode", default="ann", choices=["exact", "ann"])
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--bin", default=str(DEFAULT_BIN))
    parser.add_argument("--out", default="")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--no-index", action="store_true", help="skip CREATE VECTOR INDEX")
    parser.add_argument("--format", default="json", choices=["json", "text"])
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    rng = np.random.default_rng(args.seed)
    matrix = rng.uniform(-1.0, 1.0, size=(args.rows, args.dim)).astype(np.float32)
    categories = np.array([f"c{i % args.categories}" for i in range(args.rows)])
    queries = rng.uniform(-1.0, 1.0, size=(args.queries, args.dim)).astype(np.float32)
    filtered = [(i % 2) == 1 for i in range(args.queries)]
    query_categories = [f"c{i % args.categories}" for i in range(args.queries)]

    work = Path(tempfile.mkdtemp(prefix="mvdb-bench-"))
    data_dir = work / "data"
    data_dir.mkdir()

    try:
        load_sql = work / "load.sql"
        write_load_sql(load_sql, matrix, categories, args.batch, not args.no_index)
        started = time.time()
        _, load_stats = run(binary, data_dir, load_sql, args.format)
        load_seconds = time.time() - started

        query_sql = work / "query.sql"
        write_query_sql(query_sql, queries, query_categories, filtered, args.k, args.mode)
        started = time.time()
        stdout, query_stats = run(binary, data_dir, query_sql, args.format)
        query_seconds = time.time() - started

        # Process startup (open + index rebuild) measured separately.
        empty = work / "empty.sql"
        empty.write_text("", encoding="utf-8")
        started = time.time()
        _, startup_stats = run(binary, data_dir, empty, args.format)
        startup_seconds = time.time() - started

        result_rows = len([line for line in stdout.splitlines() if line.strip()])
        report = {
            "rows": args.rows,
            "dim": args.dim,
            "queries": args.queries,
            "k": args.k,
            "mode": args.mode,
            "seconds": {
                "load": round(load_seconds, 3),
                "queries_total": round(query_seconds, 3),
                "startup": round(startup_seconds, 3),
                "queries_only": round(query_seconds - startup_seconds, 3),
            },
            "stats": {
                "load": load_stats,
                "queries": query_stats,
                "startup": startup_stats,
            },
            "rows_returned": result_rows,
        }

        print(f"rows={args.rows} dim={args.dim} queries={args.queries} mode={args.mode}")
        print(f"load:    {load_seconds:.3f}s  peak_rss={load_stats.get('peak_rss_kb', 0) / 1024:.1f} MiB  "
              f"commit={load_stats.get('commit_ms', 0) / 1000:.2f}s  "
              f"index_build={load_stats.get('index_build_ms', 0) / 1000:.2f}s")
        print(f"queries: {query_seconds:.3f}s total, startup {startup_seconds:.3f}s, "
              f"queries-only {query_seconds - startup_seconds:.3f}s  "
              f"({(query_seconds - startup_seconds) * 1000 / args.queries:.2f} ms/query)")
        print(f"         scan={query_stats.get('select_scan_ms', 0) / 1000:.2f}s "
              f"ann={query_stats.get('select_ann_ms', 0) / 1000:.2f}s "
              f"rows_scanned={query_stats.get('rows_scanned', 0)} "
              f"avg_ef={query_stats.get('avg_ef', 0):.0f}")
        print(f"io:      read={query_stats.get('read_bytes', 0) / 1048576:.1f} MiB "
              f"write={load_stats.get('write_bytes', 0) / 1048576:.1f} MiB")

        if args.out:
            out_path = Path(args.out)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
