# PROPOSAL

## PESERTA PROGRAM PENDANAAN HACKATHON DEEPTECH

**Judul Inovasi**  
**AlgaOps Assurance**  
Sistem Edge Trust-Gated untuk Integritas Data dan Diagnosis Peralatan Bantu Fotobioreaktor Mikroalga

**Penyelenggara Kegiatan:** Universitas Padjadjaran

Direktorat Jenderal Riset dan Pengembangan  
Kementerian Pendidikan Tinggi, Sains, dan Teknologi

**TAHUN 2026**

> Catatan visual: sampul dokumen asli memuat logo Kemendikbudristek dan LPDP sebagai gambar.

---

# LEMBAR IDENTITAS

1. **Judul:** AlgaOps Assurance: Sistem Edge Trust-Gated untuk Integritas Data dan Diagnosis Peralatan Bantu Fotobioreaktor Mikroalga

2. **Ketua Tim Pelaksana**
   - a. Nama Lengkap: Ahmad Saidi
   - b. Jenis Kelamin: L
   - c. NIM: 2411512030
   - d. Jabatan Struktural: -
   - e. Jabatan Fungsional: -
   - f. Institusi: Universitas Andalas
   - g. Alamat: Jl. Lingkar Universitas Andalas, Limau Manis, Kec. Pauh, Kota Padang, Sumatera Barat 25175
   - h. Telpon/Faks: 081218617973
   - i. Alamat Rumah: Griya Cendekia Blok E6.1 Gunung Sindur Kab.Bogor
   - j. Telpon/Faks/E-mail: ahmadsaidi1045@gmail.com

3. **Lembaga Mitra Riset (*):** PT. Algatek Karbon Nusantara  
   **Alamat (*):** Kawasan Industri Jababeka, Jl. Jababeka IV Blok V Kav. 74-75, Kel. Desa. Pasirgombong, Kec. Cikarang Utara, Kab. Bekasi, Jawa Barat, Indonesia 17530

4. **Anggota Tim Pelaksana**

| No. | Nama | NIK/NIP | Institusi |
|---:|---|---|---|
| 1. | Yoan Purbolingga | 199303272024061002 | Universitas Andalas |
| 2. | Rayhan Habibi | 1371070310050005 | Universitas Andalas |
| 3. | Filjisalim C. Ramadhana | 1206012709050001 | Universitas Andalas |
| 4. | Jenius Anugrah Ridwan | 1377022605060003 | Universitas Andalas |

**Bandung, 25 Oktober 2026**  
Ketua Tim Pelaksana,

> [Tanda tangan Ketua Tim Pelaksana tercantum sebagai gambar dalam dokumen asli.]

Nama: Ahmad Saidi  
NIM: 2311512005

---

# SUBSTANSI PROPOSAL PROGRAM HILIRISASI RISET STRATEGIS
## BANTUAN PENDANAAN PENYELENGGARAAN HACKATHON DEEPTECH

## 1. Judul Proposal

AlgaOps Assurance: Sistem Edge Trust-Gated untuk Integritas Data dan Diagnosis Peralatan Bantu Fotobioreaktor Mikroalga

## 2. Tema Fokus Inovasi

**Bidang Strategis:**

- ☐ Digitalisasi; AI dan Semikonduktor
- ☐ Energi
- ☑ Hilirisasi dan Industrialisasi
- ☐ Kesehatan
- ☐ Pangan

## 5. Executive Summary (maks. 250 kata)

Fotobioreaktor (PBR) mikroalga bergantung pada pompa/blower untuk sirkulasi dan aerasi, sementara keputusan operasi bergantung pada data sensor. Sensor yang macet atau paket yang hilang membuat data tampak normal padahal menyesatkan; penurunan kondisi pompa baru terlihat dari pola getaran, temperatur, dan arus. Sistem pemantauan PBR yang ada, termasuk milik Algatek [1][2], berfokus pada parameter proses, sedangkan monitor pompa komersial berfokus pada ambang getaran [6]–[8].

