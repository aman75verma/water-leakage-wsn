# Water Leakage Detection System: Hardware & User Guide

A complete reference manual for wiring, configuring, calibrating, operating, and interpreting the dual-ESP32 pipeline leak detection system.

---

## 1. System Overview & Pipe Architecture

The setup consists of a 5-foot horizontal test pipeline with two identical ESP32 sensor units and a central leak simulation point:

```
SOURCE ── [H1: 53 cm] ── [FM_B] ── [H2: 23 cm] ── [ T ] ── [H3: 23 cm] ── [FM_A] ── [H4: 53 cm] ── SINK
            ▲                                       │                       ▲
            │ unit_source (ESP32 "B")               └── Leak branch:        │ unit_sink (ESP32 "A")
            │ (MPU6050 + INMP441 on H1)                 10 cm hose +        │ (MPU6050 + INMP441 on H4)
                                                        pinch clamp → jug
```

- **`unit_source`** (Upstream): ESP32 "B" reading flow meter `FM_B`, MPU6050 accelerometer, and INMP441 microphone clamped to hose `H1`.
- **`unit_sink`** (Downstream): ESP32 "A" reading flow meter `FM_A`, MPU6050 accelerometer, and INMP441 microphone clamped to hose `H4`.
- **T-Joint Branch**: Located between the two flow meters. Opening the pinch clamp creates a simulated leak where $Q_{source} > Q_{sink}$.
- **Flow Direction**: Water must enter at SOURCE and exit at SINK. The arrows stamped on both brass flow meters **must point toward SINK**.

---

## 2. Complete Pin Wiring Reference

Both ESP32 units (`unit_source` and `unit_sink`) have identical pin assignments:

| Function | Sensor Pin | ESP32 Pin | GPIO | Power / Logic Level | Notes |
|---|---|---|---|---|---|
| **I2C SDA** | MPU6050 SDA | D21 | **21** | 3.3V Logic | Default I2C Data |
| **I2C SCL** | MPU6050 SCL | D22 | **22** | 3.3V Logic | Default I2C Clock |
| **MPU Power** | MPU6050 VCC | 3V3 | — | **3.3V Supply** | **Never connect to 5V** |
| **MPU Ground** | MPU6050 GND | GND | — | Common GND | |
| **MPU Address** | MPU6050 AD0 | GND | — | Pull to GND | Sets I2C address to **0x68** (never float) |
| **I2S Bit Clock** | INMP441 SCK | D26 | **26** | 3.3V Logic | BCLK (16 kHz × 64 = 1.024 MHz) |
| **I2S Word Select** | INMP441 WS | D25 | **25** | 3.3V Logic | LRCLK |
| **I2S Data In** | INMP441 SD | D33 | **33** | 3.3V Logic | Data out of mic into ESP32 |
| **Mic Channel** | INMP441 L/R | GND | — | Tie to GND | **GND = Left channel**. Never leave floating! |
| **Mic Power** | INMP441 VDD | 3V3 | — | **3.3V Supply** | **Never connect to 5V** (damages sensor) |
| **Mic Ground** | INMP441 GND | GND | — | Common GND | |
| **Flow Signal** | SEN-HZ41WC Yellow | D27 | **27** | 3.3V Logic | Requires **10 kΩ pull-up to 3V3** (see §3) |
| **Flow Power** | SEN-HZ41WC Red | VIN | — | **5V Supply** | Needs ≥ 4.5V from USB VIN rail |
| **Flow Ground** | SEN-HZ41WC Black | GND | — | Common GND | **Mandatory common ground** |

### ASCII Wiring Diagram

