#pragma once
#include <Arduino.h>

struct FlowData { float lpm; float totalL; uint32_t pulses; bool active; };
struct VibData  { float rms[3]; float peak; float domHz; float bands[4]; bool ok; uint32_t seq; };
struct MicData  { float db; float centroid; float bands[5]; float spec[16]; bool ok; uint32_t seq; };

void     sensorsBegin();
FlowData flowSnapshot();   // flow averaged since the previous call
VibData  vibSnapshot();    // latest completed 512-sample window
MicData  micSnapshot();    // power-average of all windows since the previous call

