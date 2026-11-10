#!/usr/bin/env python3
"""Recall and latency check for the approximate path.

Generates a dataset, loads it through the binary, then asks the same Top-K
questions in `exact` and `ann` mode. Ground truth is computed with NumPy over
the float32 values actually stored, including the primary-key tie-break rule.

Usage:
  calibrate.py --rows 10000 --dim 128 --queries 100 --k 10
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"


def run(binary: Path, data_dir: Path, sql: str, timeout: int = 1800) -> str:
    env = os.environ.copy()
    env["MVDB_FORMAT"] = "text"
    proc = subprocess.run(
        [str(binary), "--data-dir", str(data_dir)],
        input=sql.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        env=env,
    )
    if proc.returncode != 0:
        raise SystemExit(f"exit {proc.returncode}: {proc.stderr.decode('utf-8', 'replace')[:2000]}")
    return proc.stdout.decode("utf-8", "replace")


def vector_literal(vec: np.ndarray) -> str:
    return "[" + ",".join(f"{float(x):.9g}" for x in vec) + "]"


def build_load_sql(table_rows: list[tuple[int, str, np.ndarray]], batch: int = 100) -> str:
    parts = ["CREATE TABLE items (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(%d));"
             % len(table_rows[0][2])]
    for start in range(0, len(table_rows), batch):
        tuples = [
            f"({pk}, '{category}', '{vector_literal(vec)}')"
            for pk, category, vec in table_rows[start:start + batch]
        ]
        parts.append("INSERT INTO items VALUES " + ", ".join(tuples) + ";")
    parts.append("CREATE VECTOR INDEX items_v ON items (v);")
    return "\n".join(parts) + "\n"


def truth_topk(matrix: np.ndarray, ids: np.ndarray, query: np.ndarray, mask: np.ndarray, k: int):
    diff = matrix[mask] - query
    dist = np.sqrt((diff.astype(np.float64) ** 2).sum(axis=1))
    sub_ids = ids[mask]
    order = np.lexsort((sub_ids, dist))  # distance first, then id ascending
    return sub_ids[order[:k]]


def parse_results(text: str, queries: int, k: int) -> list[list[int]]:
    lines = [line for line in text.splitlines() if line.strip() != ""]
    out: list[list[int]] = []
    index = 0
    for _ in range(queries):
        header = lines[index]
        if not header.startswith("id"):
            raise SystemExit(f"unexpected header {header!r}")
        index += 1
        ids = []
        while index < len(lines) and len(ids) < k:
            parts = lines[index].split("\t")
            if parts[0] == "id":
                break
            ids.append(int(parts[0]))
            index += 1
        out.append(ids)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--categories", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--bin", default=str(DEFAULT_BIN))
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    rng = np.random.default_rng(args.seed)
    matrix = rng.uniform(-1.0, 1.0, size=(args.rows, args.dim)).astype(np.float32)
    ids = np.arange(1, args.rows + 1, dtype=np.int64)
    categories = np.array([f"c{i % args.categories}" for i in range(args.rows)])
    category_set = [f"c{i}" for i in range(args.categories)]

    work = Path(tempfile.mkdtemp(prefix="mvdb-calib-"))
    data_dir = work / "data"
    data_dir.mkdir()

    try:
        rows = [(int(ids[i]), str(categories[i]), matrix[i]) for i in range(args.rows)]
        load_start = time.time()
        run(binary, data_dir, build_load_sql(rows))
        load_seconds = time.time() - load_start
        print(f"loaded {args.rows} rows (dim {args.dim}) in {load_seconds:.2f}s")

        queries = rng.uniform(-1.0, 1.0, size=(args.queries, args.dim)).astype(np.float32)

        # ---- unfiltered ----
        exact_sql, ann_sql = ["SET mode = 'exact';"], ["SET mode = 'ann';"]
        for q in queries:
            literal = vector_literal(q)
            exact_sql.append(
                f"SELECT id, L2_DISTANCE(v, '{literal}') AS d FROM items ORDER BY d LIMIT {args.k};"
            )
            ann_sql.append(
                f"SELECT id, L2_DISTANCE(v, '{literal}') AS d FROM items ORDER BY d LIMIT {args.k};"
            )

        started = time.time()
        exact_text = run(binary, data_dir, "\n".join(exact_sql) + "\n")
        exact_seconds = time.time() - started
        started = time.time()
        ann_text = run(binary, data_dir, "\n".join(ann_sql) + "\n")
        ann_seconds = time.time() - started

        exact_results = parse_results(exact_text, args.queries, args.k)
        ann_results = parse_results(ann_text, args.queries, args.k)

        all_mask = np.ones(args.rows, dtype=bool)
        recall_exact, recall_ann = [], []
        for i in range(args.queries):
            ground = truth_topk(matrix, ids, queries[i], all_mask, args.k)
            recall_exact.append(len(set(ground.tolist()) & set(exact_results[i])) / args.k)
            recall_ann.append(len(set(ground.tolist()) & set(ann_results[i])) / args.k)

        # ---- filtered ----
        filt_exact_sql, filt_ann_sql = ["SET mode = 'exact';"], ["SET mode = 'ann';"]
        filt_categories = [category_set[i % len(category_set)] for i in range(args.queries)]
        for i, q in enumerate(queries):
            literal = vector_literal(q)
            category = filt_categories[i]
            filt_exact_sql.append(
                f"SELECT id, L2_DISTANCE(v, '{literal}') AS d FROM items WHERE category = '{category}' "
                f"ORDER BY d LIMIT {args.k};"
            )
            filt_ann_sql.append(
                f"SELECT id, L2_DISTANCE(v, '{literal}') AS d FROM items WHERE category = '{category}' "
                f"ORDER BY d LIMIT {args.k};"
            )

        started = time.time()
        filt_exact_text = run(binary, data_dir, "\n".join(filt_exact_sql) + "\n")
        filt_exact_seconds = time.time() - started
        started = time.time()
        filt_ann_text = run(binary, data_dir, "\n".join(filt_ann_sql) + "\n")
        filt_ann_seconds = time.time() - started

        filt_exact_results = parse_results(filt_exact_text, args.queries, args.k)
        filt_ann_results = parse_results(filt_ann_text, args.queries, args.k)

        recall_filt_exact, recall_filt_ann = [], []
        for i in range(args.queries):
            mask = categories == filt_categories[i]
            ground = truth_topk(matrix, ids, queries[i], mask, args.k)
            recall_filt_exact.append(len(set(ground.tolist()) & set(filt_exact_results[i])) / args.k)
            recall_filt_ann.append(len(set(ground.tolist()) & set(filt_ann_results[i])) / args.k)

        report = {
            "rows": args.rows,
            "dim": args.dim,
            "queries": args.queries,
            "k": args.k,
            "categories": args.categories,
            "recall": {
                "unfiltered": {
                    "exact": float(np.mean(recall_exact)),
                    "ann": float(np.mean(recall_ann)),
                    "ann_min": float(np.min(recall_ann)),
                },
                "filtered": {
                    "exact": float(np.mean(recall_filt_exact)),
                    "ann": float(np.mean(recall_filt_ann)),
                    "ann_min": float(np.min(recall_filt_ann)),
                },
            },
            "seconds": {
                "load": load_seconds,
                "unfiltered_exact": exact_seconds,
                "unfiltered_ann": ann_seconds,
                "filtered_exact": filt_exact_seconds,
                "filtered_ann": filt_ann_seconds,
            },
        }

        print(f"unfiltered recall@10  exact={report['recall']['unfiltered']['exact']:.4f} "
              f"ann={report['recall']['unfiltered']['ann']:.4f} "
              f"(min {report['recall']['unfiltered']['ann_min']:.2f})")
        print(f"filtered   recall@10  exact={report['recall']['filtered']['exact']:.4f} "
              f"ann={report['recall']['filtered']['ann']:.4f} "
              f"(min {report['recall']['filtered']['ann_min']:.2f})")
        print(f"time: unfiltered exact {exact_seconds:.2f}s vs ann {ann_seconds:.2f}s | "
              f"filtered exact {filt_exact_seconds:.2f}s vs ann {filt_ann_seconds:.2f}s")

        if args.out:
            out_path = Path(args.out)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

        ok = (report["recall"]["unfiltered"]["ann"] >= 0.95 and
              report["recall"]["filtered"]["ann"] >= 0.95)
        print("calibrate: " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
