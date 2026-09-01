#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <WString.h>
void telegram_send_periodic_report();
void telegram_monitor_loop();
void telegram_set_monitoring(bool enabled);
// ============================================================
// telegram_alerts.h – Phase 4B: Non-blocking Telegram alerts
// Debounced, millis()-driven, never blocks the Watchdog.
// ============================================================
void telegram_init();
void telegram_loop();                 // call from diag_io_loop() or main loop
void telegram_send_fault(uint8_t code, uint8_t level, float value);
void telegram_send_daily_report();    // optional scheduled call
void update_last_reset_reason();
void checkTelegramCommands();
void updateResetStats(); // to handle telegram commands
void fillDetailedResetReport(char* out, size_t n);
void fillFaultsReport(char* out, size_t n);
String getDetailedResetReport();
String getFaultsReport();
void initResetStats();
void saveResetStats();
void resetTelegramBot();
bool tgSend(const char* text); 
