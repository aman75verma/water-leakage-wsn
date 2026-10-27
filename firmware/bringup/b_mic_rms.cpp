#include <Arduino.h>
#include "driver/i2s.h"
#include "config.h"

void setup() {
  Serial.begin(115200);
  delay(500);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = MIC_FS_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;      // L/R pin = GND
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { Serial.println("i2s install FAILED"); while (1) delay(1000); }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_I2S_SD;
  i2s_set_pin(I2S_NUM_0, &pins);
  Serial.println("INMP441 test: tap the board, clap, talk. The level should jump.");
}

void loop() {
  static int32_t raw[256];
  static double sumSq = 0, sum = 0;
  static int n = 0;
  size_t got = 0;
  esp_err_t e = i2s_read(I2S_NUM_0, raw, sizeof(raw), &got, pdMS_TO_TICKS(200));
  if (e != ESP_OK || got == 0) { Serial.println("I2S read TIMEOUT -> check SCK/WS/SD wiring"); return; }
  for (size_t i = 0; i < got / 4; i++) {
    double s = (raw[i] >> 8) / 8388608.0;   // 24-bit signed -> +-1.0
    sum += s; sumSq += s * s; n++;
  }
  if (n >= MIC_FS_HZ / 10) {                // every 100 ms
    double mean = sum / n;
    double rms = sqrt(sumSq / n - mean * mean);
    float db = 20.0f * log10f((float)rms + 1e-9f);
    int bar = constrain((int)((db + 90.0f) * 0.6f), 0, 54);
    char b[64]; memset(b, '#', bar); b[bar] = 0;
    Serial.printf("%7.1f dBFS  dc=%+.5f  %s\n", db, mean, b);
    sum = sumSq = 0; n = 0;
  }
}