```
                         ┌───────────────── ESP32 DevKit V1 ──────────────┐
                         │ (USB 5V in)                                    │
   MPU6050 (GY-521)       │                                                │
    VCC ──────────────────┤ 3V3                                            │
    GND ──────────────────┤ GND                                            │
    SDA ──────────────────┤ D21 (GPIO21)                                   │
    SCL ──────────────────┤ D22 (GPIO22)                                   │
    AD0 ──────────────────┤ GND (selects I2C addr 0x68)                    │
                          │                                                │
   INMP441 Microphone     │                                                │
    VDD ──────────────────┤ 3V3                                            │
    GND ──────────────────┤ GND                                            │
    L/R ──────────────────┤ GND   (left channel selected)                  │
    SCK ──────────────────┤ D26 (GPIO26)                                   │
    WS  ──────────────────┤ D25 (GPIO25)                                   │
    SD  ──────────────────┤ D33 (GPIO33)                                   │
                          │                                                │
   SEN-HZ41WC Flow Meter  │                                                │
    Red    (V+) ──────────┤ VIN (5V from USB)                              │
    Black  (GND) ─────────┤ GND                                            │
    Yellow (sig) ─┬───────┤ D27 (GPIO27)                                   │
                  └─[10k]─┤ 3V3          (external pull-up to 3.3V)        │
                          └────────────────────────────────────────────────┘
```

> **Recommendation**: Use a mini breadboard as a common **3V3**, **GND**, and **5V (VIN)** rail hub.

---

## 3. Flow Sensor Output Type & Safe Circuitry

The Hall-effect flow sensor requires 5V supply, but ESP32 GPIOs tolerate at most 3.3V. Before connecting GPIO 27, test the sensor output type with a multimeter:

1. Power the sensor with Red → 5V and Black → GND.
2. Measure DC voltage on the Yellow wire against GND (rotor stopped, nothing else attached).

| Reading at rest | Output Type | Required Circuit |
|---|---|---|
| **≈ 0 V or floating** | **Open-Collector (Standard / Expected)** | Connect a **10 kΩ pull-up resistor from Yellow to 3V3**. Wire Yellow to GPIO 27. Line never exceeds 3.3V. |
| **Solid ~5 V** | Push-Pull 5V / Internal 5V Pull-up | Use a **voltage divider**: Yellow → **10 kΩ** → GPIO 27, and GPIO 27 → **20 kΩ** → GND. Output is $5 \times \frac{20}{30} = 3.33\text{ V}$. **Do NOT add pull-up to 3V3.** |

```
 OPEN-COLLECTOR (Default)                 PUSH-PULL 5V (Alternative)

 3V3 ──[10 kΩ]──┬── GPIO 27               Yellow ──[10 kΩ]──┬── GPIO 27
 Yellow ────────┘                                           └──[20 kΩ]── GND
```

---

## 4. Configuration: Where to Feed Your Details

