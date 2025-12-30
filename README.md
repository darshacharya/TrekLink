**LoRa Trekking Dashboard & Simulator**

This repository contains a small LoRa-based telemetry demo and tooling for a trekking/field-monitoring setup. It includes an ESP32 Master node (polling slaves over LoRa), example Slave firmware, a browser-based visualization (`index.html`), a React-based LoRa simulator component (`LoRaSimulator.jsx`), and a Flask-based laptop dashboard (`python_dashboard.py`). The project stores and serves map tiles for offline mapping under `tiles/`.

**Project Overview**
- **Purpose:** Demonstrate a master-polled, half-duplex LoRa network for distributed sensor nodes (temperature, pressure, altitude, battery, GPS). Includes logging, alert broadcast, live visualization, and a simulator for testing without hardware.
- **Main features:** Master polling, SPIFFS logging (ESP32), alert broadcast, local web UI (Leaflet + Chart.js), CSV export from simulator, Flask dashboard to collect and visualize real nodes.

**Repository Components**
- **Web UI (browser):** [index.html](index.html) — single-page Leaflet map + Chart.js dashboard used to visualize nodes and animations.
- **Simulator (React):** [LoRaSimulator.jsx](LoRaSimulator.jsx) — self-contained React component that simulates master/slaves, produces in-memory CSVs (history & latest), and provides CSV download compatible with the Python dashboard.
- **Python Dashboard:** [python_dashboard.py](python_dashboard.py) — Flask app that polls an ESP32 master REST API (configurable `ESP32_IP`) and exposes endpoints and an interactive Folium map.
- **ESP32 Master:** [master/master.ino](master/master.ino) — Master firmware that polls slaves, receives JSON telemetry, logs to SPIFFS (internal flash), and broadcasts alerts. See top-of-file comments for wiring and partition notes.
- **Example Slave:** [slave_me/slave_me.ino](slave_me/slave_me.ino) — Slave firmware that reads sensors (BMP280/BMP085), GPS, battery level, and responds with JSON telemetry when polled. Provides button-triggered one-shot alert and reacts to broadcasted alerts.
- **Map tiles:** `tiles/` — offline tile set used by the web UI (pre-downloaded tiles for the map view).
- **Data files:** `master_gps_wifi.txt`, `slave_gps_enhanced.txt` — example log/data files. `master/old_sd_data/` contains older logs.

**Quick Requirements**
- Arduino/ESP32: Arduino IDE or PlatformIO, ESP32 board support, required libraries: `LoRa`, `SPIFFS` (or `LittleFS`), `ArduinoJson`, `Adafruit_BMP280`/`BMP085`, `TinyGPSPlus` as used in slave sketch.
- Python dashboard: Python 3.8+, pip packages: `flask`, `folium`, `requests`, `pandas`, `plotly` (install with `pip install flask folium requests pandas plotly`).
- Browser: Modern browser for `index.html` (uses Leaflet and Chart.js from CDN). For the simulator, a React environment (Create React App or similar) is required to use `LoRaSimulator.jsx`.

**Quickstart — Web UI (Local, no server)**
- Open the file [index.html](index.html) in a browser (double-click or `File → Open`). The page uses CDN assets for Leaflet and Chart.js. It simulates nodes locally (controls on the left) and displays telemetry in the right panel.

**Quickstart — LoRa Simulator (React)**
- To use `LoRaSimulator.jsx` inside a React app:
  - Create a React app (`npx create-react-app myapp --template javascript`) or use an existing project.
  - Place `LoRaSimulator.jsx` under `src/` and import it into a page.
  - Install any UI/icon dependencies used (e.g., `lucide-react`) if desired: `npm i lucide-react`.
  - Run the app (`npm start`). The simulator produces two CSV exports (history and latest) with header:

  CSV header used by the simulator:
  `Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,Lat,Lon`

  The simulator's CSVs can be downloaded from the UI and imported into the Python dashboard or other analysis tools.

**Quickstart — Python Dashboard**
- Install requirements:
  ```bash
  pip install flask folium requests pandas plotly
  ```
