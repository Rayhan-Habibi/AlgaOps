"""Retention building blocks: what a plan selects, what verification accepts as archived."""

import json

import pytest
from gwtest import feed

from algaops_gateway import retention
from algaops_gateway.audit.hash_chain import EVENT_TYPES
from algaops_gateway.audit.verify import _Archives
from algaops_gateway.pipeline import Pipeline


def test_archive_coverage_needs_seq_and_time_of_one_archive():
    a = _Archives()
    a.events += [(100, 5_000), (300, 4_000)]   # the later run used a longer retention
    a.freeze()
    assert a.covers(100, 4_999) and a.covers(1, 0)
    assert not a.covers(100, 5_000), "not older than the cutoff"
    assert a.covers(250, 3_999) and not a.covers(250, 4_000)
    assert not a.covers(301, 0), "after the last archived event"
    empty = _Archives()
    empty.freeze()
    assert not empty.covers(1, 0)


def test_plan_selects_old_rows_and_keeps_the_newest(pipeline, clock):
    feed(pipeline, clock, 10)
    w = pipeline.db.writer
    w.execute("INSERT INTO gaps (node_id, boot_id, from_seq, to_seq, missing, detected_ms) "
              "VALUES ('node-a', 7, 1, 1, 1, 0), ('node-a', 7, 2, 2, 1, 0)")
    plan = retention.plan_prune(w, clock.t + 1)            # everything is older
    assert plan.diagnostics == plan.windows == 9, "newest decision and window stay"
    assert plan.gaps == 1 and plan.gaps_hi == 1, "newest gap stays (row ids are not reused)"
    assert plan.through_seq == w.execute("SELECT audit_seq FROM diagnostics WHERE id = 9"
                                         ).fetchone()[0]
    assert plan.counts() == {"windows": 9, "diagnostics": 9, "gaps": 1, "ingest_log": 0}
    assert plan.events == pipeline.chain.head[0]
    nothing = retention.plan_prune(w, 0)
    assert nothing.empty and nothing.summary()["rows"]["diagnostics"] == 0


def test_plan_keeps_decisions_named_by_an_alert(pipeline, clock):
    feed(pipeline, clock, 5)
    feed(pipeline, clock, 4, start_seq=6, samples_valid=10)     # alert opens on the 3rd
    feed(pipeline, clock, 2, start_seq=10)
    w = pipeline.db.writer
    opening = w.execute("SELECT open_diag_id FROM alerts").fetchone()[0]
    plan = retention.plan_prune(w, clock.t + 1)
    assert plan.kept_for_alerts == 1 and plan.diagnostics == 9   # 11 - newest - opening
    assert opening < plan.diag_hi


def test_archive_created_is_a_registered_event_type(repo_root):
    assert "archive.created" in EVENT_TYPES
    schema = json.loads((repo_root / "schemas" / "audit_event.v1.schema.json").read_text())
    assert set(EVENT_TYPES) <= set(schema["properties"]["type"]["enum"])


def test_retention_needs_an_archive_directory(lib, policy, db):
    with pytest.raises(ValueError, match="archive_dir"):
        Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw", retention_days=30)


def test_prune_now_needs_a_retention_period(pipeline):
    with pytest.raises(ValueError, match="retention_days"):
        pipeline.prune_now()
