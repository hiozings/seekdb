#!/usr/bin/env python3
"""Local conformance driver for MiniVectorDB.

The official task package drives the binary through stdin/stdout with a
mysqltest-style comparison. This driver mirrors that shape locally so the
wire format can be re-verified after the package is published.

Usage:
  driver.py                 # run every case
  driver.py --update        # regenerate expected outputs
  driver.py --filter vec    # run matching cases only
  driver.py --bin PATH      # override the binary
  driver.py -v              # show diffs
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASES = ROOT / "tests" / "conformance" / "cases"
EXPECTED = ROOT / "tests" / "conformance" / "expected"
DEFAULT_BIN = ROOT / "build" / "minivectordb"


def normalize(text: str) -> str:
    """Mirrors the official runner: strip trailing horizontal whitespace."""
    lines = text.split("\n")
    return "\n".join(line.rstrip(" \t\r") for line in lines)


def run_case(binary: Path, case: Path, env: dict[str, str]) -> str:
    payload = case.read_bytes()
    proc = subprocess.run(
        [str(binary), "--data-dir", os.environ.get("MVDB_DATA_DIR", "/tmp/mvdb-conformance")],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        timeout=60,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{case.name}: exit code {proc.returncode}\n{proc.stderr.decode('utf-8', 'replace')}"
        )
    return proc.stdout.decode("utf-8", "replace")


def first_diff(expected: str, actual: str) -> str:
    exp_lines = expected.split("\n")
    act_lines = actual.split("\n")
    for i in range(max(len(exp_lines), len(act_lines))):
        exp = exp_lines[i] if i < len(exp_lines) else "<missing>"
        act = act_lines[i] if i < len(act_lines) else "<missing>"
        if exp != act:
            return f"line {i + 1}:\n  expected: {exp!r}\n  actual:   {act!r}"
    return "no difference"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=os.environ.get("MVDB_BIN", str(DEFAULT_BIN)))
    parser.add_argument("--filter", default="")
    parser.add_argument("--update", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--env", action="append", default=[], metavar="K=V")
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    for item in args.env:
        key, _, value = item.partition("=")
        env[key] = value

    cases = sorted(CASES.glob("*.test"))
    if args.filter:
        cases = [c for c in cases if args.filter in c.name]
    if not cases:
        print("error: no cases found", file=sys.stderr)
        return 2

    EXPECTED.mkdir(parents=True, exist_ok=True)
    failures = 0
    for case in cases:
        actual = normalize(run_case(binary, case, env))
        expected_path = EXPECTED / (case.stem + ".result")
        if args.update:
            expected_path.write_text(actual, encoding="utf-8")
            print(f"[updated] {case.name}")
            continue
        if not expected_path.exists():
            print(f"[missing] {case.name}: expected file not found")
            failures += 1
            continue
        expected = normalize(expected_path.read_text(encoding="utf-8"))
        if expected == actual:
            print(f"[ok]      {case.name}")
        else:
            failures += 1
            print(f"[FAIL]    {case.name}")
            if args.verbose:
                print("  " + first_diff(expected, actual).replace("\n", "\n  "))

    total = len(cases)
    if args.update:
        print(f"\nupdated {total} case(s)")
        return 0
    print(f"\n{total - failures}/{total} passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
