"""AlgaOps Assurance edge gateway.

Read-only, offline-first ingestion of sensor-node telemetry over a local MQTT
broker.  Every diagnostic decision is made by the C11 core (libalgaops,
loaded through ctypes); this package only validates payloads, persists
results to SQLite, maintains the Ascon-Hash256 audit chain and serves the
local dashboard API.  It never sends commands to the pump, blower or PBR.
"""

__version__ = "2.0.0"
