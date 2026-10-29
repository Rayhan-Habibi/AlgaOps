# AlgaOps gateway

Read-only edge service for the plant network:

```
MQTT (QoS 1, persistent session) -> strict schema check -> C11 core via ctypes (ABI-checked)
  -> one SQLite transaction: window + decision + gaps + audit event + alert change -> MQTT ack
  -> local HTTP API, server-sent events, dashboard
```

It subscribes only; it never publishes to the sensor network and controls
nothing.  All decision logic is in the C core (`algaops-assurance/`); the
gateway stores, audits and serves it.

## Modules

| Package | Role |
|---|---|
| `ingest/` | topic parsing, strict payload schema, idempotency (exact redelivery vs conflicting duplicate), node status, paho-mqtt client (manual ack after commit, back-off) |
| `core/` | ctypes binding with struct-offset ABI check, policy loading (`policy.default.toml`, hash recorded with every decision), per-node engine |
| `pipeline.py` | single worker thread: messages, 1 s ticks (stream timeouts), checkpoints, baseline capture, warm start from history |
| `retention.py` | optional daily archive-then-prune of old windows, decisions, gaps and ingest log (`[storage] retention_days`) |
| `audit/` | canonical JSON, Ascon-Hash256 binding, hash chain, checkpoints (database + external file), verification |
| `diagnostics/` | decision records (`diagnostic_event.v1`), baseline management, technician feedback |
| `storage/` | SQLite (WAL, `synchronous=FULL`), migrations, repositories, single-writer lock |
| `api/` | FastAPI routes, SSE event bus, security headers (strict CSP), API token, static dashboard |
| `export/` | evidence bundles, CSV / JSON-lines exports |
| `analytics/` | labelled evaluation on simulator scenarios, metrics, method comparison |
| `sources/` | optional read-only PBR context from a CSV export (needs the owner's approval) |

## Command line

```
algaops-gateway [--config gateway.toml] run                  # MQTT ingest + API + dashboard
algaops-gateway devbroker [--port 1883]                      # development broker (amqtt); use Mosquitto in production
algaops-gateway [--config ...] verify                        # audit chain; exit 3 on any issue
algaops-gateway [--config ...] export [--node ID] [--since MS] [--until MS] [--out FILE]
algaops-gateway [--config ...] prune [--dry-run] [--days N]  # archive, then prune old rows (gateway stopped, enforced; --dry-run only counts)
algaops-gateway [--config ...] evaluate [--full] [--out FILE] # labelled synthetic evaluation
algaops-gateway [--config ...] replay FIXTURE --db FILE       # offline replay into a separate database
algaops-gateway [--config ...] baseline list|import|activate # import/activate with the gateway stopped (enforced)
algaops-gateway [--config ...] policy                        # effective policy and its hash
```

Without installation: `PYTHONPATH=gateway:simulator python -m algaops_gateway ...`.

**One writer per database.**  Two processes appending to the same audit chain
would fork its head, and the gateway's next append would fail on the
`audit_events` primary key until a restart.  The gateway (`run`) and every
command that appends (`prune`, `baseline import|activate`, `replay` into its
`--db`) therefore hold an exclusive lock on `<database>.writer.lock`, next to
the database, while their pipeline is open (`storage/writer_lock.py`).  A write
command started while the gateway runs is refused with exit status 2 and a
message naming the holder (pid, start time, command line); a gateway started
while a write command runs exits with status 2 as well.  Read-only commands
(`verify`, `export`, `prune --dry-run`, `baseline list`, `policy`) take no lock
and work next to a running gateway.  The operating system releases the lock
when its process ends, also when it is killed; the file stays (never delete it
while a gateway runs: a new file would let a second writer in).

## Configuration

Copy `config/gateway.example.toml`; relative paths resolve against the config
file.  Secrets are referenced as files (`password_file`, `token_file`), never
inline.  Environment overrides: `ALGAOPS_DB`, `ALGAOPS_CHECKPOINTS`,
`ALGAOPS_MQTT_HOST`, `ALGAOPS_MQTT_PORT`, `ALGAOPS_API_HOST`, `ALGAOPS_API_PORT`,
`ALGAOPS_POLICY`, `ALGAOPS_LIB`.

## Security model

* The API binds to 127.0.0.1.  A non-loopback address is refused unless an API
  token file (≥ 16 characters) is configured or `allow_insecure_lan = true`.
  Every write endpoint (feedback, baselines, exports, verification, replay)
  requires the token when one is configured.
* Strict Content-Security-Policy, no third-party assets, no outbound
  connections (the e2e suite checks that the whole run stays on loopback).
* MQTT with password authentication and a topic ACL (deploy/mosquitto); the
  gateway user can only read.
* OpenAPI documentation at `/api/docs` while the gateway runs.

## Tests

```bash
python -m pytest gateway/tests          # unit, integration (MQTT, API, storage), security, contracts
python -m pytest tests/e2e              # live vertical slice (~2 min)
```
