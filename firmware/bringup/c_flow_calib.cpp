#include <Arduino.h>
#include "config.h"

static volatile uint32_t pulses = 0, lastUs = 0;
static void IRAM_ATTR isr() {
  uint32_t now = (uint32_t)micros();
  if ((uint32_t)(now - lastUs) >= FLOW_MIN_PULSE_US) { pulses++; lastUs = now; }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_FLOW, INPUT_PULLUP);                    // plus the external 10k to 3V3
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), isr, FALLING);
  Serial.printf("Flow test. K=%.1f pulses/L (config).  Commands:  r = reset   c<litres> = calibrate, e.g. c2.0\n",
                FLOW_PULSES_PER_LITER);
}

void loop() {
  static uint32_t lastP = 0, lastMs = millis();
  uint32_t now = millis();
  if (now - lastMs >= 1000) {
    uint32_t p = pulses, dp = p - lastP;
    float dt = (now - lastMs) / 1000.0f;
    float hz = dp / dt;
    float lpm = (dp / FLOW_PULSES_PER_LITER) * 60.0f / dt;
    Serial.printf("total=%lu  +%lu pulses/s  %.1f Hz  %.2f L/min  %.3f L\n",
                  (unsigned long)p, (unsigned long)dp, hz, lpm, p / FLOW_PULSES_PER_LITER);
    lastP = p; lastMs = now;
  }
  if (Serial.available()) {
    String s = Serial.readStringUntil('\n'); s.trim();
    if (s == "r") { noInterrupts(); pulses = 0; interrupts(); Serial.println("counter reset"); }
    else if (s.startsWith("c")) {
      float litres = s.substring(1).toFloat();
      if (litres > 0) {
        Serial.printf("CALIBRATION: %lu pulses / %.3f L = %.1f pulses per litre  -> set FLOW_PULSES_PER_LITER in config.h\n",
                      (unsigned long)pulses, litres, pulses / litres);
      }
    }
  }
}

