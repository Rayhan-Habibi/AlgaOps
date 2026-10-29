import pytest
from gwtest import feed

from algaops_gateway.audit.verify import verify_database
from algaops_gateway.diagnostics.feedback import FeedbackError, validate_feedback


def test_validation():
    fb = validate_feedback(3, "false_alarm", "  sensor cable loose ", "AS")
    assert fb.note == "sensor cable loose" and fb.technician == "AS"
    for args in [(0, "true_alarm", "", "AS"), (1, "maybe", "", "AS"), (1, "unsure", "", ""),
                 (1, "unsure", "x" * 501, "AS"), (1, "unsure", "bad\x07", "AS")]:
        with pytest.raises(FeedbackError):
            validate_feedback(*args)


def test_feedback_recorded_and_audited(pipeline, clock, tmp_path):
    feed(pipeline, clock, 25)
    feed(pipeline, clock, 4, start_seq=26, self_test="fail")    # untrusted -> alert after 3
    alert = pipeline.db.writer.execute("SELECT id, kind FROM alerts").fetchone()
    assert alert["kind"] == "CHECK_SENSOR"
    res = pipeline.record_feedback(alert["id"], "true_alarm", "accelerometer replaced", "RH")
    assert res["verdict"] == "true_alarm"
    with pytest.raises(KeyError):
        pipeline.record_feedback(9999, "true_alarm", "", "RH")
    rep = verify_database(pipeline.db.writer, pipeline.hasher)
    assert rep.ok, rep.issues
    # editing the stored feedback afterwards is detected
    pipeline.db.writer.execute("UPDATE feedback SET verdict = 'false_alarm'")
    rep = verify_database(pipeline.db.writer, pipeline.hasher)
    assert not rep.ok and any(i.kind == "ROW_MODIFIED" for i in rep.issues)
