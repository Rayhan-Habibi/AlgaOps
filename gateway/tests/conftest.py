"""Shared fixtures for gateway tests (helpers live in gwtest.py)."""

from __future__ import annotations

import time
from pathlib import Path

import pytest
from gwtest import Clock, DevBroker, free_port

from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.config import DEFAULT_POLICY, REPO_ROOT
from algaops_gateway.core.engine import load_policy
from algaops_gateway.core.libalgaops import load_library
from algaops_gateway.pipeline import Pipeline
from algaops_gateway.storage.database import Database


@pytest.fixture(scope="session")
def lib():
    return load_library()


@pytest.fixture(scope="session")
def policy(lib):
    return load_policy(lib, DEFAULT_POLICY)


@pytest.fixture(scope="session")
def legacy_policy(lib):
    return load_policy(lib, None)


@pytest.fixture
def db(tmp_path):
    d = Database(tmp_path / "test.db")
    yield d
    d.close()


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
def pipeline(lib, policy, db, clock, tmp_path):
    p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="test-gw", clock=clock,
                 checkpoint_writer=CheckpointWriter(tmp_path / "checkpoints.jsonl"),
                 checkpoint_every_events=50, checkpoint_every_s=3600)
    p.open()
    yield p
    p.close()


@pytest.fixture
def broker():
    port = free_port()
    b = DevBroker(port)
    b.start()
    time.sleep(0.2)
    yield ("127.0.0.1", port, b)
    b.stop()


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return REPO_ROOT