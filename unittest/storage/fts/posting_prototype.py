#!/usr/bin/env python3
"""M0 prototype for the FTS posting-index project.

Tokenizes the benchmark documents through the engine's TOKENIZE SQL function
and builds an in-memory (word, doc_id, word_count, doc_length) posting list.
It only reads the benchmark database; it does not modify any engine file.
"""
import argparse
import collections
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run_mysql(args, sql):
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write(sql)
        path = f.name
    try:
        cmd = [
            "mysql",
            "-h", args.host,
            "-P", str(args.port),
            "-u", args.user,
            "-N", "-B",
            "--default-character-set=utf8mb4",
        ]
        with open(path, "rb") as stdin:
            result = subprocess.run(cmd, stdin=stdin, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            sys.stderr.write(result.stderr.decode("utf-8", "replace"))
            raise SystemExit("mysql failed with %d" % result.returncode)
        return result.stdout.decode("utf-8", "replace")
    finally:
        Path(path).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12889)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="fts_large_bench")
    parser.add_argument("--parser", default="ik")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    sql = (
        "SELECT id, tokenize(cast(concat(title, ' ', content) as char), '%s') "
        "FROM %s.docs;" % (args.parser, args.database)
    )
    output = run_mysql(args, sql)

    docs = 0
    total_tokens = 0
    postings = 0
    terms = set()
    per_doc = []
    zero_docs = 0
    doc_freq = collections.Counter()
    total_freq = collections.Counter()
    parse_errors = 0

    for line in output.splitlines():
        if not line:
            continue
        try:
            _doc_id, raw = line.split("\t", 1)
            tokens = json.loads(raw)
        except Exception:
            parse_errors += 1
            continue
        docs += 1
        per_doc.append(len(tokens))
        total_tokens += len(tokens)
        if not tokens:
            zero_docs += 1
            continue
        freq = collections.Counter(tokens)
        postings += len(freq)
        terms.update(freq)
        doc_freq.update(freq.keys())
        total_freq.update(tokens)

    per_doc.sort()
    result = {
        "parser": args.parser,
        "source": "title + content",
        "docs": docs,
        "parse_errors": parse_errors,
        "zero_token_docs": zero_docs,
        "total_tokens": total_tokens,
        "posting_rows": postings,
        "unique_terms": len(terms),
        "avg_tokens_per_doc": round(total_tokens / docs, 3) if docs else 0,
        "tokens_per_doc_p50": per_doc[len(per_doc) // 2] if per_doc else 0,
        "tokens_per_doc_p95": per_doc[int(len(per_doc) * 0.95)] if per_doc else 0,
        "tokens_per_doc_max": per_doc[-1] if per_doc else 0,
        "top_terms": doc_freq.most_common(20),
        "sample_token_freq": total_freq.most_common(10),
    }
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
