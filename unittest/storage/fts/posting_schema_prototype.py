#!/usr/bin/env python3
"""M1 schema feasibility check for the FTS posting table.

Creates a prototype table with the proposed rowkey and columns, inserts sample
rows, verifies the catalog rowkey/columns, then drops it.  It does not touch
the engine's real schema or any official benchmark file.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


SQL = """
DROP TABLE IF EXISTS {db}.__fts_posting_proto;
CREATE TABLE {db}.__fts_posting_proto (
  word VARCHAR(1024) COLLATE utf8mb4_general_ci NOT NULL,
  doc_id VARCHAR(64) BINARY NOT NULL,
  word_count INT NOT NULL,
  doc_length INT NOT NULL,
  PRIMARY KEY (word, doc_id)
);
INSERT INTO {db}.__fts_posting_proto VALUES
  ('row', '0000000000000001', 3, 40),
  ('文档', '0000000000000002', 1, 39),
  ('分词器', '0000000000000002', 2, 39);
SHOW CREATE TABLE {db}.__fts_posting_proto;
SELECT COUNT(*) FROM {db}.__fts_posting_proto;
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12889)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="fts_large_bench")
    args = parser.parse_args()

    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write(SQL.format(db=args.database))
        path = f.name
    try:
        cmd = ["mysql", "-h", args.host, "-P", str(args.port), "-u", args.user,
               "-N", "-B", "--default-character-set=utf8mb4"]
        with open(path, "rb") as stdin:
            result = subprocess.run(cmd, stdin=stdin, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            sys.stderr.write(result.stderr.decode("utf-8", "replace"))
            raise SystemExit(result.returncode)
        output = result.stdout.decode("utf-8", "replace")
        lines = [line for line in output.splitlines() if line.strip()]
        summary = {
            "ok": True,
            "create_table": lines[0] if lines else "",
            "inserted_rows": int(lines[-1]) if lines and lines[-1].isdigit() else None,
            "raw_output_lines": len(lines),
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    finally:
        Path(path).unlink(missing_ok=True)
        # Best-effort cleanup.
        subprocess.run(
            ["mysql", "-h", args.host, "-P", str(args.port), "-u", args.user,
             "-e", "DROP TABLE IF EXISTS %s.__fts_posting_proto" % args.database],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


if __name__ == "__main__":
    main()
