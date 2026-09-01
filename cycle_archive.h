#pragma once
#include <stdint.h>
#include <stdbool.h>

// ============================================================
// cycle_archive.h – Phase 7: Cycle learning & archive
// Stores last 10 completed cycles in NVS for comparison.
// ============================================================

#define FAULT_PERFORMANCE_DEGRADED  0x21

void cycle_archive_init();
void cycle_archive_onCycleComplete(uint32_t cycleNum, float healthScore,
                                   float avgTemp, float duty, float overshoot);
void cycle_archive_compare();         // call after a cycle finishes
void cycle_archive_printForecast();   // Serial command FORECAST support

// Accessors
uint8_t cycle_archive_count();
float   cycle_archive_avgHealthLastN(uint8_t n);