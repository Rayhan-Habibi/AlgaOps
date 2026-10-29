-- AlgaOps gateway schema v1.
-- windows / diagnostics rows are referenced by digest from audit_events, so
-- any modification or deletion of a stored row is detectable by verification.

CREATE TABLE IF NOT EXISTS nodes (
    node_id         TEXT PRIMARY KEY,
    data_source     TEXT NOT NULL,
    first_seen_ms   INTEGER NOT NULL,
    last_seen_ms    INTEGER NOT NULL,
    last_boot_id    INTEGER,
    fw_version      TEXT,
    status_json     TEXT,
    status_ms       INTEGER
);

CREATE TABLE IF NOT EXISTS windows (
    id              INTEGER PRIMARY KEY,
    node_id         TEXT NOT NULL,
    boot_id         INTEGER NOT NULL,
    seq             INTEGER NOT NULL,
    t_end_ms        INTEGER NOT NULL,
    received_ms     INTEGER NOT NULL,
    data_source     TEXT NOT NULL,
    payload_digest  TEXT NOT NULL,          -- Ascon-Hash256 of the raw MQTT payload
    record_json     TEXT NOT NULL,          -- canonical normalised window
    record_digest   TEXT NOT NULL,          -- Ascon-Hash256 of record_json
    vib_rms_g       REAL,
    temp_c          REAL,
    current_a       REAL,
    UNIQUE (node_id, boot_id, seq, payload_digest)
);
CREATE INDEX IF NOT EXISTS idx_windows_node_time ON windows (node_id, received_ms);

CREATE TABLE IF NOT EXISTS diagnostics (
    id              INTEGER PRIMARY KEY,
    window_id       INTEGER REFERENCES windows (id),
    node_id         TEXT NOT NULL,
    kind            TEXT NOT NULL,          -- window | stream_timeout | malformed
    decided_ms      INTEGER NOT NULL,
    ref_ms          INTEGER NOT NULL,       -- window end (node clock) or receive time
    outcome         TEXT NOT NULL,
    trust_score     INTEGER NOT NULL,
    is_trusted      INTEGER NOT NULL,
    flags           INTEGER NOT NULL,
    mode            TEXT NOT NULL,
    method          TEXT,
    alert_event     TEXT NOT NULL,
    latency_ms      REAL,
    decision_json   TEXT NOT NULL,          -- canonical diagnostic_event.v1
    decision_digest TEXT NOT NULL,
    audit_seq       INTEGER
);
CREATE INDEX IF NOT EXISTS idx_diag_node_time ON diagnostics (node_id, decided_ms);
CREATE INDEX IF NOT EXISTS idx_diag_outcome ON diagnostics (outcome);

CREATE TABLE IF NOT EXISTS alerts (
    id              INTEGER PRIMARY KEY,
    node_id         TEXT NOT NULL,
    kind            TEXT NOT NULL,          -- CHECK_SENSOR | CHECK_EQUIPMENT
    status          TEXT NOT NULL,          -- open | cleared | superseded
    opened_ms       INTEGER NOT NULL,
    closed_ms       INTEGER,
    open_diag_id    INTEGER REFERENCES diagnostics (id),
    close_diag_id   INTEGER REFERENCES diagnostics (id),
    summary         TEXT NOT NULL,
    last_reason     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_alerts_node ON alerts (node_id, status);

CREATE TABLE IF NOT EXISTS baselines (
    id              INTEGER PRIMARY KEY,
    node_id         TEXT NOT NULL,
    mode            TEXT NOT NULL,
    status          TEXT NOT NULL,          -- candidate | active | retired
    created_ms      INTEGER NOT NULL,
    activated_ms    INTEGER,
    n               INTEGER NOT NULL,
    stats_json      TEXT NOT NULL,          -- canonical statistics
    stats_digest    TEXT NOT NULL,
    source_json     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_baselines_node ON baselines (node_id, mode, status);

CREATE TABLE IF NOT EXISTS feedback (
    id              INTEGER PRIMARY KEY,
    alert_id        INTEGER NOT NULL REFERENCES alerts (id),
    verdict         TEXT NOT NULL,          -- true_alarm | false_alarm | unsure
    note            TEXT NOT NULL,
    technician      TEXT NOT NULL,
    created_ms      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS gaps (
    id              INTEGER PRIMARY KEY,
    node_id         TEXT NOT NULL,
    boot_id         INTEGER NOT NULL,
    from_seq        INTEGER NOT NULL,
    to_seq          INTEGER NOT NULL,
    missing         INTEGER NOT NULL,
    detected_ms     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_gaps_node ON gaps (node_id, detected_ms);

CREATE TABLE IF NOT EXISTS ingest_log (
    id              INTEGER PRIMARY KEY,
    received_ms     INTEGER NOT NULL,
    topic           TEXT NOT NULL,
    node_id         TEXT,
    status          TEXT NOT NULL,          -- accepted | duplicate_delivery | rejected
    reason          TEXT,
    raw_digest      TEXT NOT NULL,
    raw_excerpt     TEXT
);
CREATE INDEX IF NOT EXISTS idx_ingest_status ON ingest_log (status, received_ms);

CREATE TABLE IF NOT EXISTS audit_events (
    seq             INTEGER PRIMARY KEY,    -- 1, 2, 3, ... (no gaps)
    ts_ms           INTEGER NOT NULL,
    type            TEXT NOT NULL,
    node_id         TEXT,
    payload         TEXT NOT NULL,          -- canonical JSON, hashed verbatim
    prev_hash       TEXT NOT NULL,
    hash            TEXT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS checkpoints (
    seq             INTEGER PRIMARY KEY,
    hash            TEXT NOT NULL,
    ts_ms           INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS settings (
    key             TEXT PRIMARY KEY,
    value           TEXT NOT NULL
);
