"""Tests for build-system hardening, header-shadowing immunity, and ELF ABI (WP-4A-10)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT))

from tools.verify_arm64_abi import EXPECTED_SYMBOLS, verify_elf_library


def test_cmakelists_header_shadowing_hardening():
    """Verify CMakeLists.txt configures -iquote to prevent features.h from shadowing GNU libc."""
    cmakelists = REPO_ROOT / "algaops-assurance" / "CMakeLists.txt"
    assert cmakelists.is_file(), "CMakeLists.txt must exist"
    content = cmakelists.read_text(encoding="utf-8")

    assert "-iquote" in content, "CMakeLists.txt must configure -iquote for non-MSVC compilers"
    assert "-D_POSIX_C_SOURCE=200809L" in content, "POSIX feature macros must be declared"
    assert "-D_DEFAULT_SOURCE" in content, "Default source macro must be declared"


def test_aarch64_elf_abi_exported_symbols():
    """Verify that libalgaops_aarch64_glibc.so is a valid AArch64 ELF64 with all 41 engine symbols."""
    so_path = REPO_ROOT / "algaops-assurance" / "libalgaops_aarch64_glibc.so"
    if not so_path.is_file():
        pytest.skip("libalgaops_aarch64_glibc.so not generated yet")

    report = verify_elf_library(so_path, expected_arch="AArch64")
    assert report["valid"] is True
    assert report["e_machine"] == "0x00b7"
    assert report["bits"] == 64
    assert len(report["missing"]) == 0
    assert report["expected_checked"] == len(EXPECTED_SYMBOLS)


def test_x86_64_elf_abi_exported_symbols():
    """Verify that libalgaops_x86_64_glibc.so is a valid x86_64 ELF64 with all 41 engine symbols."""
    so_path = REPO_ROOT / "algaops-assurance" / "libalgaops_x86_64_glibc.so"
    if not so_path.is_file():
        pytest.skip("libalgaops_x86_64_glibc.so not generated yet")

    report = verify_elf_library(so_path, expected_arch="x86_64")
    assert report["valid"] is True
    assert report["e_machine"] == "0x003e"
    assert report["bits"] == 64
    assert len(report["missing"]) == 0
    assert report["expected_checked"] == len(EXPECTED_SYMBOLS)


def test_deployment_script_hardening():
    """Verify raspberry pi deployment scripts use strict error modes and proper paths."""
    install_sh = REPO_ROOT / "deploy" / "raspberry-pi" / "install.sh"
    assert install_sh.is_file()
    content = install_sh.read_text(encoding="utf-8")

    assert "set -euo pipefail" in content
    assert "cmake -S \"$REPO/algaops-assurance\"" in content
    assert "/opt/algaops/lib/libalgaops.so" in content