- Configure `ESP32_IP` inside [python_dashboard.py](python_dashboard.py) to point at your Master node (default `192.168.4.1`).
- Run the dashboard (from repo root):
  ```bash
  python python_dashboard.py
  ```
- The Flask app exposes endpoints for current data (`/api/data`), alerts (`/api/alerts`), interactive map (`/api/map`) and CSV downloads (`/api/download`). Use the UI served at `/` to view the dashboard.

**Quickstart — ESP32 Master (flash)**
- Open [master/master.ino](master/master.ino) in Arduino IDE or PlatformIO.
- Ensure ESP32 board package installed. Select the correct board and the partition scheme: Tools → Partition Scheme → "Default 4MB with spiffs".
- Install required libraries: `LoRa` (by Sandeep Mistry), `ArduinoJson`, etc.
- If using SPIFFS data, upload data/format SPIFFS on first run (the sketch calls `SPIFFS.begin(true)` which formats on mount failure, but for initial files you may want to use Tools → ESP32 Sketch Data Upload to provision files).
- Wiring notes (from header comments in the sketch):
  - LoRa (VSPI): NSS/CS=GPIO5, RST=GPIO14, DIO0=GPIO26, SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23
  - Buzzer=GPIO21, LED=GPIO22
  - LoRa freq: 433 MHz (change in code if you use a different band)

**Quickstart — ESP32 Slave (flash)**
- Open [slave_me/slave_me.ino](slave_me/slave_me.ino).
- Required sensors: BMP280 or BMP085 (I2C), GPS (NEO-6M/other) on UART pins (defaults in sketch), battery ADC pin and a button for alert.
- GPIO notes (as in sketch): LED=25, BUTTON=4, BATTERY_ADC=34, GPS RX/TX pins defined.

**Data & Log Format**
- Master SPIFFS logs CSV-like lines containing: Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,JSON
- Simulator CSV header: `Timestamp,NodeID,Temp,Pressure,Altitude,Battery,Alert,RSSI,SNR,Lat,Lon` — compatible with the Python dashboard's CSV ingestion after minor column mapping.

**Troubleshooting & Tips**
- If LoRa fails to initialize on ESP32 master/slave, verify wiring and correct frequency (`433E6` in code). Confirm antenna is connected.
- For SPIFFS storage issues: ensure partition scheme includes SPIFFS and format SPIFFS before running. The master sketch calls `SPIFFS.begin(true)` (format if mount fails) but using the "ESP32 Sketch Data Upload" tool can help provision files.
- If Python dashboard cannot reach the ESP32 master, set `ESP32_IP` correctly and verify network connectivity (ESP32 may be an access point at `192.168.4.1` by default).
- Simulator CSV downloads are compatible with `python_dashboard.py` for offline testing.

**File Structure (high-level)**
- [index.html](index.html) — browser UI (Leaflet + Chart.js)
- [LoRaSimulator.jsx](LoRaSimulator.jsx) — React simulator component (CSV export)
- [python_dashboard.py](python_dashboard.py) — Flask dashboard and collector
- [master/](master/) — master firmware and web assets
- [slave_me/](slave_me/) — example slave firmware
- [tiles/](tiles/) — map tiles for offline map view
- data and logs: `master_gps_wifi.txt`, `slave_gps_enhanced.txt`, `master/old_sd_data/`

**Contributing / Extending**
- Add new node types by extending simulator defaults in `LoRaSimulator.jsx` and by adding support in `master/master.ino` parsing if you change telemetry fields.
- To change polling timing or retry logic, edit `POLL_INTERVAL`, `RESPONSE_TIMEOUT`, and `MAX_RETRIES` in `master/master.ino` and matching parameters in `LoRaSimulator.jsx`.

**License**
- No license file is provided in the repository. If you plan to share or publish this project, add a `LICENSE` file (MIT or similar recommended).

**Need help?**
- Tell me which component you want to run first (Web UI / Simulator / Master / Python dashboard) and I can provide step-by-step commands or help prepare an Arduino/React environment.
