#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s.h"
#include "config.h"
#include "logging.h"
#include "dsp.h"
#include "sensors.h"

static SemaphoreHandle_t g_mtx;

// =====================================================================
//  FLOW  (interrupt pulse counter)
// =====================================================================
static volatile uint32_t g_pulses = 0, g_lastIsrUs = 0;

static void IRAM_ATTR flowISR() {
  uint32_t now = (uint32_t)micros();
  if ((uint32_t)(now - g_lastIsrUs) >= FLOW_MIN_PULSE_US) {   // glitch / bounce filter
    g_pulses++;
    g_lastIsrUs = now;
  }
}

FlowData flowSnapshot() {
  static uint32_t lastP = 0, lastMs = 0, lastChangeMs = 0;
  static bool wasActive = true;
  uint32_t nowMs = millis();
  uint32_t p = g_pulses;
  if (lastMs == 0) { lastMs = nowMs; lastChangeMs = nowMs; }

  uint32_t dp = p - lastP;
  uint32_t dt = nowMs - lastMs;
  FlowData d;
  d.pulses = p;
  d.totalL = p / FLOW_PULSES_PER_LITER;
  d.lpm = dt ? (dp / FLOW_PULSES_PER_LITER) * 60000.0f / (float)dt : 0.0f;
  if (dp) lastChangeMs = nowMs;
  d.active = (nowMs - lastChangeMs) < FLOW_IDLE_MS;

  if (wasActive && !d.active) LOGW("flow", "no pulses for %d ms (water off, wrong direction, or sensor wiring)", FLOW_IDLE_MS);
  if (!wasActive && d.active) LOGI("flow", "pulses resumed");
  wasActive = d.active;
  lastP = p; lastMs = nowMs;
  return d;
}

// =====================================================================
//  MPU6050  (hardware FIFO @ 1 kHz, accel only)
// =====================================================================
static float s_ax[3][MPU_WIN];
static float s_re[MPU_WIN], s_im[MPU_WIN], s_pw[MPU_WIN / 2];
static VibData g_vib = {};

static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool mpuRead(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)MPU_ADDR, (size_t)n, true) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool mpuInit() {
  if (!mpuWrite(0x6B, 0x80)) return false;     // device reset
  delay(100);
  mpuWrite(0x6B, 0x01);                        // wake, clock = gyro X PLL
  delay(10);
  uint8_t who = 0;
  if (!mpuRead(0x75, &who, 1)) return false;
  LOGI("mpu", "WHO_AM_I=0x%02X", who);
  mpuWrite(0x1A, 0x00);                        // DLPF_CFG=0: accel BW 260 Hz (widest)
  mpuWrite(0x19, 7);                           // 8 kHz / (1+7) = 1 kHz sample rate
  mpuWrite(0x1C, 0x00);                        // accel full scale +-2 g (16384 LSB/g)
  mpuWrite(0x6A, 0x04);                        // FIFO reset
  delay(5);
  mpuWrite(0x23, 0x08);                        // FIFO: accelerometer only (6 bytes / sample)
  mpuWrite(0x6A, 0x40);                        // FIFO enable
  return true;
}

static void setVibOk(bool ok) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_vib.ok = ok;
  xSemaphoreGive(g_mtx);
}

static void mpuProcess() {
  VibData v = {};
  float pk = 0;
  for (int k = 0; k < MPU_WIN / 2; k++) s_pw[k] = 0;
  for (int a = 0; a < 3; a++) {
    float mean = 0;
    for (int i = 0; i < MPU_WIN; i++) mean += s_ax[a][i];
    mean /= MPU_WIN;
    float ss = 0;
    for (int i = 0; i < MPU_WIN; i++) {
      float x = s_ax[a][i] - mean;                // remove gravity / DC
      s_re[i] = x; s_im[i] = 0;
      ss += x * x;
      if (fabsf(x) > pk) pk = fabsf(x);
    }
    v.rms[a] = sqrtf(ss / MPU_WIN);
    hannWindow(s_re, MPU_WIN);
    fft(s_re, s_im, MPU_WIN);
    for (int k = 1; k < MPU_WIN / 2; k++) s_pw[k] += binPower(s_re[k], s_im[k], MPU_WIN);
  }
  v.peak = pk;

  const float binHz = (float)MPU_FS_HZ / MPU_WIN;               // 1.953 Hz
  const float edges[5] = {5, 50, 150, 300, 500};
  float bp[4] = {0, 0, 0, 0}, best = 0; int bestK = 0;
  for (int k = 1; k < MPU_WIN / 2; k++) {
    float f = k * binHz;
    if (f >= 5 && s_pw[k] > best) { best = s_pw[k]; bestK = k; }
    for (int b = 0; b < 4; b++) if (f >= edges[b] && f < edges[b + 1]) bp[b] += s_pw[k];
  }
  v.domHz = bestK * binHz;
  for (int b = 0; b < 4; b++) v.bands[b] = toDb(bp[b]);

  xSemaphoreTake(g_mtx, portMAX_DELAY);
  v.ok = g_vib.ok;
  v.seq = g_vib.seq + 1;
  g_vib = v;
  xSemaphoreGive(g_mtx);
}

