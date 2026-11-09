"""Automated deployment rehearsal and lifecycle verification test (WP-4A-12)."""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

from tools.rehearse_deployment import rehearse_deployment


def test_full_deployment_rehearsal():
    """Verify that a complete deployment rehearsal runs and passes all 9 phases."""
    assert rehearse_deployment() is True

