"""ctypes binding to libalgaops (algaops-assurance/, C11).

The struct layouts below mirror include/algaops_types.h, include/baseline.h,
include/health_methods.h and include/algaops_engine.h.  They are checked
against the library at load time with algaops_abi_info(); a mismatch raises
AbiMismatch instead of silently reading garbage.  The engine configuration is
handled as an opaque buffer and set only through the policy-key API, so its
layout never has to be mirrored here.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
import threading
from ctypes import (
    POINTER,
    Structure,
    c_char,
    c_char_p,
    c_double,
    c_int,
    c_int32,
    c_size_t,
    c_uint32,
    c_uint64,
    c_void_p,
)
from pathlib import Path

ABI_VERSION = 2
MAX_FEATURES = 4
MAX_BANDS = 4
HM_COUNT = 3
MODE_COUNT = 4
EXPLANATION_LEN = 1024
HASH_BYTES = 32

# presence bits (algaops_types.h)
HAS_RECEIVED = 1 << 0
HAS_TIMING = 1 << 1
HAS_SAMPLE_COUNTS = 1 << 2
HAS_AXES = 1 << 3
HAS_P2P = 1 << 4
HAS_CURRENT = 1 << 5
HAS_SELF_TEST = 1 << 6
HAS_TIME_SYNC = 1 << 7
HAS_NODE_FAULTS = 1 << 8
HAS_BANDS = 1 << 9

SELF_TEST = {"not_run": 0, "pass": 1, "fail": 2}

NODE_FAULTS = {
    "ACCEL_BUS": 1 << 0,
    "ACCEL_OVERRUN": 1 << 1,
    "ACCEL_MISSING": 1 << 2,
    "TEMP_CRC": 1 << 3,
    "TEMP_MISSING": 1 << 4,
    "TEMP_POR": 1 << 5,
    "CURRENT_ADC": 1 << 6,
    "CURRENT_CLIP": 1 << 7,
}

FLAG_COUNT = 22
OUTCOMES = ("NORMAL", "CHECK_SENSOR", "CHECK_EQUIPMENT", "NOT_EVALUATED")
MODES = ("UNKNOWN", "OFF", "STARTUP", "STEADY")
METHODS = ("fixed", "zscore", "mahalanobis")
FEATURES = ("vib_rms_g", "vib_p2p_g", "temp_c", "current_a")
DECISION_KINDS = ("window", "stream_timeout", "malformed")
ALERT_EVENTS = ("NONE", "OPENED", "CLEARED", "CHANGED")
METHOD_STATUS = ("skipped", "ok", "unavailable")


class AbiMismatch(RuntimeError):
    """The loaded library does not match the struct layouts of this binding."""


class CoreError(RuntimeError):
    """A core API call returned an error status."""

    def __init__(self, func: str, status: int, detail: str = "") -> None:
        super().__init__(f"{func} failed with status {status}{': ' + detail if detail else ''}")
        self.status = status


class AlgaWindow(Structure):
    _fields_ = [
        ("boot_id", c_uint32),
        ("sequence_id", c_uint32),
        ("timestamp_ms", c_uint64),
        ("received_ms", c_uint64),
        ("t_start_ms", c_uint64),
        ("window_ms", c_uint32),
        ("samples_expected", c_uint32),
        ("samples_valid", c_uint32),
        ("presence", c_uint32),
        ("node_fault_flags", c_uint32),
        ("self_test", c_int32),
        ("time_synced", c_int32),
        ("n_bands", c_int32),
        ("vibration_rms_g", c_double),
        ("vib_rms_axis_g", c_double * 3),
        ("vib_p2p_axis_g", c_double * 3),
        ("temperature_c", c_double),
        ("current_a", c_double),
        ("band_rms_g", c_double * MAX_BANDS),
    ]


class AlgaMethodResult(Structure):
    _fields_ = [
        ("status", c_int32),
        ("anomalous", c_int32),
        ("top_feature", c_int32),
        ("dims", c_int32),
        ("score", c_double),
        ("threshold", c_double),
        ("feature_z", c_double * MAX_FEATURES),
    ]


class AlgaDecision(Structure):
    _fields_ = [
        ("kind", c_int32),
        ("boot_id", c_uint32),
        ("sequence_id", c_uint32),
        ("timestamp_ms", c_uint64),
        ("received_ms", c_uint64),
        ("trust_score", c_int32),
        ("is_trusted", c_int32),
        ("flags", c_uint32),
        ("outcome", c_int32),
        ("mode", c_int32),
        ("mode_changed", c_int32),
        ("time_in_mode_ms", c_uint64),
        ("primary_method", c_int32),
        ("decided_by", c_int32),
        ("baseline_id", c_uint32),
        ("feature_mask", c_uint32),
        ("features", c_double * MAX_FEATURES),
        ("methods", AlgaMethodResult * HM_COUNT),
        ("alert_event", c_int32),
        ("alert_kind", c_int32),
        ("alert_prev_kind", c_int32),
        ("consecutive", c_uint32),
        ("missing_count", c_uint32),
        ("completeness", c_double),
        ("explanation", c_char * EXPLANATION_LEN),
        ("reason", c_char * EXPLANATION_LEN),
    ]


class AlgaBaseline(Structure):
    _fields_ = [
        ("id", c_uint32),
        ("mode", c_int32),
        ("feature_mask", c_uint32),
        ("n", c_uint32),
        ("valid", c_int32),
        ("mean", c_double * MAX_FEATURES),
        ("std", c_double * MAX_FEATURES),
        ("cov", c_double * (MAX_FEATURES * MAX_FEATURES)),
    ]


class AlgaBaselineBuilder(Structure):
    _fields_ = [
        ("requested_mask", c_uint32),
        ("seen_mask", c_uint32),
        ("n", c_uint32),
        ("mean", c_double * MAX_FEATURES),
        ("comoment", c_double * (MAX_FEATURES * MAX_FEATURES)),
    ]


class AlgaAbiInfo(Structure):
    _fields_ = [
        (name, c_uint32)
        for name in (
            "abi_version",
            "size_window",
            "size_decision",
            "size_config",
            "size_baseline",
            "size_method_result",
            "off_window_temperature",
            "off_window_band",
            "off_decision_outcome",
            "off_decision_methods",
            "off_decision_alert_event",
            "off_decision_completeness",
            "off_decision_explanation",
            "off_decision_reason",
            "off_baseline_cov",
        )
    ]


class AsconHash256Ctx(Structure):
    """include/ascon_hash256.h.  Not part of algaops_abi_info(): the tests check the
    incremental hash against the one-shot hash and the KAT vectors."""
    _fields_ = [
        ("x", c_uint64 * 5),
        ("buf", ctypes.c_uint8 * 8),
        ("buf_len", c_size_t),
    ]


DecisionCallback = ctypes.CFUNCTYPE(
    None, c_void_p, c_int, POINTER(AlgaWindow), POINTER(AlgaDecision), c_int
)


def _candidate_paths() -> list[Path]:
    here = Path(__file__).resolve()
    repo = here.parents[3]  # gateway/algaops_gateway/core/libalgaops.py -> repo root
    names = ["algaops.dll"] if sys.platform == "win32" else (
        ["libalgaops.dylib"] if sys.platform == "darwin" else ["libalgaops.so"]
    )
    dirs = [
        repo / "algaops-assurance" / "build",
        repo / "build" / "algaops-assurance",
        repo / "build",
        Path(sys.prefix) / "lib",
        Path("/usr/local/lib"),
        Path("/usr/lib"),
    ]
    return [d / n for d in dirs for n in names]


def find_library(explicit: str | os.PathLike[str] | None = None) -> Path:
    """Locate libalgaops: explicit path, $ALGAOPS_LIB, build tree, system."""
    for cand in (explicit, os.environ.get("ALGAOPS_LIB")):
        if cand:
            p = Path(cand)
            if p.is_file():
                return p
            raise FileNotFoundError(f"libalgaops not found at {p}")
    for p in _candidate_paths():
        if p.is_file():
            return p
    found = ctypes.util.find_library("algaops")
    if found:
        return Path(found)
    raise FileNotFoundError(
        "libalgaops shared library not found; build algaops-assurance (scripts/build_all) "
        "or set ALGAOPS_LIB"
    )


class CoreLibrary:
    """Typed wrapper around one loaded libalgaops instance."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self._lib = ctypes.CDLL(str(self.path))
        self._declare()
        self._check_abi()
        self.version = self._lib.algaops_version().decode()
        self.config_size = int(self._lib.algaops_engine_config_size())
        self.blocking_flags = int(self._lib.trustguard_blocking_flags())
        self.flag_names = {
            1 << b: self._lib.trustguard_flag_name(c_uint32(1 << b)).decode()
            for b in range(FLAG_COUNT)
        }
        self.config_keys = [
            self._lib.algaops_config_key_name(i).decode()
            for i in range(self._lib.algaops_config_key_count())
        ]
        self.string_keys = {
            k for k in self.config_keys if self._lib.algaops_config_key_is_string(k.encode()) == 1
        }

    # ---- declarations ------------------------------------------------------
    def _fn(self, name: str, restype, *argtypes) -> None:
        f = getattr(self._lib, name)
        f.restype = restype
        f.argtypes = list(argtypes)

    def _declare(self) -> None:
        F = self._fn
        F("algaops_version", c_char_p)
        F("algaops_abi_info", None, POINTER(AlgaAbiInfo))
        F("algaops_engine_config_default", None, c_void_p)
        F("algaops_engine_config_size", c_size_t)
        F("algaops_config_set_number", c_int, c_void_p, c_char_p, c_double)
        F("algaops_config_set_string", c_int, c_void_p, c_char_p, c_char_p)
        F("algaops_config_get_number", c_int, c_void_p, c_char_p, POINTER(c_double))
        F("algaops_config_get_string", c_char_p, c_void_p, c_char_p)
        F("algaops_config_key_count", c_int)
        F("algaops_config_key_name", c_char_p, c_int)
        F("algaops_config_key_is_string", c_int, c_char_p)
        F("algaops_config_validate", c_int, c_void_p, c_char_p, c_size_t)
        F("algaops_config_load_file", c_int, c_void_p, c_char_p, c_char_p, c_size_t)
        F("algaops_engine_create", c_void_p, c_void_p)
        F("algaops_engine_destroy", None, c_void_p)
        F("algaops_engine_reset", c_int, c_void_p)
        F("algaops_engine_set_baseline", c_int, c_void_p, POINTER(AlgaBaseline))
        F("algaops_engine_clear_baseline", c_int, c_void_p, c_int)
        F("algaops_engine_get_baseline", c_int, c_void_p, c_int, POINTER(AlgaBaseline))
        F("algaops_engine_process", c_int, c_void_p, POINTER(AlgaWindow), POINTER(AlgaDecision))
        F("algaops_engine_tick", c_int, c_void_p, c_uint64, POINTER(AlgaDecision), POINTER(c_int))
        F("algaops_engine_report_malformed", c_int, c_void_p, c_uint64, c_char_p,
          POINTER(AlgaDecision))
        F("algaops_engine_restore_alert", c_int, c_void_p, ctypes.c_int32)
        F("algaops_replay_csv", c_int, c_void_p, c_char_p, DecisionCallback, c_void_p,
          POINTER(c_int))
        F("algaops_baselines_from_csv", c_int, c_void_p, c_char_p, POINTER(AlgaBaseline),
          POINTER(c_int))
        F("algaops_mode_str", c_char_p, c_int)
        F("algaops_method_str", c_char_p, c_int)
        F("algaops_alert_event_str", c_char_p, c_int)
        F("diagnostic_outcome_str", c_char_p, c_int)
        F("trustguard_flag_name", c_char_p, c_uint32)
        F("trustguard_blocking_flags", c_uint32)
        F("alga_window_features", c_uint32, POINTER(AlgaWindow), POINTER(c_double * MAX_FEATURES))
        F("alga_baseline_builder_init", None, POINTER(AlgaBaselineBuilder), c_uint32)
        F("alga_baseline_builder_add_window", c_int, POINTER(AlgaBaselineBuilder),
          POINTER(AlgaWindow))
        F("alga_baseline_builder_finish", c_int, POINTER(AlgaBaselineBuilder), c_int, c_uint32,
          POINTER(AlgaBaseline))
        F("alga_baseline_validate", c_int, POINTER(AlgaBaseline))
        F("ascon_hash256", None, c_char_p, c_size_t, POINTER(ctypes.c_uint8 * HASH_BYTES))
        F("ascon_hash256_init", None, POINTER(AsconHash256Ctx))
        F("ascon_hash256_update", None, POINTER(AsconHash256Ctx), c_char_p, c_size_t)
        F("ascon_hash256_final", None, POINTER(AsconHash256Ctx),
          POINTER(ctypes.c_uint8 * HASH_BYTES))
        F("audit_chain_link", None, POINTER(ctypes.c_uint8 * HASH_BYTES), c_uint64, c_char_p,
          c_size_t, POINTER(ctypes.c_uint8 * HASH_BYTES))

    def _check_abi(self) -> None:
        info = AlgaAbiInfo()
        self._lib.algaops_abi_info(ctypes.byref(info))
        expected = {
            "abi_version": ABI_VERSION,
            "size_window": ctypes.sizeof(AlgaWindow),
            "size_decision": ctypes.sizeof(AlgaDecision),
            "size_baseline": ctypes.sizeof(AlgaBaseline),
            "size_method_result": ctypes.sizeof(AlgaMethodResult),
            "off_window_temperature": AlgaWindow.temperature_c.offset,
            "off_window_band": AlgaWindow.band_rms_g.offset,
            "off_decision_outcome": AlgaDecision.outcome.offset,
            "off_decision_methods": AlgaDecision.methods.offset,
            "off_decision_alert_event": AlgaDecision.alert_event.offset,
            "off_decision_completeness": AlgaDecision.completeness.offset,
            "off_decision_explanation": AlgaDecision.explanation.offset,
            "off_decision_reason": AlgaDecision.reason.offset,
            "off_baseline_cov": AlgaBaseline.cov.offset,
        }
        bad = {k: (getattr(info, k), v) for k, v in expected.items() if getattr(info, k) != v}
        if bad:
            raise AbiMismatch(f"libalgaops ABI mismatch (library, binding): {bad}")

    # ---- raw access ----------------------------------------------------------
    @property
    def raw(self):
        return self._lib

    # ---- configuration -------------------------------------------------------
    def new_config(self) -> ctypes.Array:
        buf = ctypes.create_string_buffer(self.config_size)
        self._lib.algaops_engine_config_default(buf)
        return buf

    def config_set(self, cfg, key: str, value) -> None:
        if key in self.string_keys:
            if not isinstance(value, str):
                raise CoreError("algaops_config_set_string", -7, f"{key} expects a string")
            st = self._lib.algaops_config_set_string(cfg, key.encode(), value.encode())
        else:
            if isinstance(value, bool):
                value = 1.0 if value else 0.0
            if not isinstance(value, (int, float)):
                raise CoreError("algaops_config_set_number", -7, f"{key} expects a number")
            st = self._lib.algaops_config_set_number(cfg, key.encode(), float(value))
        if st != 0:
            raise CoreError("config_set", st, f"key {key!r} value {value!r}")

    def config_get(self, cfg, key: str):
        if key in self.string_keys:
            v = self._lib.algaops_config_get_string(cfg, key.encode())
            return v.decode() if v is not None else None
        out = c_double()
        st = self._lib.algaops_config_get_number(cfg, key.encode(), ctypes.byref(out))
        if st != 0:
            raise CoreError("config_get", st, key)
        v = out.value
        return int(v) if v.is_integer() else v

    def config_validate(self, cfg) -> None:
        err = ctypes.create_string_buffer(512)
        st = self._lib.algaops_config_validate(cfg, err, len(err))
        if st != 0:
            raise CoreError("algaops_config_validate", st, err.value.decode(errors="replace"))

    def config_effective(self, cfg) -> dict[str, object]:
        return {k: self.config_get(cfg, k) for k in self.config_keys}

    def config_load_file(self, cfg, path: str | os.PathLike[str]) -> None:
        err = ctypes.create_string_buffer(512)
        st = self._lib.algaops_config_load_file(cfg, os.fsencode(path), err, len(err))
        if st != 0:
            raise CoreError("algaops_config_load_file", st, err.value.decode(errors="replace"))

    # ---- hashing ---------------------------------------------------------------
    def ascon_hash256(self, data: bytes) -> bytes:
        out = (ctypes.c_uint8 * HASH_BYTES)()
        self._lib.ascon_hash256(data, len(data), ctypes.byref(out))
        return bytes(out)

    def ascon_hash256_new(self) -> AsconHash256:
        """Incremental Ascon-Hash256: update() in pieces, then digest()."""
        return AsconHash256(self._lib)

    def audit_link(self, prev: bytes, seq: int, payload: bytes) -> bytes:
        if len(prev) != HASH_BYTES:
            raise ValueError("prev hash must be 32 bytes")
        p = (ctypes.c_uint8 * HASH_BYTES).from_buffer_copy(prev)
        out = (ctypes.c_uint8 * HASH_BYTES)()
        self._lib.audit_chain_link(ctypes.byref(p), c_uint64(seq), payload, len(payload),
                                   ctypes.byref(out))
        return bytes(out)

    # ---- labels -----------------------------------------------------------------
    def flag_list(self, flags: int) -> list[str]:
        return [name for bit, name in self.flag_names.items() if flags & bit]


