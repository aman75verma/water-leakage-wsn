#pragma once

#define FW_VERSION "1.0.0"

// ---------- Unit identity (set per board; platformio envs override via -DUNIT_ID) ----------
#ifndef UNIT_ID
#define UNIT_ID "unit_source"            // "unit_source" or "unit_sink"
#endif

// ---------- Feature switches ----------
#ifndef ENABLE_NET
#define ENABLE_NET 1                      // 0 = bench mode (serial output only)
#endif
#define ENABLE_OTA 0                      // 1 = ArduinoOTA (set OTA_PASSWORD!)

// ---------- Wi-Fi / server (EDIT THESE) ----------
#define WIFI_SSID     "YOUR_WIFI_NAME"    // 2.4 GHz network
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"
#define SERVER_URL    "http://192.168.1.50:8000/ingest"   // laptop IP, find with ipconfig / ifconfig
#define OTA_PASSWORD  "change-me"
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"

// ---------- Pins (see README section 3) ----------
#define PIN_I2C_SDA   21
#define PIN_I2C_SCL   22
#define PIN_I2S_BCLK  26
#define PIN_I2S_WS    25
#define PIN_I2S_SD    33
#define PIN_FLOW      27

// ---------- Flow sensor ----------
// VERIFY / CALIBRATE: F = 98*Q (Hz, Q in L/min) -> 98*60 = 5880 pulses per litre (assumption!).
#define FLOW_PULSES_PER_LITER  5880.0f
#define FLOW_MIN_PULSE_US      150        // ignore edges closer than this (glitch filter, ~6.6 kHz max)
#define FLOW_IDLE_MS           5000       // no pulses for this long -> flow.active=false

// ---------- MPU6050 ----------
#define MPU_ADDR      0x68                // AD0 = GND
#define MPU_FS_HZ     1000                // fixed by SMPLRT_DIV below
#define MPU_WIN       512                 // samples per feature window (0.512 s) - power of 2

// ---------- INMP441 ----------
#define MIC_FS_HZ     16000
#define MIC_WIN       2048                // 128 ms per feature window - power of 2

// ---------- Publishing / buffering ----------
#define PUBLISH_PERIOD_MS 512
#define RING_SLOTS        48              // ~25 s of offline buffer
#define SLOT_BYTES        1024            // 48 KB heap total
#define HTTP_TIMEOUT_MS   2500
#define WIFI_RESTART_AFTER_MS 180000      // reboot if offline this long
#define WDT_TIMEOUT_S     30
