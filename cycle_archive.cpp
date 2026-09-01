// cycle_archive.cpp
// Phase 7 – Cycle learning & archive (last 10 cycles in NVS)

#include "cycle_archive.h"
#include "diagnostics.h"

#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <Arduino.h>

// ============================================================
// Archive record (packed)
// ============================================================
#pragma pack(push, 1)
struct CycleRec {
    uint32_t cycleNum;
    float    health;
    float    avgTemp;
    float    duty;
    float    overshoot;
    uint32_t timestamp;   // millis at completion
};
#pragma pack(pop)

static const uint8_t MAX_CYCLES = 10;
static CycleRec archive[MAX_CYCLES];
static uint8_t  archCount = 0;
static uint8_t  archHead  = 0;   // next write index

static Preferences prefArch;

// ============================================================
// Persistence
// ============================================================
static void saveArchive() {
    prefArch.putBytes("cyc_arch", archive, sizeof(archive));
    prefArch.putUChar("cyc_cnt", archCount);
    prefArch.putUChar("cyc_head", archHead);
}

static void loadArchive() {
    size_t len = prefArch.getBytesLength("cyc_arch");
    if (len == sizeof(archive)) {
        prefArch.getBytes("cyc_arch", archive, sizeof(archive));
        archCount = prefArch.getUChar("cyc_cnt", 0);
        archHead  = prefArch.getUChar("cyc_head", 0);
        if (archCount > MAX_CYCLES) archCount = MAX_CYCLES;
        if (archHead  >= MAX_CYCLES) archHead = 0;
    } else {
        memset(archive, 0, sizeof(archive));
        archCount = 0;
        archHead  = 0;
    }
}

// ============================================================
// Public API
// ============================================================
void cycle_archive_init() {
    prefArch.begin("cyc_arch", false);
    loadArchive();
    Serial.printf("📚 Cycle archive loaded: %u records\n", archCount);
}

void cycle_archive_onCycleComplete(uint32_t cycleNum, float healthScore,
                                   float avgTemp, float duty, float overshoot) {
    CycleRec& r = archive[archHead];
    r.cycleNum  = cycleNum;
    r.health    = healthScore;
    r.avgTemp   = avgTemp;
    r.duty      = duty;
    r.overshoot = overshoot;
    r.timestamp = millis();

    archHead = (archHead + 1) % MAX_CYCLES;
    if (archCount < MAX_CYCLES) archCount++;

    saveArchive();
    Serial.printf("📚 Cycle #%lu archived (health=%.1f)\n", cycleNum, healthScore);

    // Immediate comparison
    cycle_archive_compare();
}

void cycle_archive_compare() {
    if (archCount < 4) return;   // need current + 3 previous

    // Current = most recently written (head-1)
    uint8_t curIdx = (archHead + MAX_CYCLES - 1) % MAX_CYCLES;
    float curHealth = archive[curIdx].health;

    // Average of previous 3
    float sum = 0;
    for (int i = 1; i <= 3; i++) {
        uint8_t idx = (curIdx + MAX_CYCLES - i) % MAX_CYCLES;
        sum += archive[idx].health;
    }
    float avgPrev = sum / 3.0f;

    if ((avgPrev - curHealth) > 10.0f) {
        addFault(FAULT_PERFORMANCE_DEGRADED, FAULT_WARNING, curHealth);
        Serial.printf("⚠️ Performance degraded: current %.1f vs prev avg %.1f\n",
                      curHealth, avgPrev);
    }
}

uint8_t cycle_archive_count() {
    return archCount;
}

float cycle_archive_avgHealthLastN(uint8_t n) {
    if (archCount == 0 || n == 0) return 0;
    if (n > archCount) n = archCount;

    float sum = 0;
    uint8_t idx = (archHead + MAX_CYCLES - 1) % MAX_CYCLES;
    for (uint8_t i = 0; i < n; i++) {
        sum += archive[idx].health;
        idx = (idx + MAX_CYCLES - 1) % MAX_CYCLES;
    }
    return sum / n;
}

void cycle_archive_printForecast() {
    if (archCount < 3) {
        Serial.println("📈 FORECAST: insufficient cycle data (need ≥3)");
        return;
    }

    // Simple linear regression on health vs cycle index
    int n = archCount;
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    uint8_t idx = (archHead + MAX_CYCLES - n) % MAX_CYCLES;
    for (int i = 0; i < n; i++) {
        float x = i;
        float y = archive[idx].health;
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
        idx = (idx + 1) % MAX_CYCLES;
    }

    float denom = n * sumX2 - sumX * sumX;
    if (fabsf(denom) < 1e-6f) {
        Serial.println("📈 FORECAST: slope undefined");
        return;
    }
    float slope = (n * sumXY - sumX * sumY) / denom;
    float intercept = (sumY - slope * sumX) / n;

    // Project when health reaches 70
    if (slope >= -0.01f) {
        Serial.println("📈 FORECAST: no significant degradation trend");
        return;
    }

    float x70 = (70.0f - intercept) / slope;
    int cyclesLeft = (int)(x70 - (n - 1));
    if (cyclesLeft < 0) cyclesLeft = 0;

    // Residual std-dev → approximate 95% CI on projected cycle count
    float sumRes2 = 0.0f;
    idx = (archHead + MAX_CYCLES - n) % MAX_CYCLES;
    for (int i = 0; i < n; i++) {
        float yHat = intercept + slope * (float)i;
        float res  = archive[idx].health - yHat;
        sumRes2   += res * res;
        idx = (idx + 1) % MAX_CYCLES;
    }
    float stdErr = (n > 2) ? sqrtf(sumRes2 / (float)(n - 2)) : 0.0f;
    float ciHalf = (fabsf(slope) > 1e-6f) ? (1.96f * stdErr / fabsf(slope)) : 0.0f;

    Serial.printf("📈 FORECAST: health may reach 70 in ~%d cycles "
                  "(slope=%.3f, 95%% CI ±%.1f cycles)\n",
                  cyclesLeft, slope, ciHalf);
}