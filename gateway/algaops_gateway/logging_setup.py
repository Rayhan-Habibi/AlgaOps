"""Logging: human-readable or JSON lines, optional rotating file."""

from __future__ import annotations

import json
import logging
import logging.handlers
from pathlib import Path


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        doc = {"ts": self.formatTime(record, "%Y-%m-%dT%H:%M:%S"), "level": record.levelname,
               "logger": record.name, "msg": record.getMessage()}
        if record.exc_info:
            doc["exc"] = self.formatException(record.exc_info)
        return json.dumps(doc, ensure_ascii=False)


def setup_logging(level: str = "INFO", file: str | Path | None = None, as_json: bool = False
                  ) -> None:
    root = logging.getLogger()
    root.handlers.clear()
    root.setLevel(level.upper())
    fmt: logging.Formatter = JsonFormatter() if as_json else logging.Formatter(
        "%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    console = logging.StreamHandler()
    console.setFormatter(fmt)
    root.addHandler(console)
    if file:
        Path(file).parent.mkdir(parents=True, exist_ok=True)
        fh = logging.handlers.RotatingFileHandler(file, maxBytes=5_000_000, backupCount=5,
                                                  encoding="utf-8")
        fh.setFormatter(fmt)
        root.addHandler(fh)
    # paho / uvicorn access logs are noisy at INFO
    logging.getLogger("uvicorn.access").setLevel(logging.WARNING)
    logging.getLogger("amqtt").setLevel(logging.WARNING)
