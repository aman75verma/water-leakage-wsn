#include <Arduino.h>
#include <Wire.h>
#include "config.h"

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)addr, (size_t)n, true) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  Serial.printf("\nI2C scan  SDA=GPIO%d  SCL=GPIO%d\n", PIN_I2C_SDA, PIN_I2C_SCL);
}

void loop() {
  int found = 0;
  uint8_t mpu = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      bool isMpu = (a == 0x68 || a == 0x69);
      Serial.printf("  found 0x%02X%s\n", a, isMpu ? "  <- MPU6050 (expected)" : "");
      if (isMpu) mpu = a;
      found++;
    }
  }
  if (!found) {
    Serial.println("  NOTHING FOUND -> check 3V3/GND, SDA/SCL not swapped, loose jumper");
  } else if (mpu) {
    // wake the chip, then read WHO_AM_I and acceleration
    Wire.beginTransmission(mpu); Wire.write(0x6B); Wire.write(0x00); Wire.endTransmission();
    delay(50);
    uint8_t who = 0, d[6];
    readRegs(mpu, 0x75, &who, 1);
    if (readRegs(mpu, 0x3B, d, 6)) {
      float ax = (int16_t)((d[0] << 8) | d[1]) / 16384.0f;
      float ay = (int16_t)((d[2] << 8) | d[3]) / 16384.0f;
      float az = (int16_t)((d[4] << 8) | d[5]) / 16384.0f;
      Serial.printf("  WHO_AM_I=0x%02X  accel[g] x=%.3f y=%.3f z=%.3f  |a|=%.3f\n",
                    who, ax, ay, az, sqrtf(ax * ax + ay * ay + az * az));
    }
  }
  delay(2000);
}

