"""Optional read-only context from a PBR monitoring CSV export.

Only enabled when the PBR owner has approved access (config:
pbr.enabled = true AND pbr.permission_confirmed = true).  The file is opened
read-only and tailed by byte offset; nothing is ever written to the PBR or
its files.  Values are shown as context on the dashboard and are never used
in any diagnostic decision.  No energy/CO2 saving is computed or claimed.
"""

from __future__ import annotations

import csv
import io
import math
import threading
import time
from pathlib import Path
from typing import Any

MAX_COLUMNS = 32
MAX_ROW_BYTES = 4096


class PbrReadonlyCsvSource:
    def __init__(self, path: str | Path, permission_confirmed: bool, keep_rows: int = 200) -> None:
        if not permission_confirmed:
            raise PermissionError("PBR data access requires explicit permission_confirmed")
        self.path = Path(path)
        self.keep_rows = keep_rows
        self._offset = 0
        self._header: list[str] | None = None
        self._rows: list[dict[str, Any]] = []
        self._lock = threading.Lock()
        self.errors = 0
        self.last_poll_ms: int | None = None

    def poll(self) -> int:
        """Read rows appended since the last poll; returns the number of new rows."""
        if not self.path.is_file():
            return 0
        size = self.path.stat().st_size
        if size < self._offset:          # file rotated/truncated: start over
            self._offset, self._header = 0, None
        new = 0
        with open(self.path, "rb") as fh:  # binary read-only handle
            fh.seek(self._offset)
            chunk = fh.read()
        last_nl = chunk.rfind(b"\n")
        if last_nl < 0:
            return 0
        complete, self._offset = chunk[:last_nl + 1], self._offset + last_nl + 1
        for raw in complete.splitlines():
            if not raw.strip() or len(raw) > MAX_ROW_BYTES:
                self.errors += int(bool(raw.strip()))
                continue
            try:
                cells = next(csv.reader(io.StringIO(raw.decode("utf-8"))))
            except (UnicodeDecodeError, csv.Error, StopIteration):
                self.errors += 1
                continue
            if self._header is None:
                self._header = [c.strip()[:48] for c in cells[:MAX_COLUMNS]]
                continue
            row: dict[str, Any] = {}
            for k, v in zip(self._header, cells[:MAX_COLUMNS]):
                v = v.strip()
                try:
                    f = float(v)
                    row[k] = f if math.isfinite(f) else None
                except ValueError:
                    row[k] = v[:64]
            with self._lock:
                self._rows.append(row)
                del self._rows[:-self.keep_rows]
            new += 1
        self.last_poll_ms = time.time_ns() // 1_000_000
        return new

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            rows = list(self._rows[-20:])
        return {"file": self.path.name, "columns": self._header, "latest": rows,
                "parse_errors": self.errors, "last_poll_ms": self.last_poll_ms,
                "used_for_diagnosis": False}
