// predictive_model.cpp
// Phase 6 – Thermal predictive fallback model
// Observation-only until sensors fail; then provides estimate + confidence.

#include "predictive_model.h"
#include "diagnostics.h"

#include <math.h>
#include <string.h>
#include <Arduino.h>

// ============================================================
// External symbols from main sketch
// ============================================================
extern float rawTemperature;
extern float currentTemperature;
extern bool  hasValidTemperature;
extern bool  heaterState;
extern bool  safeMode;
extern bool  emergencyMode;

// We need enterSafeMode & logEvent – declare them
void enterSafeMode(const char* reason);
void logEvent(const char* message);

// ============================================================
// State
// ============================================================
static bool  predictiveMode   = false;
static float lastKnownTemp    = 37.7f;
static unsigned long lastKnownTime = 0;

static float heatRate = 0.05f;   // °C / minute (initial guess)
static float coolRate = 0.03f;   // °C / minute
static float confidence = 100.0f;

// Moving-average of slopes (last 10 samples)
static float slopeHist[10] = {0};
static uint8_t slopeIdx = 0;
static uint8_t slopeCount = 0;
static float lastTempForRate = 0;
static unsigned long lastRateMs = 0;

// ============================================================
// Public API
// ============================================================
void predictive_init() {
    predictiveMode = false;
    confidence = 100.0f;
    lastKnownTemp = 37.7f;
    lastKnownTime = 0;
    heatRate = 0.05f;
    coolRate = 0.03f;
    slopeIdx = 0;
    slopeCount = 0;
    lastTempForRate = 0;
    lastRateMs = 0;
    Serial.println("🧠 Predictive model ready");
}

void predictive_updateRates() {
    // Called only when sensors are valid (from observation path)
    if (!hasValidTemperature) return;

    unsigned long now = millis();
    if (lastRateMs == 0) {
        lastTempForRate = rawTemperature;
        lastRateMs = now;
        return;
    }

    float dtSec = (now - lastRateMs) / 1000.0f;
    if (dtSec < 1.5f) return;   // wait for ~2 s sample

    float dT = rawTemperature - lastTempForRate;
    float slopePerMin = (dT / dtSec) * 60.0f;   // °C / min

    // Store in ring
    slopeHist[slopeIdx] = slopePerMin;
    slopeIdx = (slopeIdx + 1) % 10;
    if (slopeCount < 10) slopeCount++;

    // Moving average
    float sum = 0;
    for (uint8_t i = 0; i < slopeCount; i++) sum += slopeHist[i];
    float avgSlope = sum / slopeCount;

    // Adaptive calibration while sensors healthy
    if (heaterState) {
        // Heating phase – update heatRate slowly
        heatRate = 0.9f * heatRate + 0.1f * fabsf(avgSlope);
        if (heatRate < 0.01f) heatRate = 0.01f;
        if (heatRate > 0.5f)  heatRate = 0.5f;
    } else {
        // Cooling phase
        coolRate = 0.9f * coolRate + 0.1f * fabsf(avgSlope);
        if (coolRate < 0.005f) coolRate = 0.005f;
        if (coolRate > 0.3f)   coolRate = 0.3f;
    }

    lastTempForRate = rawTemperature;
    lastRateMs = now;

    // Keep last known for possible future loss
    lastKnownTemp = rawTemperature;
    lastKnownTime = now;
    confidence = 100.0f;
    predictiveMode = false;
}

float getEstimatedTemperature() {
    if (!predictiveMode) return lastKnownTemp;

    float elapsedMin = (millis() - lastKnownTime) / 60000.0f;
    float estimated;
    if (heaterState) {
        estimated = lastKnownTemp + (heatRate * elapsedMin);
    } else {
        estimated = lastKnownTemp - (coolRate * elapsedMin);
    }
    // Safety clamp – open-loop estimate must stay in physical band
    if (estimated < 20.0f) estimated = 20.0f;
    if (estimated > 42.0f) estimated = 42.0f;
    return estimated;
}

bool isPredictiveModeActive() {
    return predictiveMode;
}

float getPredictiveConfidence() {
    return confidence;
}

void predictive_loop() {
    // Activate on sensor loss
    if (!hasValidTemperature) {
        if (!predictiveMode) {
            predictiveMode = true;
            lastKnownTime = millis();
            // lastKnownTemp already stored by updateRates
            Serial.println("🧠 Predictive mode ACTIVATED (sensor loss)");
            logEvent("Predictive mode activated");
        }

        // Update confidence
        float elapsedMin = (millis() - lastKnownTime) / 60000.0f;
        confidence = 100.0f - (elapsedMin * 0.5f);   // –0.5 % per minute
        if (confidence < 0) confidence = 0;

        float est = getEstimatedTemperature();

        // Safety exit criteria
        if (confidence < 60.0f || est > 39.0f) {
            predictiveMode = false;
            addFault(FAULT_PREDICTIVE_MODEL_FAILED, FAULT_CRITICAL, est);
            // Avoid Safe/Emergency mode conflict
            if (!emergencyMode) {
                enterSafeMode("Predictive model confidence lost or temp too high");
                Serial.println("🧠 Predictive model FAILED → Safe Mode");
            } else {
                Serial.println("🧠 Predictive model FAILED (Emergency active – skip Safe Mode)");
            }
        }
    } else {
        // Sensors recovered – leave predictive mode
        if (predictiveMode) {
            predictiveMode = false;
            confidence = 100.0f;
            Serial.println("🧠 Predictive mode cleared (sensors recovered)");
        }
        // Keep rates updated
        predictive_updateRates();
    }
}