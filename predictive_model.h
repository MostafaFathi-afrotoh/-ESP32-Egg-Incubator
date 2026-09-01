#pragma once
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// predictive_model.h – Phase 6: Thermal predictive fallback
// Read-only observation of rates; activates only on sensor loss.
// ============================================================

#define FAULT_PREDICTIVE_MODEL_FAILED  0x20

void predictive_init();
void predictive_updateRates();        // call from diag_sample path (observation)
void predictive_loop();               // call every ~100 ms
float getEstimatedTemperature();
bool  isPredictiveModeActive();
float getPredictiveConfidence();