class AsconHash256:
    """ascon_hash256_init/update/final: the digest of the concatenated input, equal to
    the one-shot ascon_hash256, in constant memory.  final() wipes the C context, so
    the digest is kept and further update() calls are refused."""

    def __init__(self, lib: ctypes.CDLL) -> None:
        self._lib = lib
        self._ctx = AsconHash256Ctx()
        self._digest: bytes | None = None
        lib.ascon_hash256_init(ctypes.byref(self._ctx))

    def update(self, data: bytes | bytearray | memoryview) -> None:
        if self._digest is not None:
            raise ValueError("ascon_hash256: update() after digest()")
        if not isinstance(data, bytes):
            data = bytes(data)
        self._lib.ascon_hash256_update(ctypes.byref(self._ctx), data, len(data))

    def digest(self) -> bytes:
        if self._digest is None:
            out = (ctypes.c_uint8 * HASH_BYTES)()
            self._lib.ascon_hash256_final(ctypes.byref(self._ctx), ctypes.byref(out))
            self._digest = bytes(out)
        return self._digest

    def hexdigest(self) -> str:
        return self.digest().hex()


_LOCK = threading.Lock()
_LOADED: dict[str, CoreLibrary] = {}


def load_library(path: str | os.PathLike[str] | None = None) -> CoreLibrary:
    """Load (once per path) and ABI-check libalgaops."""
    resolved = find_library(path).resolve()
    key = str(resolved)
    with _LOCK:
        lib = _LOADED.get(key)
        if lib is None:
            lib = CoreLibrary(resolved)
            _LOADED[key] = lib
        return lib
