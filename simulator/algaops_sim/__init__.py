"""AlgaOps deterministic scenario simulator (DEMO / SYNTHETIC data only).

Emulates an ESP32 sensor node at the level of its MQTT output (telemetry
window summaries), including sensor faults, equipment anomalies and network
effects.  Ground-truth labels are produced separately from the payloads and
are never published, so they cannot leak into the gateway's decisions.
"""

__version__ = "2.0.0"