static void mpuTask(void *) {
  int n = 0, failCount = 0;
  uint32_t lastTry = 0;
  bool ok = mpuInit();
  setVibOk(ok);
  if (!ok) LOGE("mpu", "init failed (I2C wiring? address 0x%02X)", MPU_ADDR);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));                // ~20 samples per wake-up
    if (!ok) {
      if (millis() - lastTry > 2000) {
        lastTry = millis();
        ok = mpuInit();
        setVibOk(ok);
        if (ok) { LOGI("mpu", "recovered"); failCount = 0; n = 0; }
      }
      continue;
    }
    uint8_t c[2];
    if (!mpuRead(0x72, c, 2)) {
      if (++failCount >= 5) { ok = false; setVibOk(false); LOGE("mpu", "I2C failure x5 -> sensor marked unhealthy"); }
      continue;
    }
    failCount = 0;
    uint16_t cnt = ((uint16_t)c[0] << 8) | c[1];
    if (cnt >= 1000) {                            // FIFO is 1024 B: overflow, resync
      LOGW("mpu", "FIFO overflow (%u bytes) - resetting", cnt);
      mpuWrite(0x6A, 0x00); mpuWrite(0x6A, 0x04); mpuWrite(0x6A, 0x40);
      continue;
    }
    int samples = cnt / 6;
    while (samples > 0) {
      int chunk = samples > 20 ? 20 : samples;    // 120 B fits in the 128 B Wire buffer
      uint8_t b[120];
      if (!mpuRead(0x74, b, chunk * 6)) { failCount++; break; }
      for (int i = 0; i < chunk; i++) {
        for (int a = 0; a < 3; a++) {
          int16_t raw = (int16_t)(((uint16_t)b[i * 6 + a * 2] << 8) | b[i * 6 + a * 2 + 1]);
          s_ax[a][n] = raw / 16384.0f;
        }
        if (++n == MPU_WIN) { mpuProcess(); n = 0; }
      }
      samples -= chunk;
    }
  }
}

VibData vibSnapshot() {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  VibData v = g_vib;
  xSemaphoreGive(g_mtx);
  return v;
}

// =====================================================================
//  INMP441  (I2S 16 kHz, left channel, 32-bit frames / 24-bit data)
// =====================================================================
static float m_buf[MIC_WIN], m_re[MIC_WIN], m_im[MIC_WIN];
static const float MIC_BAND_EDGES[6] = {100, 500, 1000, 2000, 4000, 8000};
static int specLo[16], specHi[16];

static struct {
  double ms, band[5], spec[16], cNum, cDen;
  int n;
} acc;
static MicData g_mic = {};

static void micInitBins() {
  const float binHz = (float)MIC_FS_HZ / MIC_WIN;
  for (int j = 0; j < 16; j++) {
    float lo = 100.0f * powf(80.0f, j / 16.0f);
    float hi = 100.0f * powf(80.0f, (j + 1) / 16.0f);
    specLo[j] = (int)roundf(lo / binHz);
    specHi[j] = (int)roundf(hi / binHz);
    if (specHi[j] <= specLo[j]) specHi[j] = specLo[j] + 1;
  }
}