AlgaOps Assurance adalah modul edge retrofit dan read-only untuk satu pompa/blower pendukung PBR. Pembedanya adalah Trust-Gated Diagnostics: peringatan kondisi peralatan hanya naik setelah data lolos uji kepercayaan, sehingga sistem membedakan “sensor bermasalah” dari “pompa bermasalah”. Tiga layer berjalan lokal: TrustGuard (data hilang, terlambat, duplikat, macet, tidak konsisten antarsensor, dan self-test sensor); Equipment Health (baseline per kondisi operasi, fitur spektral, deteksi anomali ringan); serta log tamper-evident berbasis hash chain. ESP32 dengan ADXL345 dan DS18B20 mengirim ringkasan lewat MQTT lokal ke Raspberry Pi 4 yang bekerja offline-first.

Studi ITB pada pilot Muara Karang mencatat efisiensi serapan CO₂ rata-rata 77,27% dengan listrik budidaya sebagai kontributor dampak terbesar [3], dan studi model bisnis ITB menyoroti kebutuhan data kinerja yang valid dan transparan [4]. Angka tersebut adalah konteks, bukan hasil AlgaOps.

Estimasi TKT 2; target TKT 3 setelah proof-of-concept laboratorium terdokumentasi. Anggaran MVP indikatif Rp4.754.000. Validasi memakai rig pompa pada kondisi normal dan penyimpangan terkontrol terhadap instrumen referensi, dengan KPI kelengkapan data, latensi peringatan, alarm palsu, dan ketepatan atribusi sensor-vs-pompa. Algatek berstatus calon mitra; akses belum dikonfirmasi.

## 6. Deskripsi Inovasi (Maksimum 2 halaman)

### 1. Sub-tema dan Problem Statement

**Masalah.** PBR mikroalga untuk penangkapan karbon membutuhkan pompa/blower untuk sirkulasi dan aerasi; aliran udara serta posisi inlet memengaruhi pencampuran dan produktivitas alga [11]. Teknisi mengandalkan data sensor yang dapat macet, terlambat, atau hilang tanpa terdeteksi. Sistem pemantauan PBR, termasuk Smart Monitoring System Algatek (cahaya, CO₂, pH) [1][2], berfokus pada parameter proses; monitor komersial berfokus pada getaran dan temperatur dengan alarm ambang [6]–[8]. Dalam penelusuran tim, belum ditemukan modul yang menggabungkan keduanya dengan integritas data sebagai syarat alarm.

**Konteks data.** Studi ITB pada pilot Muara Karang mencatat efisiensi serapan CO₂ rata-rata 77,27% dan GWP 8,49 kg CO₂-eq/kg CO₂ tertangkap (cradle-to-gate); listrik budidaya menjadi kontributor dampak terbesar [3]. Studi model bisnis ITB menekankan kebutuhan data kinerja valid dan transparan [4]. Angka berlaku pada studi tersebut, bukan hasil AlgaOps.

**Pertanyaan yang diuji.** Dapatkah skor kepercayaan data mengunci alarm agar teknisi membedakan gangguan sensor dari penurunan kondisi pompa/blower, dengan alarm palsu lebih rendah daripada ambang tetap tanpa mengganggu sistem? Ini hipotesis, bukan klaim tentang kegagalan Algatek. Pengguna sasaran: teknisi operasi/pemeliharaan PBR; Algatek calon pengguna validasi, tetapi akses belum dikonfirmasi.

### 2. Solusi Awal, Arsitektur, dan Pembeda

AlgaOps Assurance memakai prinsip Trust-Gated Diagnostics: peringatan kondisi peralatan hanya dinaikkan jika data lolos pemeriksaan kepercayaan. Jika tidak, sistem mengeluarkan “periksa sensor”, bukan “periksa pompa”.

