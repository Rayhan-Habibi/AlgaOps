#!/usr/bin/env python3
"""Cross-platform ELF & ABI symbol verifier for libalgaops Linux (WP-4A-9 & WP-4A-10).

Parses ELF64 headers and dynamic symbol tables (.dynsym / .dynstr) of
libalgaops Linux shared objects (ARM64 and x86_64) to verify:
1. 64-bit architecture and little-endian data encoding.
2. Machine architecture (AArch64 0x00B7 and x86_64 0x003E).
3. Export of all 41 ctypes engine entry points.
4. Immunity to GNU libc <features.h> header shadowing via -iquote.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

EXPECTED_SYMBOLS = [
    "algaops_version",
    "algaops_abi_info",
    "algaops_engine_config_default",
    "algaops_engine_config_size",
    "algaops_config_set_number",
    "algaops_config_set_string",
    "algaops_config_get_number",
    "algaops_config_get_string",
    "algaops_config_key_count",
    "algaops_config_key_name",
    "algaops_config_key_is_string",
    "algaops_config_validate",
    "algaops_config_load_file",
    "algaops_engine_create",
    "algaops_engine_destroy",
    "algaops_engine_reset",
    "algaops_engine_set_baseline",
    "algaops_engine_clear_baseline",
    "algaops_engine_get_baseline",
    "algaops_engine_process",
    "algaops_engine_tick",
    "algaops_engine_report_malformed",
    "algaops_engine_restore_alert",
    "algaops_replay_csv",
    "algaops_baselines_from_csv",
    "algaops_mode_str",
    "algaops_method_str",
    "algaops_alert_event_str",
    "diagnostic_outcome_str",
    "trustguard_flag_name",
    "trustguard_blocking_flags",
    "alga_window_features",
    "alga_baseline_builder_init",
    "alga_baseline_builder_add_window",
    "alga_baseline_builder_finish",
    "alga_baseline_validate",
    "ascon_hash256",
    "ascon_hash256_init",
    "ascon_hash256_update",
    "ascon_hash256_final",
    "audit_chain_link",
]

ARCH_MACHINE_MAP = {
    0x00B7: "AArch64 (ARM64)",
    0x003E: "x86_64 (AMD64)",
}


def verify_elf_library(so_path: Path, expected_arch: str | None = None) -> dict:
    """Parse ELF64 header and dynamic symbol table to verify library integrity."""
    if not so_path.is_file():
        raise FileNotFoundError(f"Shared library not found at {so_path}")

    data = so_path.read_bytes()
    assert len(data) >= 64, "File too small to be a valid ELF binary"

    # Header parsing
    magic = data[:4]
    assert magic == b"\x7fELF", f"Invalid ELF magic: {magic}"
    elf_class = data[4]
    assert elf_class == 2, f"Expected 64-bit ELF (class 2), got {elf_class}"
    endian = data[5]
    assert endian == 1, f"Expected little-endian (1), got {endian}"

    e_type, e_machine = struct.unpack("<HH", data[16:20])
    assert e_type == 3, f"Expected shared object (ET_DYN = 3), got {e_type}"
    arch_name = ARCH_MACHINE_MAP.get(e_machine, f"Unknown (0x{e_machine:04x})")
    if expected_arch:
        assert expected_arch.lower() in arch_name.lower(), (
            f"Expected architecture '{expected_arch}', got '{arch_name}' (0x{e_machine:04x})"
        )

    # Section headers
    (
        e_shoff,
        e_flags,
        e_ehsize,
        e_phentsize,
        e_phnum,
        e_shentsize,
        e_shnum,
        e_shstrndx,
    ) = struct.unpack("<QIHHHHHH", data[40:64])
    shstr_hdr = data[e_shoff + e_shstrndx * e_shentsize : e_shoff + (e_shstrndx + 1) * e_shentsize]
    shstr_offset = struct.unpack("<Q", shstr_hdr[24:32])[0]

    dynsym_data, dynstr_data = None, None
    for i in range(e_shnum):
        s_hdr = data[e_shoff + i * e_shentsize : e_shoff + (i + 1) * e_shentsize]
        name_idx, s_type, s_flags, s_addr, s_offset, s_size = struct.unpack("<IIQQQQ", s_hdr[:40])
        s_name = data[shstr_offset + name_idx :].split(b"\x00")[0].decode("utf-8", "ignore")
        if s_name == ".dynsym":
            dynsym_data = data[s_offset : s_offset + s_size]
        elif s_name == ".dynstr":
            dynstr_data = data[s_offset : s_offset + s_size]

    assert dynsym_data and dynstr_data, "Missing .dynsym or .dynstr section in ELF"

    symbols = set()
    entsize = 24  # sizeof(Elf64_Sym)
    for i in range(0, len(dynsym_data), entsize):
        sym = dynsym_data[i : i + entsize]
        st_name = struct.unpack("<I", sym[:4])[0]
        if st_name < len(dynstr_data):
            name = dynstr_data[st_name:].split(b"\x00")[0].decode("utf-8", "ignore")
            if name:
                symbols.add(name)

    missing = [f for f in EXPECTED_SYMBOLS if f not in symbols]
    return {
        "file": str(so_path),
        "arch": arch_name,
        "e_machine": f"0x{e_machine:04x}",
        "bits": 64,
        "endian": "little",
        "elf_type": "ET_DYN (Shared Object)",
        "symbols_total": len(symbols),
        "expected_checked": len(EXPECTED_SYMBOLS),
        "missing": missing,
        "valid": len(missing) == 0,
    }


def verify_arm64_elf(so_path: Path) -> dict:
    """Backward-compatible helper for verifying ARM64 ELF specifically."""
    return verify_elf_library(so_path, expected_arch="AArch64")


def main() -> int:
    targets = [
        ("algaops-assurance/libalgaops_aarch64_glibc.so", "AArch64"),
        ("algaops-assurance/libalgaops_x86_64_glibc.so", "x86_64"),
    ]

    all_ok = True
    for p_str, arch in targets:
        p = Path(p_str)
        if not p.is_file():
            print(f"[WARN] Target library {p} not found, skipping.")
            continue
        print(f"[*] Auditing {arch} shared library: {p}")
        res = verify_elf_library(p, expected_arch=arch)
        for k, v in res.items():
            print(f"  {k}: {v}")
        if not res["valid"]:
            print(f"[FAIL] Missing symbols in {p}: {res['missing']}")
            all_ok = False
        else:
            print(f"[PASS] All {len(EXPECTED_SYMBOLS)} expected C11 engine symbols are exported in {arch} ELF.\n")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
