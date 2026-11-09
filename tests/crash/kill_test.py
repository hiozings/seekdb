#!/usr/bin/env python3
"""Crash-consistency test: force-kill the process and check the invariants.

Invariants under test (from the task statement):
  * a transaction whose COMMIT was acknowledged must survive a restart;
  * a transaction that never received a commit request must not appear;
  * a partially applied transaction is never visible.

The driver talks to the binary over stdin/stdout, using MVDB_DML_OUTPUT=affected
so every statement produces exactly one acknowledgement line.

Usage:
  kill_test.py --trials 8
"""

from __future__ import annotations

import argparse
import os
import random
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = ROOT / "build" / "minivectordb"


class Session:
    def __init__(self, binary: Path, data_dir: Path):
        env = os.environ.copy()
        env["MVDB_DML_OUTPUT"] = "affected"
        self.proc = subprocess.Popen(
            [str(binary), "--data-dir", str(data_dir)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )

    def send(self, sql: str, timeout: float = 10.0) -> str:
        assert self.proc.stdin is not None
        self.proc.stdin.write(sql.rstrip() + ";\n")
        self.proc.stdin.flush()
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            raise TimeoutError(f"no response to {sql!r}")
        line = self.proc.stdout.readline()
        if line == "":
            raise RuntimeError(f"process died while running {sql!r}")
        return line.rstrip("\n")

    def kill(self) -> None:
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait(timeout=10)

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def query_ids(binary: Path, data_dir: Path, table: str = "t") -> list[int]:
    env = os.environ.copy()
    proc = subprocess.run(
        [str(binary), "--data-dir", str(data_dir)],
        input=f"SELECT id FROM {table} ORDER BY id;\n",
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"restart failed: {proc.stderr}")
    lines = proc.stdout.splitlines()
    return [int(line) for line in lines[1:] if line.strip() != ""]


def run_trial(binary: Path, seed: int, verbose: bool) -> list[str]:
    rng = random.Random(seed)
    failures: list[str] = []
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-crash-"))

    try:
        # Phase 1: autocommitted inserts, killed at a random point.
        session = Session(binary, data_dir)
        session.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16))")
        acknowledged: list[int] = []
        kill_after = rng.randint(1, 40)
        for i in range(1, 200):
            session.send(f"INSERT INTO t VALUES ({i}, 'v{i}')")
            acknowledged.append(i)
            if i == kill_after:
                break
        session.kill()

        ids = query_ids(binary, data_dir)
        if ids != acknowledged:
            failures.append(
                f"seed={seed}: acknowledged {acknowledged[-3:]}.., found {ids[-3:]} "
                f"(count {len(ids)} vs {len(acknowledged)})"
            )

        # Phase 2: an explicit transaction that never commits must vanish.
        session = Session(binary, data_dir)
        session.send("BEGIN")
        for i in range(1000, 1050):
            session.send(f"INSERT INTO t VALUES ({i}, 'uncommitted')")
        session.kill()

        ids = query_ids(binary, data_dir)
        leaked = [i for i in ids if i >= 1000]
        if leaked:
            failures.append(f"seed={seed}: uncommitted rows leaked: {leaked[:5]}")
        if ids[: len(acknowledged)] != acknowledged:
            failures.append(f"seed={seed}: committed rows lost after uncommitted crash")

        # Phase 3: a transaction whose COMMIT was acknowledged must survive an
        # immediate kill.
        session = Session(binary, data_dir)
        session.send("BEGIN")
        for i in range(2000, 2020):
            session.send(f"INSERT INTO t VALUES ({i}, 'committed')")
        session.send("COMMIT")
        session.kill()  # kill right after the acknowledgement

        ids = query_ids(binary, data_dir)
        missing = [i for i in range(2000, 2020) if i not in ids]
        if missing:
            failures.append(f"seed={seed}: committed rows missing after kill: {missing[:5]}")

        # Phase 4: kill after a COMMIT was sent but before its acknowledgement.
        # The task allows this window to be all-or-nothing, never partial.
        session = Session(binary, data_dir)
        session.send("BEGIN")
        for i in range(3000, 3020):
            session.send(f"INSERT INTO t VALUES ({i}, 'window')")
        assert session.proc.stdin is not None
        session.proc.stdin.write("COMMIT;\n")
        session.proc.stdin.flush()
        time.sleep(rng.uniform(0.0, 0.003))
        session.kill()

        ids = query_ids(binary, data_dir)
        window = [i for i in ids if 3000 <= i < 3020]
        if window and len(window) != 20:
            failures.append(f"seed={seed}: partially applied commit window: {len(window)}/20 rows")

        # Phase 5: kill during recovery, then recover again on the next start.
        # Replay must be idempotent.
        for attempt in range(3):
            proc = subprocess.Popen(
                [str(binary), "--data-dir", str(data_dir)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(rng.uniform(0.0, 0.002))
            proc.kill()
            proc.wait()

        ids = query_ids(binary, data_dir)
        if ids != sorted(set(ids)):
            failures.append(f"seed={seed}: duplicated rows after repeated recovery")

        if verbose:
            print(f"  seed={seed}: {len(ids)} rows, {len(failures)} failure(s)")
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trials", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--bin", default=str(DEFAULT_BIN))
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(args.bin).resolve()
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    started = time.time()
    failures: list[str] = []
    for trial in range(args.trials):
        seed = args.seed + trial
        if args.verbose:
            print(f"trial {trial + 1}/{args.trials}")
        failures.extend(run_trial(binary, seed, args.verbose))

    elapsed = time.time() - started
    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}")
        print(f"\nFAILED: {len(failures)} violation(s) in {elapsed:.1f}s")
        return 1
    print(f"kill_test: {args.trials} trial(s) passed in {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