**Layer 1 — TrustGuard.** Menandai paket hilang, terlambat/duplikat (sequence ID dan timestamp), nilai macet, spike, serta nilai di luar rentang. Sistem memeriksa konsistensi sensor—misalnya perubahan RMS getaran saat arus menunjukkan pompa menyala dan tren temperatur yang wajar—serta menjalankan self-test ADXL345 terjadwal. Hasilnya TrustScore per jendela data beserta alasan yang dapat dipahami teknisi. Rujukan stuck-at/spike: [10]; kebutuhan konteks/redundansi sensor: [9].

**Layer 2 — Equipment Health.** Membandingkan getaran, tren temperatur, dan arus opsional dengan baseline pompa/blower pada kondisi OFF, start-up, dan steady untuk menekan alarm palsu saat start/stop. Fitur mencakup RMS, peak-to-peak, dan energi pita di sekitar frekuensi putar/harmonik. Studi ablasi membandingkan ambang tetap, z-score baseline, dan model one-class (mis. Mahalanobis atau Isolation Forest). Keluaran hanya “perlu diperiksa”, bukan diagnosis pasti.

**Layer 3 — Jejak data tamper-evident.** Batch data dan peringatan disimpan di SQLite dengan rantai hash ASCON untuk mendeteksi perubahan dan diekspor sebagai paket bukti. Teknisi menandai alarm “benar/palsu” untuk kalibrasi ambang dan penyusunan dataset uji.

**Performance & Energy Assurance (opsional).** Indikator energi/CO₂ hanya dihitung jika data aliran, CO₂ masuk/keluar, dan energi tersedia, valid, serta diizinkan. Penghematan tidak diklaim tanpa data pembanding.

**Arsitektur.** ADXL345 (getaran), DS18B20 (temperatur permukaan), dan CT terisolasi opsional (arus) terhubung ke ESP32 untuk sampling berjendela, timestamp, sequence ID, dan buffering. Ringkasan melalui Wi-Fi lokal/MQTT dikirim ke Raspberry Pi 4 untuk TrustGuard, ekstraksi fitur, baseline, SQLite, dan dashboard [14]. Data PBR diakses read-only melalui CSV/API jika diizinkan.

**Pembeda.** Pemantauan IoT PBR [2][12], monitor pompa komersial [6]–[8], dan riset getaran pompa [5] telah ada; AlgaOps tidak mengklaim sebagai yang pertama. Kebaruan yang diuji ialah kombinasi (i) integritas data sebagai syarat alarm, (ii) diagnosis peralatan berkonteks PBR, dan (iii) jejak tamper-evident dalam modul retrofit offline-first. Perbandingan fitur ada di Tabel A2. ADXL345 dipakai untuk PoC (bandwidth sekitar 1,6 kHz pada ODR 3200 Hz [15]), bukan diagnosis bantalan tersertifikasi.

### 3. TKT, Target Kinerja, dan Validasi

Estimasi awal TKT 2. Target TKT 3 diajukan setelah proof of concept laboratorium terdokumentasi, sesuai instrumen TKT yang berlaku [17].

**Target rekayasa awal** (target, bukan hasil; ditinjau ulang setelah baseline):

- **TrustGuard:** mendeteksi ≥95% kejadian data hilang/nilai macet yang diinjeksi dalam skenario terdefinisi; menampilkan paket tervalidasi ≤2 detik setelah diterima Raspberry Pi; mengukur false positive per 8 jam pemutaran ulang data normal.
- **Atribusi sensor-vs-pompa:** pada injeksi gangguan sensor dan kondisi pompa terkontrol, dilaporkan proporsi atribusi yang benar; target ditetapkan setelah baseline.
- **Equipment Health:** alarm palsu, latensi, dan konsistensi antarpercobaan dibandingkan untuk ambang tetap, baseline, dan one-class.
- **Ketahanan:** kelengkapan data setelah koneksi diputus lalu dipulihkan, dan keutuhan rantai hash.

**Validasi:** pemeriksaan sensor terhadap instrumen pembanding, pengulangan kondisi normal, injeksi gangguan data, dan variasi operasi aman yang disetujui pembimbing. Tidak ada klaim penghematan energi atau tangkapan CO₂ tanpa data pembanding yang sah.

