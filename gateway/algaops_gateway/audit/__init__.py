"""Tamper-evident audit trail: canonical JSON, Ascon-Hash256 chain, checkpoints."""

from .ascon import ZERO_HASH, Hasher
from .canonical import canonical_bytes, canonical_json
from .hash_chain import AuditChain, AuditEvent
from .verify import VerificationReport, verify_database

__all__ = ["ZERO_HASH", "Hasher", "canonical_bytes", "canonical_json", "AuditChain",
           "AuditEvent", "VerificationReport", "verify_database"]
