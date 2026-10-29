"""Canonical JSON serialisation ("AlgaOps canonical JSON v1").

Rules: UTF-8, object keys sorted, no insignificant whitespace, non-ASCII kept
as UTF-8 (not \\u-escaped), floats in Python's shortest round-trip repr,
NaN/Infinity forbidden, only str keys, tuples serialised as lists.  The exact
bytes produced here are what the audit chain hashes and what is stored, so
verification never re-serialises: it hashes the stored text.
"""

from __future__ import annotations

import json
import math
from typing import Any


class CanonicalError(ValueError):
    """Value cannot be represented in canonical JSON."""


def _check(value: Any, path: str = "$") -> None:
    if isinstance(value, bool) or value is None or isinstance(value, (int, str)):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise CanonicalError(f"non-finite float at {path}")
        return
    if isinstance(value, (list, tuple)):
        for i, v in enumerate(value):
            _check(v, f"{path}[{i}]")
        return
    if isinstance(value, dict):
        for k, v in value.items():
            if not isinstance(k, str):
                raise CanonicalError(f"non-string key {k!r} at {path}")
            _check(v, f"{path}.{k}")
        return
    raise CanonicalError(f"unsupported type {type(value).__name__} at {path}")


def canonical_json(value: Any) -> str:
    _check(value)
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False)


def canonical_bytes(value: Any) -> bytes:
    return canonical_json(value).encode("utf-8")


def finite_or_none(x: float | None) -> float | None:
    """Map NaN/Inf to None so measurements can always be canonicalised."""
    if x is None:
        return None
    x = float(x)
    return x if math.isfinite(x) else None