### 5. Keamanan dan Hilirisasi

**Keamanan.** Pemrosesan dan dashboard berjalan di jaringan lokal dengan akses terbatas. Paket hilang dicatat dan tidak dianggap valid. Arus diukur secara non-invasif dengan isolasi yang sesuai. AlgaOps memberi peringatan untuk diperiksa teknisi; ia bukan diagnosis pasti, sistem keselamatan, atau kendali otomatis PBR.

**Hilirisasi dan nilai bagi pengguna** (1) Tetap berfungsi saat jaringan terbatas: PBR Algatek ditargetkan juga untuk lokasi terpencil [2], sedangkan salah satu monitor komersial yang ditelaah mengirim data ke layanan cloud [6]. (2) Peringatan yang membedakan sensor bermasalah dari pompa bermasalah, sehingga teknisi tidak memeriksa aset yang keliru. (3) Retrofit read-only yang tidak berisiko bagi sistem berjalan [7]. (4) Jejak data yang dapat diaudit, sejalan dengan kebutuhan data valid/transparan [4]. Pembuktiannya melalui wawancara teknisi, uji putus-koneksi, dan perbandingan biaya terhadap penawaran produk pembanding. Setelah PoC, tim menyiapkan panduan pemasangan, konfigurasi baseline, laporan KPI, batas penggunaan, estimasi biaya pemeliharaan, dan rencana pilot. Anggaran MVP indikatif Rp4.754.000; harga komponen dan asumsi peminjaman instrumen referensi akan diverifikasi.

## 7. Pembagian Tugas dan Tanggung Jawab Tim Pelaksana (maksimum 1 halaman)

- **Arsitek sistem — Yoan Purbolingga:** memimpin kebutuhan, rancangan arsitektur, integrasi akhir dan review teknis.
- **Hardware, sensor dan keselamatan — Ahmad Saidi:** memilih/memasang sensor, pemeriksaan kalibrasi, catu daya, proteksi dan rig uji aman.
- **Firmware/akuisisi — Filjisalim C. Ramadhana:** sampling ESP32, timestamp, buffering, deteksi paket hilang dan komunikasi.
- **Edge analytics/dashboard — Rayhan Habibi:** TrustGuard, ekstraksi fitur, deteksi anomali ringan, log lokal dan grafik hasil uji.
- **Validasi pengguna/bisnis — Jenius Anugrah Ridwan:** discovery, dokumentasi pengujian, BMC dan rencana pilot. Salah satu dari lima anggota wajib dosen; status dan nama harus dikonfirmasi.

**Monitoring internal:** rapat mingguan; log keputusan dan risiko; setiap uji mencatat versi firmware, konfigurasi sensor, kondisi rig, data mentah, KPI dan isu. Review milestone tiap bulan oleh ketua dan dosen anggota/pembimbing. Perubahan target/protokol setelah melihat hasil wajib dicatat dengan alasan dan versi baru.

## 8. Daftar Pustaka

[1] PT Algatek Karbon Nusantara. “Outdoor Photobioreactors (PBR)”. Situs resmi, diakses 24 Oktober 2026. https://algateknusantara.com/outdoor-pbr/

[2] Indonesia Business Post. “Algatek presents microalgae technology to capture carbon, produce green energy”, 15 April 2025. https://indonesiabusinesspost.com/4103/environment/algatek-presents-microalgae-technology-to-capture-carbon-produce-green-energy

[3] W. H. Fathin. *Analisis Kelayakan dan Strategi Pengembangan Sistem Carbon Capture and Utilization Berbasis Mikroalga di PT Algatek Karbon Nusantara (Studi Kasus PLTGU X, Jakarta Utara).* Tesis, ITB, 2026. https://digilib.itb.ac.id/gdl/view_data/analisis-kelayakan-dan-strategi-pengembangan-sistem-carbon-capture-and-utilization-berbasis-mikroalga-di-pt-algatek-karbon-nusantara-studi-kasus-di-power-plant-pltgu-x-jakarta-utara-21325023/

