# Signal Logger

**Signal Logger** is an advanced Software-Defined Radio (SDR) and Signals Intelligence (SIGINT) platform. It captures raw RF signals using an RTL-SDR dongle, processes and decodes them using a high-performance custom C++ Digital Signal Processing (DSP) engine, and streams the decoded data to a modern web dashboard.

Currently, it supports tracking aircraft via **ADS-B** (1090 MHz) and vessels via **AIS** (161.975 MHz).

##  Features

- **Native C++ DSP Engine**:
  - Direct USB interfacing with RTL-SDR devices via `librtlsdr`.
  - Custom baseband IQ demodulation and bit extraction.
  - Full Mode S / ADS-B decoding: Supports Downlink Formats 11, 17, 18.
  - Complete CRC24 payload validation and parity checking.
  - Airborne and Surface position decoding using Compact Position Reporting (CPR).
  - Aircraft category, Squawk codes, and Emergency status detection.
  - FNV-1a hashing for 500ms sliding-window duplicate packet rejection.
- **FastAPI Backend (v2.0)**:
  - Real-time WebSocket streaming pushing updates to the frontend every second.
  - Robust REST endpoints (`/api/stats`, `/api/history`, `/api/search`).
  - Flight lifecycle tracking (first seen, message rate, timeout handling).
  - Military ICAO identification and alerting.
  - Auto-rotating CSV Data Lake (archives when > 100MB).
  - GeoJSON export support for external GIS software.
- **Modern React Dashboard**:
  - Live tactical map with satellite imagery via Leaflet.
  - Real-time fading of stale contacts (90s signal timeout).
  - Live statistics, altitude histograms, and signal quality metrics.
  - Military-grade UI with advanced color-coding (e.g., emergencies flashing red, military units in green).

##  Architecture

The system consists of three main decoupled layers communicating asynchronously:

1. **The Ingestor & Decoder (C++ / Win32)**
   - **File:** `src/main.cpp`, `include/Decoder.h`
   - Captures raw RF frames, runs DSP algorithms, maintains a high-speed in-memory database of current contacts, and writes the structured outputs to the `data_lake.csv` state file. Includes a native Win32 Control Panel GUI for hardware configuration.
2. **The API Bridge (Python / FastAPI)**
   - **File:** `dashboard/backend/main.py`
   - Monitors the Data Lake in real-time. Manages the lifecycle of contacts (detecting dropped signals), identifies military aircraft, handles analytics, and broadcasts updates via WebSockets.
3. **The Tactical Dashboard (Node.js / React)**
   - **File:** `dashboard/frontend/`
   - A highly responsive browser-based UI that renders the live data stream onto an interactive map with accompanying telemetry tables.

##  Getting Started

### Prerequisites
- Windows OS (C++ engine utilizes Win32 APIs).
- RTL-SDR USB Dongle (with proper WinUSB drivers via Zadig).
- Visual Studio / MSBuild for compiling the C++ engine.
- Python 3.10+
- Node.js 18+

### 1. Compile the C++ Engine
Open the project in Visual Studio and build `SignalLogger.vcxproj` in **Release x64** mode. Ensure that `rtlsdr.dll` and `pthreadVCE2.dll` are in your build/execution directory.

### 2. Start the Backend
Navigate to the backend directory and install dependencies:
```bash
cd dashboard/backend
pip install -r requirements.txt
python main.py
```
The FastAPI server will start on `http://localhost:8000`.

### 3. Start the Frontend Dashboard
Open a new terminal, navigate to the frontend directory, and run the dev server:
```bash
cd dashboard/frontend
npm install
npm run dev
```
Open `http://localhost:5173` in your browser.

### 4. Initiate Signal Capture
1. Launch the compiled `SignalLogger.exe`.
2. Select **Source:** Local USB (RTL-SDR).
3. Select **Mode:** ADS-B.
4. Click **START**.
5. Watch the dashboard populate as aircraft signals are captured and decoded in real-time