static void micProcess() {
  static int flatCount = 0;
  const int N = MIC_WIN;
  float mean = 0;
  for (int i = 0; i < N; i++) mean += m_buf[i];
  mean /= N;
  float ms = 0;
  for (int i = 0; i < N; i++) { float x = m_buf[i] - mean; m_re[i] = x; m_im[i] = 0; ms += x * x; }
  ms /= N;

  if (ms < 1e-10f) {                              // below about -100 dBFS: essentially digital silence
    if (++flatCount == 40) LOGW("mic", "signal is flat (<-100 dBFS) - check L/R pin, SD/WS/SCK wiring, 3V3");
  } else flatCount = 0;

  hannWindow(m_re, N);
  fft(m_re, m_im, N);
  const float binHz = (float)MIC_FS_HZ / N;
  for (int k = 1; k < N / 2; k++) m_re[k] = binPower(m_re[k], m_im[k], N);   // m_re[k] := power

  double band[5] = {0}, cNum = 0, cDen = 0, spec[16] = {0};
  for (int k = 1; k < N / 2; k++) {
    float f = k * binHz, p = m_re[k];
    if (f >= 100 && f < 8000) { cNum += f * p; cDen += p; }
    for (int b = 0; b < 5; b++) if (f >= MIC_BAND_EDGES[b] && f < MIC_BAND_EDGES[b + 1]) band[b] += p;
  }
  for (int j = 0; j < 16; j++) for (int k = specLo[j]; k < specHi[j] && k < N / 2; k++) spec[j] += m_re[k];

  xSemaphoreTake(g_mtx, portMAX_DELAY);
  acc.ms += ms; acc.cNum += cNum; acc.cDen += cDen; acc.n++;
  for (int b = 0; b < 5; b++) acc.band[b] += band[b];
  for (int j = 0; j < 16; j++) acc.spec[j] += spec[j];
  xSemaphoreGive(g_mtx);
}

static void micTask(void *) {
  static int32_t raw[256];
  int n = 0;
  uint32_t timeouts = 0;
  for (;;) {
    size_t got = 0;
    esp_err_t e = i2s_read(I2S_NUM_0, raw, sizeof(raw), &got, pdMS_TO_TICKS(200));
    if (e != ESP_OK || got == 0) {
      if (++timeouts == 5) {
        LOGE("mic", "I2S read timeout x5 - marking unhealthy");
        xSemaphoreTake(g_mtx, portMAX_DELAY); g_mic.ok = false; xSemaphoreGive(g_mtx);
      }
      continue;
    }
    if (timeouts >= 5) {
      LOGI("mic", "I2S recovered");
      xSemaphoreTake(g_mtx, portMAX_DELAY); g_mic.ok = true; xSemaphoreGive(g_mtx);
    }
    timeouts = 0;
    int cnt = got / 4;
    for (int i = 0; i < cnt; i++) {
      m_buf[n++] = (float)(raw[i] >> 8) / 8388608.0f;      // 24-bit signed -> +-1.0
      if (n == MIC_WIN) { micProcess(); n = 0; }
    }
  }
}

MicData micSnapshot() {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (acc.n > 0) {
    double n = acc.n;
    g_mic.db = toDb((float)(acc.ms / n));
    g_mic.centroid = acc.cDen > 0 ? (float)(acc.cNum / acc.cDen) : 0;
    for (int b = 0; b < 5; b++) g_mic.bands[b] = toDb((float)(acc.band[b] / n));
    for (int j = 0; j < 16; j++) g_mic.spec[j] = toDb((float)(acc.spec[j] / n));
    g_mic.seq++;
    memset(&acc, 0, sizeof(acc));
  }
  MicData m = g_mic;
  xSemaphoreGive(g_mtx);
  return m;
}

// =====================================================================
void sensorsBegin() {
  g_mtx = xSemaphoreCreateMutex();
  micInitBins();

  // Flow
  pinMode(PIN_FLOW, INPUT_PULLUP);                // external 10k to 3V3 does the real work
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowISR, FALLING);
  LOGI("flow", "GPIO%d, K=%.1f pulses/L", PIN_FLOW, FLOW_PULSES_PER_LITER);

  // I2C + MPU task
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(20);

  // I2S
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = MIC_FS_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    LOGE("mic", "i2s_driver_install failed");
  } else {
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_I2S_SD;
    i2s_set_pin(I2S_NUM_0, &pins);
    g_mic.ok = true;
  }

  xTaskCreatePinnedToCore(mpuTask, "mpu", 4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(micTask, "mic", 6144, nullptr, 3, nullptr, 1);
  LOGI("sensors", "tasks started (mpu 1 kHz FIFO, mic %d Hz)", MIC_FS_HZ);
}

