#!/usr/bin/env python3
"""Structural checks for agent/conversation.json.

Mirrors the rules the evaluation imposes on the material file:
  * valid UTF-8 JSON, top-level object, no duplicate keys, no NaN/Infinity;
  * at most 128 KiB;
  * every evidence pointer resolves inside the document and its quote appears
    verbatim at that location, at most 240 characters;
  * a 0.5-level instance carries no result evidence, a higher level must carry it.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

MAX_BYTES = 128 * 1024
MAX_QUOTE = 240


def fail(messages: list[str], text: str) -> None:
    messages.append(text)


def resolve(document, pointer: str):
    if not pointer.startswith("/"):
        raise ValueError(f"pointer must start with '/': {pointer}")
    node = document
    for token in pointer.lstrip("/").split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(node, list):
            node = node[int(token)]
        elif isinstance(node, dict):
            node = node[token]
        else:
            raise ValueError(f"cannot descend into {type(node).__name__} at {pointer}")
    return node


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "agent/conversation.json")
    if not path.exists():
        print(f"error: {path} not found", file=sys.stderr)
        return 2

    raw = path.read_bytes()
    messages: list[str] = []

    if len(raw) > MAX_BYTES:
        fail(messages, f"file is {len(raw)} bytes, limit is {MAX_BYTES}")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        print(f"error: not valid UTF-8: {exc}", file=sys.stderr)
        return 1

    duplicates: list[str] = []

    def hook(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                duplicates.append(key)
            seen[key] = value
        return seen

    def reject_constant(value):
        raise ValueError(f"non-standard JSON constant: {value}")

    try:
        document = json.loads(text, object_pairs_hook=hook, parse_constant=reject_constant)
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"error: invalid JSON: {exc}", file=sys.stderr)
        return 1

    if not isinstance(document, dict):
        fail(messages, "top level must be a JSON object")
    if duplicates:
        fail(messages, f"duplicate JSON keys: {sorted(set(duplicates))}")
    if document.get("synthetic") is not False:
        fail(messages, "synthetic must be false for real learning material")

    instances = document.get("instances", [])
    if not isinstance(instances, list):
        fail(messages, "instances must be a list")
        instances = []

    for index, instance in enumerate(instances):
        label = f"instances[{index}]"
        level = instance.get("level")
        if level not in (0.5, 1, 1.5, 2):
            fail(messages, f"{label}: unsupported level {level!r}")

        student = instance.get("student_evidence")
        if not isinstance(student, dict):
            fail(messages, f"{label}: student_evidence missing")
        else:
            _check_evidence(document, label + ".student_evidence", student, messages)

        result = instance.get("result_evidence")
        if level == 0.5:
            if result:
                fail(messages, f"{label}: level 0.5 must not carry result_evidence")
        else:
            if not isinstance(result, dict):
                fail(messages, f"{label}: level {level} requires result_evidence")
            else:
                _check_evidence(document, label + ".result_evidence", result, messages)

        dimension = instance.get("dimension")
        if dimension not in {
            "问题理解与自主拆解",
            "技术理解与迁移",
            "独立验证与质疑",
            "实验与反馈迭代",
            "取舍与反思",
        }:
            fail(messages, f"{label}: unknown dimension {dimension!r}")

    for message in messages:
        print(f"[FAIL] {message}")
    if messages:
        print(f"\n{len(messages)} problem(s)")
        return 1

    total = sum(float(i.get("level", 0)) for i in instances)
    print(f"conversation.json ok: {len(raw)} bytes, {len(instances)} instance(s), "
          f"raw total {total:.1f}")
    return 0


def _check_evidence(document, label: str, evidence: dict, messages: list[str]) -> None:
    pointer = evidence.get("pointer")
    quote = evidence.get("quote")
    if not isinstance(pointer, str) or not isinstance(quote, str):
        fail(messages, f"{label}: pointer and quote must be strings")
        return
    if len(quote) > MAX_QUOTE:
        fail(messages, f"{label}: quote is {len(quote)} chars, limit is {MAX_QUOTE}")
    try:
        target = resolve(document, pointer)
    except (KeyError, IndexError, ValueError) as exc:
        fail(messages, f"{label}: pointer {pointer} does not resolve ({exc})")
        return
    if not isinstance(target, str):
        fail(messages, f"{label}: pointer {pointer} does not address a string")
        return
    if quote not in target:
        fail(messages, f"{label}: quote not found verbatim at {pointer}")


if __name__ == "__main__":
    sys.exit(main())
