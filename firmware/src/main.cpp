#include <Arduino.h>
#include "config.h"
#include "logging.h"
#include "sensors.h"
#include "net.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  LOGI("main", "boot unit=%s fw=%s reset_reason=%d", UNIT_ID, FW_VERSION, (int)esp_reset_reason());
  sensorsBegin();
#if ENABLE_NET
  netBegin();
#endif
}

void loop() {
#if ENABLE_NET
  vTaskDelay(pdMS_TO_TICKS(1000));               // everything runs in tasks
#else
  vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
  FlowData f = flowSnapshot();
  VibData v = vibSnapshot();
  MicData m = micSnapshot();
  LOGI("bench", "Q=%.2f L/min tot=%.3f L | vib rms=%.1f/%.1f/%.1f mg peak=%.0f mg dom=%.0f Hz | mic %.1f dBFS cen=%.0f Hz | ok mpu=%d mic=%d",
       f.lpm, f.totalL, v.rms[0] * 1000, v.rms[1] * 1000, v.rms[2] * 1000, v.peak * 1000, v.domHz,
       m.db, m.centroid, v.ok, m.ok);
#endif
}

