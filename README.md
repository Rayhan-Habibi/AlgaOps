# AlgaOps: Edge Trust-Gated Photobioreactor Auxiliary Assurance

<div align="center">

![AlgaOps Overview](AlgaOps_Overview.png)

**Sistem Edge Trust-Gated untuk Integritas Data dan Diagnosis Peralatan Bantu Fotobioreaktor Mikroalga**

[![Language: C11](https://img.shields.io/badge/Core-C11-blue.svg)](algaops-assurance/)
[![Gateway: Python 3.11+](https://img.shields.io/badge/Gateway-Python%203.11+-brightgreen.svg)](gateway/)
[![Dashboard: Vite + TS](https://img.shields.io/badge/Dashboard-Vite%20%2B%20TS-646CFF.svg)](dashboard/)
[![Firmware: ESP-IDF](https://img.shields.io/badge/Firmware-ESP--IDF-red.svg)](firmware/)
[![Audit: Ascon-Hash256](https://img.shields.io/badge/Audit-Ascon--Hash256-orange.svg)](gateway/algaops_gateway/audit/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

*Program Pendanaan Hackathon DeepTech 2026 — Universitas Padjadjaran & Kementerian Pendidikan Tinggi, Sains, dan Teknologi*  
*Mitra Riset: PT. Algatek Karbon Nusantara*

</div>

---

## 📌 Executive Summary

Dalam budidaya mikroalga menggunakan **Photobioreactor (PBR)**, kontinuitas operasional peralatan bantu—terutama pompa sirkulasi kultur dan blower aerasi—sangat kritikal. Kegagalan mekanis mendadak pada unit-unit ini memicu pengendapan biomassa, thermal shock, dan kematian kultur massal (*culture crash*) yang dapat menimbulkan kerugian finansial hingga puluhan juta rupiah.

Sistem monitoring prediktif IoT konvensional seringkali gagal di lingkungan PBR yang lembap, bersuhu fluktuatif, dan berinterferensi listrik tinggi karena **tidak mampu membedakan anomali sensor (data palsu, macet, atau noise) dengan kerusakan mekanis riil**. Kerusakan koneksi kabel atau sensor drift kerap disalahartikan sebagai kerusakan bearing mesin, memicu alarm palsu yang merusak kepercayaan operator.

**AlgaOps Assurance** menghadirkan paradigma **Trust-Gated Diagnostics**:
1. **TrustGuard (Feature A):** Melakukan verifikasi integritas data sensor secara real-time pada edge (validasi packet loss, deduplikasi urutan, deteksi non-monotonic timestamp, sensor macet/*stuck*, drift, dan nilai di luar batas fisis).
2. **Trust-Gated Equipment Health (Feature B):** Mengevaluasi kesehatan peralatan bantu mekanik berbasis deviasi statistik (Z-score dan regularized Mahalanobis distance) terhadap *baseline* operasi normal multi-mode (`OFF`, `STARTUP`, `STEADY`).
3. **The Trust Gate Invariant:** Jika integritas data diragukan (`is_trusted == false`), evaluasi kesehatan mesin **wajib diblokir (suppressed)** dan sistem mengeluarkan peringatan `CHECK_SENSOR`. Diagnosis `CHECK_EQUIPMENT` hanya diperbolehkan jika data sensor terbukti 100% valid dan terpercaya.
4. **Tamper-Evident Evidence Chain:** Setiap jendela telemetri, keputusan diagnostik, dan transisi status dicatat dalam hash chain kriptografis berbasis standar lightweight cryptography **Ascon-Hash256** untuk auditabilitas forensik.

---

## 🎨 System Visualization & 3D Hardware Models

<div align="center">
<table>
  <tr>
    <td align="center"><b>Auxiliary Pump Rig & Sensor Pod</b></td>
    <td align="center"><b>Edge Assurance Gateway Unit</b></td>
  </tr>
  <tr>
    <td><img src="AlgaOps_PumpRig_CloseUp.png" width="450" alt="Pump Rig CloseUp"/></td>
    <td><img src="AlgaOps_Gateway_CloseUp.png" width="450" alt="Gateway CloseUp"/></td>
  </tr>
  <tr>
    <td><i>Sensor pod pemantau getaran 3-sumbu (RMS & kurtosis) dan suhu permukaan pada pompa sirkulasi PBR</i></td>
    <td><i>Unit Edge Gateway offline-first dengan Ascon-Hash256 audit log dan dashboard lokal</i></td>
  </tr>
</table>
</div>

Model 3D lengkap untuk dudukan pompa, enclosure sensor pod, dan casing edge gateway tersedia dalam file Blender [**`AlgaOps_Assurance_3D.blend`**](AlgaOps_Assurance_3D.blend).

---

## 🏗️ End-to-End System Architecture

AlgaOps Assurance dibangun dengan arsitektur modular yang memisahkan akusisi sensor, engine komputasi inti, gateway orkestrator, dan visualisasi:

```
┌─────────────────────────────────┐
│   Auxiliary Pump / Blower Rig   │
│   (ADXL345 Vib + DS18B20 Temp)  │
└────────────────┬────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────────────┐
│  ESP32 Sensor Node (firmware/esp32-sensor-node/)       │
│  - 800 Hz vibration sampling -> RMS & kurtosis window  │
│  - MQTT Client (QoS 1, persistent session, back-off)   │
└────────────────┬───────────────────────────────────────┘
                 │ MQTT (algaops/telemetry/v1/{node_id})
                 ▼
┌────────────────────────────────────────────────────────┐
│  Edge Gateway Service (gateway/)                       │
│  - Mosquitto Broker + Strict JSON Schema Ingest        │
│  - Single-writer lock + SQLite WAL storage             │
│  - Ascon-Hash256 tamper-evident audit chain            │
│  - FastAPI REST / Server-Sent Events (SSE) Bus         │
└────────────────┬───────────────────────────────────────┘
                 │ ctypes ABI binding (zero-copy)
                 ▼
┌────────────────────────────────────────────────────────┐
│  C11 Assurance Core Engine (algaops-assurance/)        │
│  - Feature A: TrustGuard v2 (integrity verification)   │
│  - Feature B: Equipment Health (multi-mode baselines)  │
│  - The Trust Gate Invariant (strict alert suppression) │
└────────────────┬───────────────────────────────────────┘
                 │ SSE Realtime Stream / REST API
                 ▼
┌────────────────────────────────────────────────────────┐
│  Operator Dashboard (dashboard/)                       │
│  - Vite + TypeScript responsive web monitoring UI      │
│  - Live gauges, vibration spectrum, & alert management │
│  - Offline scenario replay & evidence package export   │
└────────────────────────────────────────────────────────┘
```

### Primary Diagnostic Outcomes

| Status | Makna Operasional | Tindakan Operator |
| :--- | :--- | :--- |
| `NORMAL` | Data sensor valid dan vibrasi/suhu berada dalam batas baseline normal. | Tidak ada tindakan (kondisi aman). |
| `CHECK_SENSOR` | Integritas data terganggu (packet loss, freeze, noise fisis, timeout). Evaluasi mesin diblokir. | Periksa kabel sensor, mounting accelerometer, atau jaringan node. |
| `CHECK_EQUIPMENT` | Data terpercaya, tetapi vibrasi atau suhu melonjak signifikan ($Z > 3.0$). | Jadwalkan inspeksi mekanis/bearing pompa sebelum terjadi kegagalan fatal. |
| `NOT_EVALUATED` | Mode transisi (`OFF` atau awal `STARTUP`) di mana baseline belum berlaku penuh. | Tunggu peralatan mencapai status steady-state. |

---

## 📂 Repository Layout

```text
AlgaOps/
├── README.md                          # Dokumentasi utama repositori terpadu
├── AlgaOps_Proposal.md                # Naskah lengkap proposal Hackathon DeepTech 2026
├── AlgaOps_Assurance_3D.blend         # Model 3D rig PBR, sensor pod, dan gateway (Blender)
├── AlgaOps_Overview.png               # Render visualisasi arsitektur sistem
├── AlgaOps_PumpRig_CloseUp.png        # Render detail rig pompa dan sensor pod
├── AlgaOps_Gateway_CloseUp.png        # Render detail edge gateway unit
│
├── algaops-assurance/                 # ENGINE KOMPUTASI INTI (C11 Core Library & CLI)
│   ├── CMakeLists.txt                 # Build system CMake (C11 murni, zero-dependency)
│   ├── include/                       # Public API C11 & shared engine header
│   │   ├── algaops_engine.h           # API engine v2 untuk integrasi gateway ctypes
│   │   ├── algaops_types.h            # Definisi struktur data, flags integritas, threshold
│   │   ├── trustguard.h               # API Feature A: TrustGuard
│   │   └── equipment_health.h         # API Feature B: Equipment Health
│   ├── src/                           # Implementasi C11 (TrustGuard, Z-score, Mahalanobis)
│   ├── tests/                         # Automated unit & integration tests C11
│   └── data/                          # Dataset sintetis deterministik & file baseline
│
├── gateway/                           # EDGE SERVICE (Python 3.11+ / FastAPI / SQLite)
│   ├── algaops_gateway/               # Pipeline ingest, audit Ascon, storage, REST/SSE API
│   ├── config/                        # Template konfigurasi gateway & default policy TOML
│   └── tests/                         # Unit, integration, security, & ABI contract tests
│
├── dashboard/                         # OPERATOR WEB DASHBOARD (Vite + TypeScript)
│   ├── src/                           # Komponen UI interaktif (live metrics, charts, SSE)
│   └── tests/                         # Automated vitest & Playwright e2e test suite
│
├── firmware/                          # EMBEDDED SENSOR NODE (ESP32 ESP-IDF)
│   └── esp32-sensor-node/             # Akuisisi sensor ADXL345, DS18B20, & publisher MQTT
│       ├── host_test/                 # Unit test firmware yang dapat dijalankan pada PC host
│       └── main/                      # FreeRTOS tasks, edge feature extraction, MQTT client
│
├── hardware/                          # HARDWARE SPECIFICATIONS
│   ├── bom.csv                        # Bill of Materials lengkap dengan estimasi biaya komponen
│   └── esp32-pinout.csv               # Tabel mapping pinout sensor, ADC, dan bus I2C/SPI
│
├── simulator/                         # DETERMINISTIC SENSOR SIMULATOR
│   ├── algaops_sim/                   # Generator sinyal fisik, injeksi fault, & publisher MQTT
│   ├── scenarios/                     # Definisi skenario uji (stuck sensor, packet loss, spike)
│   └── fixtures/                      # Rekaman telemetri deterministik (JSONL)
│
├── schemas/                           # STRICT DATA CONTRACTS (JSON Schema)
│   ├── telemetry_window.v1.schema.json# Skema kontrak payload telemetri ESP32
│   ├── diagnostic_event.v1.schema.json# Skema rekaman keputusan diagnostik edge
│   └── audit_event.v1.schema.json     # Skema log audit rantai Ascon-Hash256
│
├── scripts/                           # AUTOMATION SCRIPTS (Cross-platform)
│   ├── run_demo.ps1 / .sh             # Script one-click demonstrasi live vertikal
│   ├── build_all.ps1 / .sh            # Kompilasi C11 core, gateway, dashboard, & firmware
│   └── test_all.ps1 / .sh             # Eksekusi seluruh rangkaian pengujian sistem
│
├── tests/e2e/                         # LIVE END-TO-END SYSTEM TESTS
│   ├── test_demo_end_to_end.py        # Pengujian integrasi penuh dari ingest hingga API
│   └── test_trust_gate_invariant.py   # Verifikasi kepatuhan Trust Gate Invariant
│
├── tools/                             # INDEPENDENT VERIFICATION TOOLS
│   └── verify_evidence.py             # CLI independen pemeriksa integritas audit Ascon
│
└── docs/                              # DETAILED ENGINEERING DOCUMENTATION
    ├── quickstart.md                  # Panduan cepat setup dan demonstrasi lokal
    ├── firmware-esp32.md              # Dokumentasi teknis flashing dan tuning ESP32
    ├── deployment-raspberry-pi.md     # Panduan instalasi gateway pada Raspberry Pi OS
    ├── evidence-verification.md       # Spesifikasi rantai audit kriptografi Ascon
    └── mqtt-contract.md               # Protokol komunikasi MQTT dan format topik
```

---

## ⚡ Quick Start: Running the Live System

### 1. One-Click Demonstration (Simulasi Lengkap)

Demonstrasi lokal menjalankan broker MQTT embedded, gateway pipeline dengan C11 engine, simulator 3 pompa PBR (pompa normal, pompa sensor macet, pompa degradasi bearing), serta dashboard web interaktif:

**Windows (PowerShell):**
```powershell
# Build seluruh modul (C core, dashboard dist, dependensi Python)
.\scripts\build_all.ps1

# Jalankan live demo vertikal (membuka dashboard pada http://127.0.0.1:8080)
.\scripts\run_demo.ps1
```

**Linux / Raspberry Pi OS:**
```bash
# Build seluruh modul
./scripts/build_all.sh

# Jalankan live demo vertikal
./scripts/run_demo.sh
```

### 2. Menjalankan Komputasi C11 Inti Secara Mandiri

Modul C11 dapat dikompilasi secara independen tanpa dependensi eksternal:

```bash
cd algaops-assurance
mkdir build && cd build
cmake ..
cmake --build .

# Menjalankan 24 unit & integration test inti C11
ctest --output-on-failure

# Evaluasi skenario anomali mekanik terhadap baseline normal
./algaops --baseline ../data/baseline_normal.csv \
          --input ../data/scenario_equipment_anomaly.csv \
          --output ../results.csv
```

### 3. Verifikasi Audit Chain Kriptografis (Ascon-Hash256)

Paket bukti diagnostik (*evidence bundle*) yang diekspor oleh gateway dapat diverifikasi secara independen untuk memastikan log audit belum dimanipulasi:

```bash
python -I tools/verify_evidence.py out/evidence_bundle/
```

---

## 🔒 Rekayasa Integritas & Keamanan Sistem

1. **Non-Invasive & Read-Only:** Gateway dan sensor pod beroperasi secara strictly read-only terhadap jaringan kontrol PBR. Sistem tidak memiliki modul aktuator relay, perintah kendali kecepatan pompa, atau shutdown otomatis. Sistem hanya bertindak sebagai *decision support* bagi teknisi.
2. **Single Decision Authority:** Seluruh kalkulasi keputusan diagnostik dipusatkan pada engine C11 (`algaops-assurance/`) yang dipanggil oleh gateway melalui ctypes dengan verifikasi ABI offset struct yang ketat. Logika keputusan tidak pernah diduplikasi di layer Python maupun TypeScript.
3. **Single Writer Database Lock:** Database SQLite WAL pada gateway dilindungi oleh mekanisme mutual-exclusion OS lockfile (`<database>.writer.lock`), menjamin integritas rantai audit kriptografis dari konkurensi ganda.
4. **Data Sintetis & Transparansi Pengujian:** Seluruh dataset demonstrasi pada repositori ini dilabeli secara eksplisit sebagai data sintetis deterministik untuk validasi rekayasa sebelum pengujian lapangan industri.

---

## 👥 Tim Pengusul (Universitas Andalas)

- **Ahmad Saidi** (Ketua Tim Pelaksana)
- **Rayhan Habibi** (Software & Embedded Systems Engineering)
- **Yoan Purbolingga** (Dosen Pendamping)
- **Filjisalim C. Ramadhana**
- **Jenius Anugrah Ridwan**

---

## 📄 Lisensi

Proyek ini didistribusikan di bawah lisensi open-source [MIT License](LICENSE).