All user settings are located in [firmware/include/config.h](file:///c:/Users/ammua/Desktop/waterleakage/firmware/include/config.h):

| Setting | Define Name | Default | What to Enter |
|---|---|---|---|
| **Wi-Fi SSID** | `WIFI_SSID` | `"YOUR_WIFI_NAME"` | Name of your 2.4 GHz Wi-Fi network (ESP32 does not support 5 GHz) |
| **Wi-Fi Password** | `WIFI_PASS` | `"YOUR_WIFI_PASSWORD"` | Your Wi-Fi network password |
| **Server URL** | `SERVER_URL` | `"http://192.168.1.50:8000/ingest"` | Replace `192.168.1.50` with your laptop's local IP address (find using `ipconfig` on Windows or `ifconfig` on Linux) |
| **Flow Meter Pulses** | `FLOW_PULSES_PER_LITER` | `5880.0f` | Specific calibration constant (pulses per litre) determined during bucket test (§6) |
| **Unit Identity** | `UNIT_ID` | `"unit_source"` | Set to `"unit_source"` or `"unit_sink"`. *(Automatically set when using PlatformIO target environments)* |
| **OTA Password** | `OTA_PASSWORD` | `"change-me"` | Secret password if enabling Over-The-Air firmware updates |

---

## 5. Sensor Mounting Guide

Sensor mounting determines signal quality and detection accuracy:

- **MPU6050 Accelerometer**:
  - Mount flat onto a **rigid plastic saddle** or epoxy putty clamped directly against the hose wall with a worm-drive clamp.
  - Mount at least **20 cm away from the flow meter rotor** (rotor creates mechanical noise).
  - Flexible PVC dampens vibrations; rigid coupling ensures leak turbulence is transmitted to the accelerometer.
- **INMP441 Acoustic Microphone**:
  - Point the small acoustic port (the hole on the PCB) toward the hose wall, spaced **3 to 5 mm away**.
  - Wrap the edges of the PCB with foam to isolate against room audio (voices, fans, pumps).
  - Do **not** cover or seal the microphone hole with glue or tape.
- **Waterproofing & Electronics**:
  - Keep ESP32 and breadboards **elevated and inside a plastic bag or enclosure**.
  - Water and electronics must remain physically separated. Route cables downward with drip loops.

---

## 6. Flow Sensor Calibration (Bucket Method)

Every flow meter has slight manufacturing variations. Calibrate each meter before testing:

1. Flash the calibration sketch to the ESP32:
   ```bash
   cd firmware
   pio run -e bringup_c -t upload && pio device monitor -e bringup_c
   ```
2. Turn on water at standard flow rate (≥ 2 L/min).
3. Type `r` in the serial terminal and hit Enter to reset the pulse counter to 0.
4. Immediately direct the hose outlet into a measuring jug until exactly 2.0 litres are collected.
5. Move the hose away and enter `c2.0` in the serial monitor.
6. The terminal prints the calibrated pulses per litre (e.g., `CALIBRATION: 11760 pulses / 2.000 L = 5880.0 pulses per litre`).
7. Repeat 3 times, average the result, and update `FLOW_PULSES_PER_LITER` in `config.h` for that unit.

---

## 7. How to Run the System

### 7.1 Start the Server

1. Open PowerShell and navigate to `server/`:
   ```powershell
   cd c:\Users\ammua\Desktop\waterleakage\server
   python -m venv .venv
   .\.venv\Scripts\Activate.ps1
   pip install -r requirements.txt
   uvicorn main:app --host 0.0.0.0 --port 8000
   ```
2. Note your laptop's local IP using `ipconfig` (e.g. `192.168.1.100`).
3. Allow Python through the Windows Firewall prompt on private networks.
4. Open the live dashboard in your browser: `http://localhost:8000`.

### 7.2 Flash the ESP32 Units

Connect the first ESP32 via USB and upload the source unit firmware:
```powershell
cd c:\Users\ammua\Desktop\waterleakage\firmware
pio run -e unit_source -t upload
```

Connect the second ESP32 via USB and upload the sink unit firmware:
```powershell
pio run -e unit_sink -t upload
```

### 7.3 Step-by-Step Bringup Diagnostics

If any sensor fails to report, test each module in isolation:
```powershell
# 1. Test I2C bus & MPU6050
pio run -e bringup_a -t upload && pio device monitor -e bringup_a

# 2. Test INMP441 microphone (clap to observe dB jump)
pio run -e bringup_b -t upload && pio device monitor -e bringup_b

# 3. Test Flow Sensor pulse count & calibration
pio run -e bringup_c -t upload && pio device monitor -e bringup_c

# 4. Test all sensors locally with FreeRTOS (No Wi-Fi)
pio run -e bringup_d -t upload && pio device monitor -e bringup_d
```

---

## 8. How to Interpret Results

### 8.1 Dashboard States & Banner

The banner at the top of the dashboard displays the active system verdict:

| Banner Color | State | Condition | Meaning & Required Action |
|---|---|---|---|
| 🟢 **Green** | **NORMAL** | Score < 0.30 | Pipeline intact. Source and sink flow match closely. Acoustic and vibration levels match baseline. |
| 🟡 **Amber** | **SUSPECTED** | Score ≥ 0.30 for 3 continuous seconds | Anomaly detected. Flow mismatch or acoustic hiss present. Inspect pipeline. |
| 🔴 **Red** | **LEAK DETECTED** | Score ≥ 0.60 for 4 continuous seconds | Confirmed leak. Discrepancy between source and sink flow exceeds safe tolerance. |
| ⚪ **Grey** | **CALIBRATING / NO DATA** | Offline or calibrating | Baseline is being captured or units are disconnected. |

### 8.2 Key Dashboard Metrics

- **Combined Leak Score (0.00 to 1.00)**:
  $Score = 0.65 \times s_{flow} + 0.35 \times s_{ac}$
  Combines physical volume mismatch ($s_{flow}$) with high-frequency sound/vibration z-scores ($s_{ac}$).
- **Flow Mismatch (L/min)**:
  $Mismatch = Q_{source}(t - \Delta t) - Q_{sink}(t) - \text{Bias}$
  Measures water escaping between meters, accounting for transit delay ($\Delta t \approx 1\text{–}2\text{ s}$) and initial meter bias.
- **Acoustic & Vibration RMS**:
  - `cVib (mg)`: Shows vibration turbulence in milli-g ($1\text{ g} = 9.8\text{ m/s}^2$).
  - `cMic (dBFS)`: Noise level in decibels relative to full scale. Leak hiss generates broadband power in 1 kHz – 4 kHz.
  - `Spectrum Bar Chart`: 16 logarithmic frequency bins from 100 Hz to 8 kHz.
- **Location Estimate**:
  Compares acoustic excess $A_{source}$ vs $A_{sink}$. Reports `"closer to unit_source"`, `"closer to unit_sink"`, or `"between units (central)"`.

### 8.3 Establishing the Baseline

1. Ensure the T-joint branch clamp is **completely closed**.
2. Run water through the pipeline at normal operating flow (≥ 2 L/min) for 1 minute to flush air bubbles.
3. Click **Start baseline** on the dashboard (runs for 3 minutes).
4. Keep the room quiet during calibration.
5. When complete, the server calculates flow meter bias and ambient acoustic baselines. The banner will turn green (**NORMAL**).

---

## 9. Troubleshooting Quick Reference

| Symptom | Probable Cause | Action |
|---|---|---|
| **Flow reads 0.00 L/min** | Flow direction reversed; yellow wire missing 10k pull-up; red not on 5V; flow < 1 L/min | Check arrow on brass meter points toward sink; verify 10 kΩ pull-up to 3V3; test with `bringup_c` by gently blowing into meter |
| **Spurious flow counts without water** | Signal wire floating (missing pull-up); electrical noise | Secure the 10 kΩ pull-up between Yellow and 3V3; keep wiring short |
| **INMP441 Mic flat (~ -120 dBFS)** | L/R pin floating or tied to 3V3; bad SD/WS/SCK connection | Connect L/R to GND (left channel); run `bringup_b` and clap hands to verify |
| **MPU6050 not detected (I2C error)** | SDA/SCL swapped; AD0 pin floating; 5V on 3V3 pin | Wire AD0 to GND; connect SDA to GPIO 21 and SCL to GPIO 22; check 3.3V supply |
| **ESP32 reboot loop (`Brownout detector`)** | USB voltage sagging during Wi-Fi transmission; poor USB cable | Use a high-quality USB cable and a ≥ 2A power adapter; add a 100 µF capacitor across 5V and GND |
| **Unit offline on dashboard** | Incorrect IP in `SERVER_URL`; laptop firewall blocking port 8000; ESP32 on 5 GHz Wi-Fi | Verify laptop IP with `ipconfig`; allow port 8000 through Windows Firewall; connect ESP32 to 2.4 GHz network |
| **Banner flapping between states** | Hold times too short; thresholds too sensitive | In dashboard threshold settings, increase `suspect_hold_s` or widen gap between `leak_on` and `leak_off` |

