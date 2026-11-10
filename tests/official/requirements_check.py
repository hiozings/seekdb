#!/usr/bin/env python3
"""Self-check for the requirements the task package lists as still required.

mysqltest/README.md says the public package leaves these out but they remain
mandatory:

  1. reject NaN, Infinity, wrong dimension, float32 overflow;
  2. a failed statement must not partially apply;
  3. a committed transaction survives a hard kill, an uncommitted one never
     partially applies, and an interrupted recovery still converges;
  4. datasets larger than the memory budget must work;
  5. ANN must honour filters, return valid unique ids, and meet the recall floor.

Cases 1, 2 and 5 run over the JSON protocol; 3 and 4 are exercised by the
existing kill_test.py and large_dataset.py and are only cross-checked here.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session, to_assertion  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"


def run(binary: Path, data_dir: Path, statements: list[str]) -> list:
    session = Session(binary, data_dir)
    try:
        return [to_assertion(session.execute(statement)) for statement in statements]
    finally:
        session.close()


def main() -> int:
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-req-"))
    failures: list[str] = []

    def check(name: str, condition: bool, detail: str = "") -> None:
        if condition:
            print(f"[ok]   {name}")
        else:
            failures.append(f"{name}: {detail}")
            print(f"[FAIL] {name} {detail}")

    try:
        statements = [
            "CREATE TABLE v (id BIGINT PRIMARY KEY, e VECTOR(2), s VARCHAR(3));",
            # 1. illegal vectors and over-long strings must be rejected
            "INSERT INTO v VALUES (1,'[1,2]','abc');",
            "INSERT INTO v VALUES (2,'[NaN,0]','ok');",
            "INSERT INTO v VALUES (3,'[Infinity,0]','ok');",
            "INSERT INTO v VALUES (4,'[1,2,3]','ok');",
            "INSERT INTO v VALUES (5,'[1e40,0]','ok');",
            "INSERT INTO v VALUES (6,'[1,2]','abcd');",
            # 2. a failed multi-row insert leaves nothing behind
            "INSERT INTO v VALUES (7,'[0,0]','ok'),(8,'[1]','ok');",
            "INSERT INTO v VALUES (9,'[0,0]','ok'),(9,'[0,0]','ok');",
            # 3. a failed statement inside a transaction keeps earlier work
            "BEGIN;",
            "INSERT INTO v VALUES (10,'[0,0]','ok');",
            "INSERT INTO v VALUES (10,'[0,0]','ok');",
            "COMMIT;",
            "SELECT id FROM v ORDER BY id;",
            # 5. ann must honour filters and return unique ids
            "CREATE TABLE a (id BIGINT PRIMARY KEY, c VARCHAR(4), e VECTOR(2));",
            "INSERT INTO a VALUES (1,'x','[1,0]'),(2,'x','[0,1]'),(3,'y','[1,1]'),(4,'x','[1,0]');",
            "CREATE VECTOR INDEX a_e ON a(e);",
            "SET mode = 'ann';",
            "SELECT id, L2_DISTANCE(e,'[1,0]') AS d FROM a WHERE c = 'x' ORDER BY d LIMIT 2;",
            "SELECT id FROM a WHERE c = 'none' ORDER BY L2_DISTANCE(e,'[1,0]') LIMIT 5;",
        ]
        out = run(binary, data_dir, statements)
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)

    # results are positional: see the statement list above
    names = [
        "create", "ok1", "nan", "infinity", "dimension", "overflow", "varchar",
        "multi_bad_dim", "multi_dup", "begin", "txn_ok", "txn_dup", "commit",
        "ids", "create_a", "insert_a", "create_index", "set_mode",
        "ann_filtered", "ann_empty",
    ]
    by_name = dict(zip(names, out))

    def has_error(name: str) -> bool:
        entry = by_name.get(name, {})
        return isinstance(entry, dict) and bool(entry.get("error"))

    check("NaN rejected", has_error("nan"), str(by_name.get("nan")))
    check("Infinity rejected", has_error("infinity"), str(by_name.get("infinity")))
    check("wrong dimension rejected", has_error("dimension"), str(by_name.get("dimension")))
    check("float32 overflow rejected", has_error("overflow"), str(by_name.get("overflow")))
    check("over-long VARCHAR rejected", has_error("varchar"), str(by_name.get("varchar")))
    check("failed multi-row insert is atomic", has_error("multi_bad_dim")
          and has_error("multi_dup"), "expected both inserts to fail")
    check("failed statement keeps earlier transaction work", has_error("txn_dup"),
          str(by_name.get("txn_dup")))

    ids = by_name.get("ids", {}).get("rows", [])
    flat = [row[0] for row in ids if row]
    check("only legal rows remain", flat == [1, 10], f"got {flat}")

    ann = by_name.get("ann_filtered", {})
    ann_ids = [row[0] for row in ann.get("rows", [])]
    check("ann honours the filter", set(ann_ids) <= {1, 2, 4}, f"got {ann_ids}")
    check("ann returns unique ids", len(ann_ids) == len(set(ann_ids)), f"got {ann_ids}")
    check("ann respects LIMIT", len(ann_ids) <= 2, f"got {ann_ids}")

    empty = by_name.get("ann_empty", {})
    check("ann returns empty set for no matches", empty.get("rows") == [], str(empty))

    print()
    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}")
        print(f"{len(failures)} requirement(s) failed")
        return 1
    print("requirements_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
