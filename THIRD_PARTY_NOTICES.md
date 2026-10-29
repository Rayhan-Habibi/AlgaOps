# Third-party notices

## Code included in this repository

| Component | Where | Origin | License |
|---|---|---|---|
| Ascon reference implementation (Ascon-Hash256, NIST SP 800-232) | `algaops-assurance/src/ascon_hash256.c` (permutation and hash core; incremental wrapper added) | [ascon/ascon-c](https://github.com/ascon/ascon-c), `crypto_hash/asconhash256/ref` | CC0 1.0 |
| Ascon-Hash256 known-answer vectors (subset) | `algaops-assurance/tests/vectors/ascon_hash256_kat.txt` | ascon-c `LWC_HASH_KAT_128_256.txt` | CC0 1.0 |
| pyascon (hash variant only) | `tools/verify_evidence.py` (reference functions; unrolled permutation added and cross-checked) | [meichlseder/pyascon](https://github.com/meichlseder/pyascon) | CC0 1.0 |

The AlgaOps v1 C11 code under `algaops-assurance/` comes from
[Rayhan-Habibi/AlgaOps](https://github.com/Rayhan-Habibi/AlgaOps) (MIT).

## Dependencies (not vendored)

Gateway runtime (`gateway/requirements.lock`): paho-mqtt (EPL-2.0 OR BSD-3-Clause),
FastAPI (MIT), Starlette (BSD-3-Clause), uvicorn (BSD-3-Clause), pydantic and
pydantic-core (MIT), anyio (MIT), h11 (MIT), click (BSD-3-Clause), idna
(BSD-3-Clause), typing-extensions (PSF-2.0), annotated-types (MIT),
typing-inspection (MIT), annotated-doc (MIT), opentelemetry-api (Apache-2.0).

Development and tests: pytest (MIT), httpx and httpcore (BSD-3-Clause), jsonschema
(MIT), amqtt (MIT, development broker only), PyYAML (MIT), pwdlib (MIT),
argon2-cffi (MIT), websockets (BSD-3-Clause), rich (MIT), typer (MIT), certifi
(MPL-2.0) and their dependencies listed in the lock file; build helpers cmake
(Apache-2.0), ninja (Apache-2.0) and ziglang (MIT).

Dashboard development (no third-party code is bundled into `dashboard/dist`
besides the Vite runtime): Vite (MIT), Vitest (MIT), TypeScript (Apache-2.0),
jsdom (MIT), Playwright (Apache-2.0).

Firmware: ESP-IDF (Apache-2.0) and the ESP Component Registry package
`espressif/onewire_bus` (Apache-2.0), fetched by the IDF component manager at
build time.

Deployment: Eclipse Mosquitto (EPL-2.0 OR EDL-1.0) and chrony (GPL-2.0) are
installed from the operating system packages, not redistributed here.
