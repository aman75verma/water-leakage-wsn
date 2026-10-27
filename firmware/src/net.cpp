#include "config.h"
#if ENABLE_NET
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#if ENABLE_OTA
#include <ArduinoOTA.h>
#endif
#include "logging.h"
#include "sensors.h"
#include "net.h"

static char *ring = nullptr;
static int rHead = 0, rCount = 0;
static uint32_t g_seq = 0, g_drops = 0, g_reconnects = 0;

#define SLOT(i) (ring + (size_t)(((i) % RING_SLOTS)) * SLOT_BYTES)

static void ringPush(const char *s, size_t len) {
  if (rCount == RING_SLOTS) { rHead = (rHead + 1) % RING_SLOTS; rCount--; g_drops++; }   // drop oldest
  memcpy(SLOT(rHead + rCount), s, len + 1);
  rCount++;
}
static const char *ringPeek() { return rCount ? SLOT(rHead) : nullptr; }
static void ringPop() { rHead = (rHead + 1) % RING_SLOTS; rCount--; }

static inline float rd(float x, int dec) {
  if (isnan(x) || isinf(x)) return 0;
  float p = powf(10.0f, dec);
  return roundf(x * p) / p;
}

static size_t buildMessage(char *out, size_t cap) {
  FlowData f = flowSnapshot();
  VibData v = vibSnapshot();
  MicData m = micSnapshot();
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  bool tok = tv.tv_sec > 1700000000;

  JsonDocument d;
  d["unit"] = UNIT_ID;
  d["fw"] = FW_VERSION;
  d["seq"] = g_seq++;
  d["ts"] = tok ? ((uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000)) : 0ULL;
  d["time_ok"] = tok;
  d["ms"] = (uint32_t)millis();

  d["flow"]["lpm"] = rd(f.lpm, 3);
  d["flow"]["total_l"] = rd(f.totalL, 3);
  d["flow"]["pulses"] = f.pulses;
  d["flow"]["active"] = f.active;

  d["vib"]["ok"] = v.ok;
  JsonArray rms = d["vib"]["rms"].to<JsonArray>();
  for (int i = 0; i < 3; i++) rms.add(rd(v.rms[i], 5));
  d["vib"]["peak"] = rd(v.peak, 4);
  d["vib"]["dom_hz"] = rd(v.domHz, 1);
  JsonArray vb = d["vib"]["bands"].to<JsonArray>();
  for (int i = 0; i < 4; i++) vb.add(rd(v.bands[i], 1));

  d["mic"]["ok"] = m.ok;
  d["mic"]["db"] = rd(m.db, 1);
  d["mic"]["centroid"] = rd(m.centroid, 1);
  JsonArray mb = d["mic"]["bands"].to<JsonArray>();
  for (int i = 0; i < 5; i++) mb.add(rd(m.bands[i], 1));
  JsonArray ms = d["mic"]["spec"].to<JsonArray>();
  for (int i = 0; i < 16; i++) ms.add(rd(m.spec[i], 1));

  d["sys"]["rssi"] = WiFi.RSSI();
  d["sys"]["heap"] = ESP.getFreeHeap();
  d["sys"]["up_s"] = (uint32_t)(millis() / 1000);
  d["sys"]["reconnects"] = g_reconnects;
  d["sys"]["buf"] = rCount;
  d["sys"]["drops"] = g_drops;

  if (measureJson(d) >= cap) { LOGE("net", "message too large (%u B)", (unsigned)measureJson(d)); return 0; }
  return serializeJson(d, out, cap);
}

static int postJson(const char *body) {
  static WiFiClient client;
  static HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(1500);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, SERVER_URL)) return -100;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)body, strlen(body));
  http.end();
  return code;
}

static void wifiStart() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                         // lower latency, steadier link
  WiFi.setHostname(UNIT_ID);
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    switch (ev) {
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        g_reconnects++;
        LOGW("wifi", "disconnected, reason=%d (reconnects=%lu)", info.wifi_sta_disconnected.reason, (unsigned long)g_reconnects);
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        LOGI("wifi", "connected, ip=%s rssi=%d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        break;
      default: break;
    }
  });
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  LOGI("wifi", "connecting to '%s'", WIFI_SSID);
}

static void netTask(void *) {
  esp_task_wdt_add(NULL);
  uint32_t nextPub = millis() + PUBLISH_PERIOD_MS;
  uint32_t lastWifiTry = 0, offlineSince = millis(), nextSend = 0, lastStat = 0;
  bool ntpStarted = false;
  wifiStart();

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    bool up = WiFi.status() == WL_CONNECTED;

    if (up) {
      offlineSince = now;
      if (!ntpStarted) {
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);       // UTC
        ntpStarted = true;
        LOGI("ntp", "sync started");
#if ENABLE_OTA
        ArduinoOTA.setHostname(UNIT_ID);
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.onStart([]() { LOGW("ota", "update starting"); });
        ArduinoOTA.begin();
#endif
      }
#if ENABLE_OTA
      ArduinoOTA.handle();
#endif
    } else {
      if (now - lastWifiTry > 10000) {
        lastWifiTry = now;
        LOGW("wifi", "not connected, retrying (offline %lu s)", (unsigned long)((now - offlineSince) / 1000));
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      if (now - offlineSince > WIFI_RESTART_AFTER_MS) {
        LOGE("wifi", "offline too long, rebooting");
        delay(200);
        ESP.restart();
      }
    }

    // ---- produce a message on schedule (also while offline -> ring buffer) ----
    if ((int32_t)(now - nextPub) >= 0) {
      nextPub += PUBLISH_PERIOD_MS;
      if ((int32_t)(now - nextPub) >= 0) nextPub = now + PUBLISH_PERIOD_MS;   // we were blocked; don't burst
      char tmp[SLOT_BYTES];
      size_t n = buildMessage(tmp, sizeof(tmp));
      if (n) ringPush(tmp, n);
    }

    // ---- drain buffer oldest-first ----
    if (up && rCount && now >= nextSend) {
      int sent = 0;
      while (rCount && sent < 4) {
        int code = postJson(ringPeek());
        if (code >= 200 && code < 300) { ringPop(); sent++; }
        else if (code >= 400 && code < 500) { LOGW("net", "server rejected message (HTTP %d) - dropping it", code); ringPop(); }
        else {
          LOGW("net", "POST failed (code %d %s), buffered=%d", code, HTTPClient::errorToString(code).c_str(), rCount);
          nextSend = millis() + 2000;                       // back off 2 s
          break;
        }
      }
    }

    if (now - lastStat >= 10000) {
      lastStat = now;
      LOGI("stat", "up=%lus heap=%u rssi=%d buf=%d drops=%lu reconnects=%lu ntp=%s",
           (unsigned long)(now / 1000), (unsigned)ESP.getFreeHeap(), WiFi.RSSI(), rCount,
           (unsigned long)g_drops, (unsigned long)g_reconnects, time(nullptr) > 1700000000 ? "ok" : "waiting");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void netBegin() {
  ring = (char *)malloc((size_t)RING_SLOTS * SLOT_BYTES);
  if (!ring) { LOGE("net", "ring buffer alloc failed"); return; }
  esp_task_wdt_init(WDT_TIMEOUT_S, true);       // panic + reboot if netTask hangs
  xTaskCreatePinnedToCore(netTask, "net", 10240, nullptr, 2, nullptr, 0);
}
#else
#include "net.h"
void netBegin() {}
#endif

