from algaops_gateway.audit.checkpoint import (
    Checkpoint,
    CheckpointWriter,
    checkpoint_file_size,
    iter_checkpoint_file,
    read_checkpoint_file,
    read_db_checkpoints,
)


def test_write_db_and_file(db, tmp_path):
    w = CheckpointWriter(tmp_path / "cp" / "checkpoints.jsonl")
    with db.transaction() as tx:
        w.write(tx, Checkpoint(10, "a" * 64, 1000))
        w.write(tx, Checkpoint(20, "b" * 64, 2000))
    f = read_checkpoint_file(tmp_path / "cp" / "checkpoints.jsonl")
    assert [c.seq for c in f.checkpoints] == [10, 20] and f.skipped_lines == 0
    assert [c.seq for c in read_db_checkpoints(db.writer)] == [10, 20]


def test_partial_last_line_is_skipped(tmp_path):
    p = tmp_path / "checkpoints.jsonl"
    p.write_bytes(b'{"hash":"' + b"a" * 64 + b'","seq":1,"ts_ms":5}\n{"hash":"bb","se')
    f = read_checkpoint_file(p)
    assert len(f.checkpoints) == 1 and f.skipped_lines == 1


def test_missing_file_is_empty(tmp_path):
    f = read_checkpoint_file(tmp_path / "none.jsonl")
    assert f.checkpoints == [] and f.skipped_lines == 0
    assert checkpoint_file_size(tmp_path / "none.jsonl") == 0
    assert list(iter_checkpoint_file(tmp_path / "none.jsonl", 10)) == []


def test_file_read_up_to_an_earlier_size(db, tmp_path):
    """Lines appended after the size was taken are not read; a line cut by it is
    read as far as it went (like a partial write)."""
    p = tmp_path / "checkpoints.jsonl"
    w = CheckpointWriter(p)
    with db.transaction() as tx:
        w.write(tx, Checkpoint(10, "a" * 64, 1000))
        size = checkpoint_file_size(p)
        w.write(tx, Checkpoint(20, "b" * 64, 2000))
    assert [c.seq for c in iter_checkpoint_file(p, size)] == [10]
    assert [c and c.seq for c in iter_checkpoint_file(p, size + 5)] == [10, None]
    assert [c.seq for c in iter_checkpoint_file(p)] == [10, 20] == \
        [c.seq for c in iter_checkpoint_file(p, checkpoint_file_size(p))]