[4] V. Vercellino T. *Perancangan Model Bisnis Microalgae Carbon Capture and Utilization PT Algatek Karbon Nusantara untuk Pembangkit Listrik.* Tugas Akhir, ITB, 2026. https://digilib.itb.ac.id/gdl/view_data/perancangan-model-bisnis-microalgae-carbon-capture-and-utilization-pt-algatek-karbon-nusantara-untuk-pembangkit-listrik-14422037/

[5] A. Martone et al. “Sensor-Based Monitoring Data from an Industrial System of Centrifugal Pumps.” *Data*, 10(6), 91, 2025. https://doi.org/10.3390/data10060091

[6] Sulzer Ltd. “Sulzer Sense condition monitoring system — Industrial node and gateway”. Lembar data A10269, Oktober 2024. https://www.sulzer.com/spain/-/media/files/products/pumps-accessories/brochures/industrial_node_and_gateway_a10269.pdf?la=fr-fr

[7] Flowserve. “Flowserve Introduces IPS Beacon Pump Condition Monitor”. *Automation.com*, 17 Juni 2012. https://www.automation.com/article/flowserve-introduces-ips-beacon-pump-condition-mon

[8] Banner Engineering. “Banner Engineering introduces wireless vibration and temperature sensor”. *Automation.com*, 9 Juni 2015. https://www.automation.com/article/banner-engineering-introduces-wireless-vibration-a

[9] S. Marathe et al. “CurrentSense: A novel approach for fault and drift detection in environmental IoT sensors”. ACM/IEEE IoTDI. https://classpages.cselabs.umn.edu/Fall-2021/csci8980-ec/papers/CurrentSense.pdf (lengkapi tahun dan DOI dari sumber aslinya)

[10] “IoTRepair: Systematically Addressing Device Faults in Commodity IoT (Extended Paper)”. arXiv:2002.07641. https://arxiv.org/pdf/2002.07641

[11] Z. Yang, M. del Ninno, Z. Wen, H. Hu. “An Experimental Investigation on the Multiphase Flows and Turbulent Mixing in a Flat-Panel Photobioreactor for Algae Cultivation”. *Journal of Applied Phycology*, 26(5), 2097–2107, 2014. https://doi.org/10.1007/s10811-014-0239-0

[12] P. E. Tham et al. “Sustainable smart photobioreactor for continuous cultivation of microalgae embedded with Internet of Things”. *Bioresource Technology*, 346, 126558, 2022.

[13] C. I. L. Barka. *Rancang Bangun Sistem Monitoring pH, Temperatur, dan Karbon Dioksida pada Fotobioreaktor Tipe Tubular sebagai Reduktor Karbon Dioksida.* Tugas Akhir, ITS, 2025. https://repository.its.ac.id/122644/

[14] Raspberry Pi Ltd. “Raspberry Pi 4 Model B Specifications”. https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/

[15] Analog Devices. “ADXL345: 3-Axis Digital Accelerometer Data Sheet”. https://www.analog.com/en/products/adxl345.html

[16] Analog Devices/Maxim Integrated. “DS18B20 Programmable Resolution 1-Wire Digital Thermometer”. https://www.analog.com/en/products/ds18b20.html

[17] [Isi dengan sumber resmi instrumen TKT yang dipakai program.]

## 9. Lampiran (Diunggah/Diisi dalam sistem)

- Daftar Riwayat Hidup;
- Portofolio karya inovasi/sertifikasi kompetensi.
- Diagram blok AlgaOps
- Prototype 3D AlgaOps

---

# PAKTA INTEGRITAS
## PESERTA PROGRAM HACKATHON DEEPTECH 2026
### “Kemandirian Bangsa Melalui Akselerasi Inovasi”

| Isian | Keterangan |
|---|---|
| Nama Ketua Tim | Ahmad Saidi |
| NIM/NIDN/NIP | 2311512005 |
| Nama Tim | Gagal Peruri Gagal ICDeC |
| Asal Instansi | Universitas Andalas |
| Sub Tema | Track 03 — Smarter & Sustainable Industry. |
| Judul Prototipe | AlgaOps Assurance — Sistem Edge Trust-Gated untuk Integritas Data dan Diagnosis Peralatan Bantu Fotobioreaktor Mikroalga |

