"""Method comparison on stored (unlabelled) decisions.

Live data has no ground truth, so this reports how often each method flags
trusted windows and how often the methods agree — not accuracy.  Accuracy
metrics come only from labelled scenarios (evaluate.py).
"""

from __future__ import annotations

import json
import sqlite3
from collections import Counter
from typing import Any

METHODS = ("fixed", "zscore", "mahalanobis")


def compare_methods(conn: sqlite3.Connection, node_id: str | None = None,
                    since_ms: int | None = None, until_ms: int | None = None,
                    limit: int = 50000) -> dict[str, Any]:
    clauses, args = ["kind = 'window'", "is_trusted = 1"], []
    if node_id:
        clauses.append("node_id = ?")
        args.append(node_id)
    if since_ms is not None:
        clauses.append("decided_ms >= ?")
        args.append(since_ms)
    if until_ms is not None:
        clauses.append("decided_ms <= ?")
        args.append(until_ms)
    rows = conn.execute(f"SELECT decision_json FROM diagnostics WHERE {' AND '.join(clauses)} "
                        "ORDER BY id DESC LIMIT ?", (*args, limit)).fetchall()
    per_mode: dict[str, dict[str, Counter]] = {}
    agreement = Counter()
    windows = Counter()
    for r in rows:
        d = json.loads(r[0])
        mode = d["mode"]
        windows[mode] += 1
        pm = per_mode.setdefault(mode, {m: Counter() for m in METHODS})
        verdicts = {}
        for m in METHODS:
            res = d["methods"].get(m, {})
            st = res.get("status", "skipped")
            pm[m][st] += 1
            if st == "ok":
                pm[m]["anomalous"] += int(bool(res.get("anomalous")))
                verdicts[m] = bool(res.get("anomalous"))
        if len(verdicts) >= 2:
            agreement["compared"] += 1
            agreement["all_agree"] += int(len(set(verdicts.values())) == 1)
            if "zscore" in verdicts and "mahalanobis" in verdicts:
                agreement["zscore_vs_mahalanobis_compared"] += 1
                agreement["zscore_vs_mahalanobis_agree"] += int(
                    verdicts["zscore"] == verdicts["mahalanobis"])
    summary = {}
    for mode, ms in per_mode.items():
        summary[mode] = {}
        for m, c in ms.items():
            ok = c.get("ok", 0)
            summary[mode][m] = {"evaluated": ok, "unavailable": c.get("unavailable", 0),
                                "flagged": c.get("anomalous", 0),
                                "flag_rate": round(c["anomalous"] / ok, 4) if ok else None}
    return {"windows": dict(windows), "per_mode": summary, "agreement": dict(agreement),
            "note": "Unlabelled live data: flag rates and agreement only, not accuracy."}
