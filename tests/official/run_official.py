#!/usr/bin/env python3
"""Run the official public sample package (tests/official) against the binary.

Protocol implemented here follows mysqltest/README.md:
  * one SQL statement per non-empty line, one JSON object per statement;
  * `--restart` closes the process and starts it again on the same data dir;
  * `--mode ann|exact` sets the query mode and produces no result;
  * directives never occupy an assertion slot.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"
OFFICIAL = ROOT / "tests" / "official"
FLOAT_TOLERANCE = 1e-5


class Session:
    def __init__(self, binary: Path, data_dir: Path):
        self.binary = binary
        self.data_dir = data_dir
        self.proc: subprocess.Popen | None = None
        self.start()

    def start(self):
        self.proc = subprocess.Popen(
            [str(self.binary), "--data-dir", str(self.data_dir)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def send_sql(self, sql: str) -> dict:
        assert self.proc is not None and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(sql.rstrip() + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if line == "":
            raise RuntimeError(f"process exited while running {sql!r}")
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"non-JSON response to {sql!r}: {line!r} ({exc})") from exc

    def send_directive(self, directive: str) -> None:
        assert self.proc is not None and self.proc.stdin
        self.proc.stdin.write(directive + "\n")
        self.proc.stdin.flush()

    def restart(self):
        assert self.proc is not None and self.proc.stdin
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        if self.proc.returncode != 0:
            raise RuntimeError(f"process exited with {self.proc.returncode} on --restart")
        self.start()

    def close(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def close_enough(expected, actual) -> bool:
    if isinstance(expected, bool) or isinstance(actual, bool):
        return expected == actual
    if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
        diff = abs(float(expected) - float(actual))
        return diff <= FLOAT_TOLERANCE or diff <= FLOAT_TOLERANCE * abs(float(expected))
    return expected == actual


def rows_equal(expected: list, actual: list, unordered: bool) -> bool:
    if len(expected) != len(actual):
        return False
    if not unordered:
        return all(
            isinstance(e, list) and isinstance(a, list) and len(e) == len(a)
            and all(close_enough(x, y) for x, y in zip(e, a))
            for e, a in zip(expected, actual)
        )
    remaining = list(actual)
    for row in expected:
        for index, candidate in enumerate(remaining):
            if isinstance(row, list) and isinstance(candidate, list) and len(row) == len(candidate) \
                    and all(close_enough(x, y) for x, y in zip(row, candidate)):
                remaining.pop(index)
                break
        else:
            return False
    return not remaining


def compare(expected: dict, actual: dict) -> str | None:
    if "error" in expected:
        if "error" not in actual:
            return f"expected error {expected['error']!r}, got {actual}"
        if not actual["error"]:
            return "error name must be non-empty"
        if expected["error"] != actual["error"]:
            return f"error name mismatch: expected {expected['error']!r}, got {actual['error']!r}"
        return None
    if "error" in actual:
        return f"unexpected error {actual['error']!r}"
    if expected == {}:
        return None
    if "affected_rows" in expected:
        if actual.get("affected_rows") != expected["affected_rows"]:
            return f"affected_rows: expected {expected['affected_rows']}, got {actual.get('affected_rows')}"
        return None
    if "columns" in expected:
        if actual.get("columns") != expected["columns"]:
            return f"columns: expected {expected['columns']}, got {actual.get('columns')}"
        if not rows_equal(expected.get("rows", []), actual.get("rows", []),
                          bool(expected.get("unordered"))):
            return f"rows: expected {expected.get('rows')}, got {actual.get('rows')}"
        return None
    return None


def run_case(binary: Path, case_id: str, verbose: bool) -> list[str]:
    test_path = OFFICIAL / f"{case_id}.test"
    result_path = OFFICIAL / f"{case_id}.result"
    expected = json.loads(result_path.read_text(encoding="utf-8"))
    lines = [line.strip() for line in test_path.read_text(encoding="utf-8").splitlines()]

    data_dir = Path(tempfile.mkdtemp(prefix=f"mvdb-official-{case_id}-"))
    failures: list[str] = []
    session = Session(binary, data_dir)
    index = 0
    try:
        for line_number, line in enumerate(lines, start=1):
            if not line:
                continue
            if line.startswith("--"):
                directive = line.lower()
                if directive == "--restart":
                    session.restart()
                elif directive.startswith("--mode"):
                    session.send_directive(line)
                continue
            if index >= len(expected):
                failures.append(f"line {line_number}: more statements than expected results")
                break
            actual = session.send_sql(line)
            problem = compare(expected[index], actual)
            if problem:
                failures.append(f"statement {index + 1} (line {line_number}): {problem}\n"
                                f"    sql: {line}")
            index += 1
    finally:
        session.close()
        shutil.rmtree(data_dir, ignore_errors=True)

    if index != len(expected):
        failures.append(f"statement count: expected {len(expected)}, produced {index}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=str(DEFAULT_BIN))
    parser.add_argument("--filter", default="")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    manifest = json.loads((OFFICIAL / "manifest.json").read_text(encoding="utf-8"))
    case_ids = [case["id"] for case in manifest["cases"]]
    if args.filter:
        case_ids = [c for c in case_ids if args.filter in c]

    failures = 0
    for case_id in case_ids:
        try:
            problems = run_case(binary, case_id, args.verbose)
        except Exception as exc:  # noqa: BLE001 - report and continue
            failures += 1
            print(f"[ERROR] {case_id}: {exc}")
            continue
        if problems:
            failures += 1
            print(f"[FAIL]  {case_id}")
            for problem in problems[:4]:
                print(f"  {problem}")
        else:
            print(f"[ok]    {case_id}")

    print(f"\n{len(case_ids) - failures}/{len(case_ids)} official cases passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
