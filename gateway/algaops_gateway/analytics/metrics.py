"""Pure metric helpers (no I/O).  All rates are None when undefined (0/0)."""

from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass
class Confusion:
    tp: int = 0
    fp: int = 0
    tn: int = 0
    fn: int = 0

    def add(self, predicted: bool, actual: bool) -> None:
        if predicted and actual:
            self.tp += 1
        elif predicted:
            self.fp += 1
        elif actual:
            self.fn += 1
        else:
            self.tn += 1

    def merge(self, other: Confusion) -> None:
        self.tp += other.tp
        self.fp += other.fp
        self.tn += other.tn
        self.fn += other.fn

    @property
    def precision(self) -> float | None:
        return ratio(self.tp, self.tp + self.fp)

    @property
    def recall(self) -> float | None:
        return ratio(self.tp, self.tp + self.fn)

    @property
    def fpr(self) -> float | None:
        return ratio(self.fp, self.fp + self.tn)

    @property
    def f1(self) -> float | None:
        p, r = self.precision, self.recall
        if p is None or r is None or p + r == 0:
            return None
        return 2 * p * r / (p + r)

    def as_dict(self) -> dict:
        d = asdict(self)
        d.update(precision=rnd(self.precision), recall=rnd(self.recall), fpr=rnd(self.fpr),
                 f1=rnd(self.f1), support=self.tp + self.fn, negatives=self.fp + self.tn)
        return d


def ratio(a: int, b: int) -> float | None:
    return None if b == 0 else a / b


def rnd(x: float | None, nd: int = 4) -> float | None:
    return None if x is None else round(x, nd)


def latency_stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "mean": None, "p50": None, "p95": None, "max": None}
    v = sorted(values)

    def pct(p: float) -> float:
        return v[min(len(v) - 1, int(round(p * (len(v) - 1))))]
    return {"n": len(v), "mean": rnd(sum(v) / len(v), 3), "p50": rnd(pct(0.5), 3),
            "p95": rnd(pct(0.95), 3), "max": rnd(v[-1], 3)}
