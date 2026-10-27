#pragma once
#include <math.h>

// In-place iterative radix-2 FFT. n must be a power of two.
inline void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {                 // bit-reversal permutation
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float tr = re[b] * cr - im[b] * ci;
        float ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

inline void hannWindow(float *x, int n) {
  for (int i = 0; i < n; i++) x[i] *= 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (n - 1)));
}

// One-sided mean-square contribution of FFT bin k for a Hann-windowed frame (units: signal^2).
// Hann window power gain = 0.375.  Sum over k ~= mean square of the signal.
inline float binPower(float re, float im, int n) {
  return 2.0f * (re * re + im * im) / ((float)n * (float)n * 0.375f);
}

inline float toDb(float p) { return 10.0f * log10f(p + 1e-12f); }