Yang bertandatangan di bawah ini, mewakili seluruh anggota tim sebagaimana tercantum di atas, dengan penuh kesadaran dan tanpa paksaan dari pihak manapun, menyatakan dan berkomitmen untuk:

1. **Orisinalitas Karya:** Produk, desain, sirkuit, program, atau prototipe yang kami ajukan sepenuhnya merupakan karya orisinal tim kami, bebas dari plagiarisme, bukan hasil plagiarisme, duplikasi, atau pemanfaatan karya pihak lain tanpa izin, dan tidak melanggar Hak Kekayaan Intelektual (HKI) siapapun, dan belum pernah memenangkan pendanaan atau kompetisi sejenis di tempat lain dalam dua tahun terakhir.
2. **Akurasi Data:** Seluruh data dokumen pendukung, status mahasiswa dari PDDIKTI, serta laporan penggunaan anggaran prototipe yang kami sampaikan adalah benar dan sah sesuai kondisi riil.
3. **Komitmen Mengikuti Rangkaian Program:** Kami berkomitmen penuh mengikuti seluruh rangkaian kegiatan dari tahap pendaftaran (proposal submission), problem validation report, bootcamp, mentoring, hingga penyusunan Laporan Akhir tepat waktu.
4. **Bebas Konflik Kepentingan:** Tidak memiliki hubungan yang menimbulkan konflik kepentingan dengan juri, tim pakar, panitia, atau pihak penyelenggara yang dapat mempengaruhi objektivitas penilaian.
5. **Kerahasiaan data (NDA):** Kami bersedia menjaga kerahasiaan kekayaan intelektual (IP) milik tim lain maupun mitra industri yang terlibat dalam program ini.
6. **Kepatuhan Hukum:** Kami akan mematuhi seluruh ketentuan hukum yang berlaku, termasuk peraturan penggunaan dan pertanggungjawaban anggaran sesuai ketentuan LPDP dan ketentuan yang berlaku. Pelanggaran terhadap butir-butir pakta ini dapat dikenai sanksi berupa:
   - a. diskualifikasi dari program;
   - b. kewajiban mengembalikan seluruh dana yang telah diterima;
   - c. pencantuman dalam daftar hitam program Kemdiktisaintek dan LPDP;
   - d. pelaporan kepada pihak berwenang apabila terdapat unsur pidana.

Pakta integritas ini kami buat dengan penuh kesadaran dan memiliki kekuatan hukum. Apabila di kemudian hari ditemukan pelanggaran terhadap butir-butir pernyataan di atas, kami bersedia menerima sanksi administratif berupa pembatalan kepesertaan, pembatalan status pemenang, pengembalian seluruh dana yang telah diterima ke kas negara, beserta sanksi lain sesuai ketentuan yang berlaku.

Demikian Pakta Integritas ini kami buat dengan sebenar-benarnya untuk dapat digunakan sebagaimana mestinya.

**Bandung, 25 Oktober 2026**

| No. | Nama Lengkap | Peran | NIM/NIDN | TTD |
|---:|---|---|---|---|
| 1. | Ahmad Saidi | Ketua tim | 2311512005 | (materai) [tanda tangan gambar pada dokumen asli] |
| 2. | Yoan Purbolingga | Anggota Tim 1 |  | [tanda tangan gambar pada dokumen asli] |
| 3. | Rayhan Habibi | Anggota Tim 2 | 2411512030 | [tanda tangan gambar pada dokumen asli] |
| 4. | Filjisalim C. Ramadhana | Anggota Tim 3 | 2311512003 | [tanda tangan gambar pada dokumen asli] |
| 5. | Jenius Anugrah Ridwan | Anggota Tim 4 | 2411511004 | [tanda tangan gambar pada dokumen asli] |
