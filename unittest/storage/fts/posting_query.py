#!/usr/bin/env python3
"""M3 prototype: answer MATCH-like queries from the populated posting table.

Tokenizes the query through the engine, intersects doc_id sets per token, and
compares the result count with the official benchmark hits.  Read-only.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


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


def tokenize(args, query):
    sql = "SELECT tokenize('%s', '%s');" % (query.replace("'", "''"), args.parser)
    return json.loads(mysql(args, sql).strip())


def doc_ids(args, word):
    sql = "SELECT doc_id FROM `%s` WHERE word = '%s';" % (
        args.table, word.replace("\\", "\\\\").replace("'", "''"))
    return {line for line in mysql(args, sql).splitlines() if line}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12889)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="fts_large_bench")
    parser.add_argument("--table", required=True)
    parser.add_argument("--parser", default="ik")
    parser.add_argument("--query", required=True)
    parser.add_argument("--expected", type=int)
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()

    words = tokenize(args, args.query)
    sets = [doc_ids(args, w) for w in words]
    result = set().union(*sets) if sets else set()
    count = len(result)
    summary = {
        "query": args.query,
        "tokens": words,
        "posting_counts": [len(s) for s in sets],
        "union_count": count,
        "limit": args.limit,
        "reported": min(count, args.limit) if args.limit else count,
        "expected": args.expected,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if args.expected is not None:
        shown = min(count, args.limit) if args.limit else count
        sys.exit(0 if shown == args.expected else 1)


if __name__ == "__main__":
    main()
