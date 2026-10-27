#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <time.h>

// Compile-time level: 0 none, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG
#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

inline void logPrint(char lvl, const char *tag, const char *fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char ts[16] = "--:--:--";                       // UTC wall-clock once NTP has synced
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    gmtime_r(&now, &t);
    strftime(ts, sizeof(ts), "%H:%M:%SZ", &t);
  }
  Serial.printf("[%9lu][%s][%c][%-5s] %s\n", (unsigned long)millis(), ts, lvl, tag, msg);
}

#define LOGE(tag, ...) do { if (LOG_LEVEL >= 1) logPrint('E', tag, __VA_ARGS__); } while (0)
#define LOGW(tag, ...) do { if (LOG_LEVEL >= 2) logPrint('W', tag, __VA_ARGS__); } while (0)
#define LOGI(tag, ...) do { if (LOG_LEVEL >= 3) logPrint('I', tag, __VA_ARGS__); } while (0)
#define LOGD(tag, ...) do { if (LOG_LEVEL >= 4) logPrint('D', tag, __VA_ARGS__); } while (0)

