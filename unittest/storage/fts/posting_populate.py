#!/usr/bin/env python3
"""M2 prototype: populate the engine-created FTS posting table.

Reads the benchmark docs through the engine TOKENIZE function and writes
(word, doc_id, word_count, doc_length) rows into the posting aux table that
the feature-flagged DDL created.  It does not modify any official file.
"""
import argparse
import collections
import json
import subprocess
import sys
import tempfile
from pathlib import Path

BATCH = 2000


def mysql(args, sql):
    cmd = ["mysql", "-h", args.host, "-P", str(args.port), "-u", args.user,
           "-N", "-B", "--default-character-set=utf8mb4", args.database]
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write(sql)
        path = f.name
    try:
        with open(path, "rb") as stdin:
            result = subprocess.run(cmd, stdin=stdin, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            sys.stderr.write(result.stderr.decode("utf-8", "replace"))
            raise SystemExit("mysql failed with %d" % result.returncode)
        return result.stdout.decode("utf-8", "replace")
    finally:
        Path(path).unlink(missing_ok=True)


def escape_word(word):
    return word.replace("\\", "\\\\").replace("'", "''")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12889)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="fts_large_bench")
    parser.add_argument("--table", required=True)
    parser.add_argument("--parser", default="ik")
    parser.add_argument("--source", default="title_content",
                        choices=["title_content", "content"])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    source = "concat(title, ' ', content)" if args.source == "title_content" else "content"
    read_sql = ("SELECT id, tokenize(cast(%s as char), '%s') FROM %s.docs;"
                % (source, args.parser, args.database))
    output = mysql(args, read_sql)

    docs = 0
    total_tokens = 0
    posting_rows = 0
    rows = []
    inserts = []
    for line in output.splitlines():
        if not line:
            continue
        doc_id, raw = line.split("\t", 1)
        tokens = json.loads(raw)
        docs += 1
        total_tokens += len(tokens)
        if not tokens:
            continue
        freq = collections.Counter(tokens)
        posting_rows += len(freq)
        doc_len = len(tokens)
        for word, count in freq.items():
            rows.append("('%s', '%s', %d, %d)" % (escape_word(word), doc_id, count, doc_len))
            if len(rows) >= BATCH:
                inserts.append("INSERT INTO `%s` VALUES %s;" % (args.table, ",".join(rows)))
                rows = []
    if rows:
        inserts.append("INSERT INTO `%s` VALUES %s;" % (args.table, ",".join(rows)))
    mysql(args, "\n".join(inserts))

    summary = {
        "table": args.table,
        "docs": docs,
        "total_tokens": total_tokens,
        "posting_rows": posting_rows,
        "insert_batches": len(inserts),
    }
    text = json.dumps(summary, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
