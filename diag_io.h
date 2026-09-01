#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pull Phase 6 & 7 public APIs so a single #include in the main sketch is enough
#include "predictive_model.h"
#include "cycle_archive.h"
#include "telegram_alerts.h"

// ============================================================
// diag_io.h – Phase 4A: Non-blocking cloud I/O + JSON export
// Observational + publish only. Never alters control logic.
// ============================================================

void diag_io_init();
void diag_io_loop();          // call every ~100 ms from loop()
void diag_io_sync();          // optional: force local actuator state publish

// Lightweight JSON payload builder (Phase 5)
// Returns pointer to static buffer (valid until next call)
const char* buildJsonPayload();

// Diagnostics counters (visible via DIAG / REPORT)
extern uint32_t wifiReconnectAttempts;
extern bool  wifiConnected;
bool httpPostFeed(const char* feed, float value);
void publishAllFeeds();
