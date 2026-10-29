"""Live vertical slice shared by the end-to-end tests (DEMO / SYNTHETIC data):

    simulator (1x real time) -> MQTT broker (amqtt dev broker, password auth)
      -> gateway: MQTT ingest -> C core -> SQLite + Ascon audit chain -> HTTP API

Five short scenarios are published in parallel (about 65 s of telemetry plus
the stream timeouts afterwards) and the gateway is restarted once mid-run.
Ground-truth labels stay in this process and never enter a payload.

Run only these tests with  pytest -m e2e ; skip them with  pytest -m "not e2e".
"""

from __future__ import annotations

from pathlib import Path

import pytest
from e2e_support import start_system


def pytest_collection_modifyitems(items):
    here = Path(__file__).resolve().parent
    for item in items:
        if Path(item.path).resolve().is_relative_to(here):
            item.add_marker(pytest.mark.e2e)


@pytest.fixture(scope="session")
def system(tmp_path_factory):
    from algaops_gateway.core.libalgaops import load_library
    try:
        load_library()
    except Exception as exc:  # pragma: no cover - depends on the local build
        pytest.skip(f"C core library not built: {exc}")
    sysm = start_system(tmp_path_factory.mktemp("e2e"))
    yield sysm
    sysm.close()
