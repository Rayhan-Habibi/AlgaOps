# Evidence and verification

Every window, decision, alert change, rejected message, stream timeout,
baseline action, technician feedback, export, retention archive and
verification run is an event in an append-only hash chain.  The chain is **tamper-evident, not
tamper-proof**: it shows that recorded data was changed, removed or reordered;
it cannot stop someone who controls the gateway from rewriting everything.

## Construction

```
hash_i = Ascon-Hash256( "AlgaOps/audit/v1" || 0x00 || hash_{i-1} || be64(i) || payload_i )
hash_0 = 32 zero bytes,  i = 1, 2, 3, ...
```

* Ascon-Hash256 as standardised in NIST SP 800-232 (reference implementation
  from ascon-c, checked against the official known-answer vectors).
* `payload_i` is canonical JSON: keys sorted, no whitespace, UTF-8, no NaN or
  infinity; it is stored verbatim and contains the event type, sequence number,
  node id and time, so the indexed columns can be checked against it.
* Window records, decisions and baseline statistics are stored as canonical
  JSON as well; their Ascon-Hash256 digests are part of the referencing event.
* Checkpoints `(seq, hash)` are written every 500 events or 5 minutes, at
  shutdown, before exports and around archives, into the database **and** appended to an
  external JSON-lines file (`storage.checkpoint_file`).  Put that file on other
  media, copy it off the gateway regularly, or print the head hash: an
  independently kept checkpoint is what exposes a complete rewrite.

## Verifying on the gateway

```bash
algaops-gateway --config /etc/algaops/gateway.toml verify     # exit 0 = verified, 3 = issues found
```

or **Audit -> Verify chain now** in the dashboard.  Issue kinds:

