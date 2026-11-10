#!/usr/bin/env python3
"""Minimal client for the official JSONL stdio protocol.

Used by the local test drivers so they exercise the same path the judge uses:
start the binary, wait for {"type":"ready"}, send one request per SQL and read
one response, then shut down through op=shutdown.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


class ProtocolError(RuntimeError):
    pass


class Session:
    def __init__(self, binary: Path, data_dir: Path, extra_args: list[str] | None = None):
        self.binary = binary
        self.data_dir = data_dir
        self.extra_args = extra_args or []
        self.proc: subprocess.Popen | None = None
        self.next_id = 1
        self.start()

    def start(self) -> None:
        self.proc = subprocess.Popen(
            [str(self.binary), "--data-dir", str(self.data_dir), *self.extra_args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        line = self.proc.stdout.readline()
        if not line:
            raise ProtocolError("process exited before the ready handshake")
        try:
            hello = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ProtocolError(f"ready line is not JSON: {line!r} ({exc})") from exc
        if hello != {"type": "ready"}:
            raise ProtocolError(f"unexpected ready object: {hello!r}")

    def _send(self, payload: dict) -> dict:
        assert self.proc is not None and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(json.dumps(payload, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if line == "":
            raise ProtocolError("process exited before responding")
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            raise ProtocolError(f"response is not JSON: {line!r} ({exc})") from exc

    def execute(self, sql: str, mode: str = "exact") -> dict:
        ident = self.next_id
        self.next_id += 1
        response = self._send({"id": ident, "op": "execute", "sql": sql, "search_mode": mode})
        if response.get("id") != ident:
            raise ProtocolError(f"response id {response.get('id')!r} does not match request {ident}")
        return response

    def shutdown(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            ident = self.next_id
            self.next_id += 1
            response = self._send({"id": ident, "op": "shutdown"})
            if response.get("ok") is not True:
                raise ProtocolError(f"shutdown failed: {response!r}")
            self.proc.stdin.close()
            code = self.proc.wait(timeout=30)
            self.last_stderr = self.proc.stderr.read() or ""
            if code != 0:
                raise ProtocolError(f"process exited with {code} after shutdown")
        self.proc = None

    def restart(self) -> None:
        self.shutdown()
        self.next_id = 1
        self.start()

    def stderr_text(self) -> str:
        if self.proc is not None and self.proc.stderr is not None:
            try:
                self.last_stderr = self.proc.stderr.read() or ""
            except Exception:
                pass
        return getattr(self, "last_stderr", "")

    def close(self) -> None:
        try:
            self.shutdown()
        except Exception:
            if self.proc is not None:
                self.proc.kill()
                self.proc.wait()
        self.proc = None


def to_assertion(response: dict) -> dict:
    """Maps a protocol response onto the shape used by .result files."""
    if response.get("ok") is not True:
        error = response.get("error") or {}
        code = error.get("code") or "error"
        return {"error": code}
    if "columns" in response:
        return {"columns": response["columns"], "rows": response.get("rows", [])}
    if "affected_rows" in response:
        return {"affected_rows": response["affected_rows"]}
    return {}
