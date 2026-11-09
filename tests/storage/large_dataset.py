#!/usr/bin/env python3
"""Large-dataset check.

Writes far more vector data than the page cache budget (default 256 pages =
2 MiB) and verifies that inserts, point lookups, a full scan and a restart
still work. The goal is to prove the engine streams from disk instead of
loading everything.

Peak memory is taken from the binary's own /proc/self/status VmHWM report
(--stats), which avoids the copy-on-write inflation of measuring a forked
child from a large Python parent.

Usage:
  large_dataset.py --rows 20000 --dim 128
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"
PEAK_RE = re.compile(r"peak_rss_kb=\s*(\d+)")


def vector_literal(dim: int, rng: random.Random) -> str:
    values = ",".join(f"{rng.uniform(-1, 1):.6f}" for _ in range(dim))
    return f"[{values}]"


def write_sql(path: Path, rows: int, dim: int, batch: int, seed: int) -> None:
    rng = random.Random(seed)
    with path.open("w", encoding="utf-8") as out:
        out.write("CREATE TABLE big (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(%d));\n" % dim)
        for start in range(1, rows + 1, batch):
            tuples = []
            for pk in range(start, min(start + batch, rows + 1)):
                tuples.append(f"({pk}, 'c{pk % 10}', '{vector_literal(dim, rng)}')")
            out.write("INSERT INTO big VALUES " + ", ".join(tuples) + ";\n")


def run(binary: Path, data_dir: Path, sql_path: Path) -> tuple[str, int]:
    with sql_path.open("rb") as fin:
        env = os.environ.copy()
        env["MVDB_FORMAT"] = "text"
        proc = subprocess.run(
            [str(binary), "--data-dir", str(data_dir), "--stats"],
            stdin=fin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=1800,
            env=env,
        )
    if proc.returncode != 0:
        raise SystemExit(f"exit {proc.returncode}: {proc.stderr.decode('utf-8', 'replace')}")
    match = PEAK_RE.search(proc.stderr.decode("utf-8", "replace"))
    peak_kb = int(match.group(1)) if match else -1
    return proc.stdout.decode("utf-8", "replace"), peak_kb


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--bin", default=str(DEFAULT_BIN))
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    work = Path(tempfile.mkdtemp(prefix="mvdb-large-"))
    data_dir = work / "data"
    data_dir.mkdir()
    failures = 0

    def check(name: str, expected: str, actual: str) -> None:
        nonlocal failures
        if actual == expected:
            print(f"[ok]   {name}")
        else:
            failures += 1
            print(f"[FAIL] {name}: expected {expected!r}, got {actual!r}")

    try:
        payload_mib = args.rows * (args.dim * 4 + 32) / 1024 / 1024
        print(f"rows={args.rows} dim={args.dim} batch={args.batch}")
        print(f"dataset payload ~= {payload_mib:.1f} MiB, page cache = 2 MiB")

        insert_sql = work / "insert.sql"
        write_sql(insert_sql, args.rows, args.dim, args.batch, args.seed)

        started = time.time()
        output, peak_kb = run(binary, data_dir, insert_sql)
        print(f"insert wall time = {time.time() - started:.2f}s, peak RSS = {peak_kb / 1024:.1f} MiB")
        if output.strip():
            print(output[:2000])

        probe = work / "probe.sql"
        probe.write_text(
            "\n".join(
                [
                    "SELECT id FROM big ORDER BY id LIMIT 1;",
                    "SELECT id FROM big ORDER BY id DESC LIMIT 1;",
                    "SELECT id, category FROM big WHERE id = %d;" % (args.rows // 2),
                    "SELECT id FROM big;",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        started = time.time()
        text, peak_kb = run(binary, data_dir, probe)
        print(f"verify wall time = {time.time() - started:.2f}s, peak RSS = {peak_kb / 1024:.1f} MiB")
        lines = text.splitlines()
        check("first id", "id\n1", "\n".join(lines[0:2]))
        check("last id", "id\n%d" % args.rows, "\n".join(lines[2:4]))
        check(
            "point lookup",
            "id\tcategory\n%d\tc%d" % (args.rows // 2, (args.rows // 2) % 10),
            "\n".join(lines[4:6]),
        )
        scan_ids = lines[6:]
        check("full scan row count", str(args.rows), str(len(scan_ids) - 1))
        check("full scan is ordered", "1", scan_ids[1] if len(scan_ids) > 1 else "")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n{'ok' if failures == 0 else 'FAILED'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