| Kind | Meaning |
|---|---|
| `HASH_MISMATCH` | an event's content or hash was changed |
| `BROKEN_LINK` | `prev_hash` does not match the previous event (reordering, insertion) |
| `SEQUENCE_GAP` | an event is missing |
| `METADATA_MISMATCH` | indexed columns (type, node, time) differ from the hashed payload |
| `ROW_MODIFIED` | a window, decision, baseline, alert or feedback row differs from its audited digest |
| `MISSING_ROW` / `UNREFERENCED_ROW` | a referenced row was deleted / a row was added without an event (rows pruned by [retention](#retention-archive-then-prune) count as `rows_archived` instead) |
| `TRUNCATED` | a checkpoint lies beyond the chain head: newest events were removed |
| `CHECKPOINT_MISMATCH` | a checkpoint hash, or the archive head recorded in an `archive.created` event, differs from the chain |

## Evidence bundles

Dashboard **Export**, `POST /api/export/evidence` (API token) or
`algaops-gateway export [--node ID] [--since MS] [--until MS]` writes a zip:

| File | Content |
|---|---|
| `audit_events.jsonl` | the complete chain |
| `windows.jsonl`, `diagnostics.jsonl` | rows in scope, with their digests |
| `baselines.jsonl`, `alerts.csv`, `feedback.csv`, `nodes.json` | context |
| `gaps.csv`, `ingest_log.csv` | sequence gaps; rejected messages and exact redeliveries, in scope (not chained, covered by the digests) |
| `checkpoints.jsonl` | database and external checkpoints (the external file as far as it was written when the export began: later lines name events the bundle does not hold) |
| `verification_report.json` | gateway verification at export time |
| `manifest.json` | scope, data label (DEMO / SYNTHETIC or LIVE HARDWARE), policy hash, engine version, chain head |
| `SHA-ASCON256SUMS` | Ascon-Hash256 of every other file |

Verify on any computer with Python 3.11+, without the gateway or the C core:

```bash
python -I tools/verify_evidence.py algaops-evidence-gw-01-....zip   # exit 0 verified, 3 failed
python -I tools/verify_evidence.py bundle.zip --json                # full report
```

The verifier uses its own pure-Python Ascon implementation (derived from the
CC0 pyascon reference and checked against it and the NIST vectors), so a defect
in the gateway's hashing would not hide itself.  It processes roughly
0.35 MB/s; a bundle of one node-day takes on the order of 15 minutes.

The C command line writes and checks its own evidence directories:
`algaops --evidence DIR ...` and `algaops --verify-evidence DIR`.

## Retention: archive, then prune

`[storage] retention_days = 0`, the default, keeps every row.  With
`retention_days = N` the gateway prunes history older than N days (gateway
clock minus N days), 10 minutes after it starts and then every 24 hours, and
never before that history is in an archive bundle:

1. **Plan** (read-only snapshot): windows, decisions, gaps and ingest-log rows
   older than the cutoff.  Never deleted: audit events, alerts (open or
   closed), baselines, feedback, checkpoints, nodes; the decisions an alert was
   opened, changed or cleared with, and their windows; the newest window,
   decision, gap and ingest-log row, so that a row id is never reused.
2. **Archive**: a checkpoint is forced, then an ordinary evidence bundle scoped
   to everything before the cutoff (complete chain, the rows, `gaps.csv`,
   `ingest_log.csv`, checkpoints) is written to `storage.export_dir` as
   `algaops-evidence-<gateway>-archive-<UTC time>.zip` and flushed to disk.
   If the gateway-side verification of that snapshot reports any issue, the
   bundle is discarded and nothing is pruned.
3. **Prune**: the transaction that appends an `archive.created` event also
   deletes the first batch (1,000 decisions with their windows, then gap and
   ingest-log rows by the 1,000); the other batches follow one per pipeline
   tick, between incoming messages, and a checkpoint then anchors the event in
   the external file.  One transaction for a node-day (67,500 windows) blocked
   ingestion for about 17 s on a desktop PC, longer than the 15 s stream
   timeout; one batch took under 0.25 s.  A gateway stopped in the middle
   leaves rows that the next run archives again.

`archive.created` records `cutoff_ms`, `retention_days`,
`archived_through_seq` (the highest seq whose rows are pruned), `file`,
`bundle_digest` (Ascon-Hash256 of the zip file), `bundle_head_seq` /
`bundle_head_hash` (the chain head inside the bundle), the row counts (`prune`)
and `kept_for_alerts`.  List them with `GET /api/audit/events?type=archive.created`.

By hand:

```bash
algaops-gateway --config /etc/algaops/gateway.toml prune --dry-run   # counts; changes nothing
algaops-gateway --config /etc/algaops/gateway.toml prune [--days N]  # with the gateway stopped (enforced)
```

`--dry-run` opens the database read-only and may run next to the gateway.  A
real `prune`, like `baseline import` and `activate`, needs the gateway stopped:
two writers would fork the chain head, and the gateway's next append would fail
on the `audit_events` primary key until a restart.  This is enforced.  The
gateway holds an exclusive lock on `<database>.writer.lock`, next to the
database, while it runs; a write command started meanwhile is refused with exit
status 2 before it appends anything, and its message names the process holding
the lock.  A gateway started during a `prune` exits with status 2 in
the same way.  The operating system releases the lock when its process ends,
also when it is killed.  `verify` and `export` take no lock.  While the gateway
runs, the daily run prunes.  Exit status: 0 done or nothing to prune, 3
verification failed (nothing pruned), 2 no retention period, no database, or
the database is in use by the gateway or another write command.

**Verification after pruning.**  A window or decision whose audit event has
`seq <= archived_through_seq` and `ts_ms < cutoff_ms` of an `archive.created`
event, and that no alert event names, may be absent: it is counted in
`rows_archived` (with a note naming the latest archive), not as
`MISSING_ROW`.  Everything else is checked as before: rows that remain against
their digests, every other missing row, the whole chain from seq 1, the
checkpoints, and the bundle head recorded in each `archive.created`.  Evidence
bundles exported before and after a prune verify with
`tools/verify_evidence.py`; for those made afterwards it lists the chain's
archives (`archives` in `--json`) and notes that their rows are elsewhere.

**Keeping archives.**  Check an archive with `python -I tools/verify_evidence.py
<archive>.zip`: the chain head it prints must equal `bundle_head_seq` /
`bundle_head_hash` of its `archive.created` event.  The gateway never deletes
archives; copy them off the gateway together with `checkpoints.jsonl` before
removing them from `export_dir`, otherwise the pruned history is gone.

## Limits

* Whoever can rewrite the database **and** every copy of the checkpoints can
  produce a consistent forged chain (demonstrated in
  `tests/e2e/test_audit_tamper_detection.py`); compare head hashes with an
  independently kept bundle or checkpoint.
* Integrity of records is not correctness of measurements: a sensor that was
  mounted badly produces faithfully chained bad data.  TrustGuard flags what it
  can detect; the rest is installation quality.
* Times come from the gateway clock and the nodes' SNTP clock (chrony on the
  gateway); without an RTC or upstream time source absolute dates can be off.
* Rows pruned by retention are vouched for only by their `archive.created`
  event: whoever can append to the chain can declare rows archived and delete
  them.  Keep every archive bundle off the gateway and check that each
  `archive.created` event in the chain matches one you hold (file, digest,
  chain head).
* Storage grows by about 4 KB per window (~276 MB per node-day at 1.28 s
  windows) while `retention_days = 0`.  Retention never prunes the audit chain:
  about 0.9 KB per window (60 MB per node-day, measured) stays for ever.  SQLite
  reuses freed pages, so the file stops growing (except for the chain) but does
  not shrink; `VACUUM` with the gateway stopped returns the space.
* Every bundle, each archive included, holds the complete chain, so each daily
  archive is bigger and slower than the one before: per node-day of chain about
  13 MB of zip (62 MB unpacked) and, on a desktop PC, about 4.5 s to write and
  verify (not measured on a Raspberry Pi).  Archives kept on the gateway add up
  accordingly.  Memory does not grow with the chain: files are streamed into
  the zip and hashed as they are written, and verification meets the windows
  and decisions in id order while it walks the chain.  Peak Python memory of a
  bundle was 7.7 MB for one node-day and for three (966 MB for one node-day
  before bundles were streamed); the whole process peaked at about 35 MB.
  What verification keeps grows only with tables that are never pruned
  (alerts, feedback, baselines, `archive.created` events).  A chain continued
  in a new segment is not implemented.
* `tools/verify_evidence.py` still reads every file of a bundle into memory:
  about 395 MB for one node-day of chain on a desktop PC.  Check bundles of a
  long chain on a computer with memory to match.
