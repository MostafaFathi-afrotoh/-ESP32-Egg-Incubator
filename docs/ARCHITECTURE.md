# Software Architecture

## Design principle

**Offline-first, safety-first.**  
The temperature / humidity control loop and safety modes never depend on WiFi,
Telegram, or Adafruit IO. Cloud features are observational and best-effort.

## Modules

| File | Responsibility |
|------|----------------|
| `Memory10_2_4_1.ino` | Main loop, sensors, heater/turner/fan/evaporator, Safe & Emergency modes, serial commands, Watchdog |
| `diagnostics.cpp/.h` | Health score, Welford stats, dual-blob NVS + CRC16, fault ring, degradation tracking |
| `predictive_model.cpp/.h` | Open-loop thermal estimate used only when sensors are lost |
| `cycle_archive.cpp/.h` | Last 10 completed cycles in NVS; comparison and FORECAST |
| `diag_io.cpp/.h` | Non-blocking WiFi, Adafruit IO publish, ties predictive + archive + Telegram |
| `telegram_alerts.cpp/.h` | Debounced Telegram alerts and optional command handling |
| `build_opt.h` | Core-dump related build flags |
| `secrets.h` | Local credentials only (never committed) |

## Control vs. observation

- **Control path:** sensor read → hysteresis / safety decisions → actuator GPIO. Runs regardless of network state.
- **Observation path:** diagnostics sampling, predictive model updates, cycle archive, cloud publish. Must not block the control path.

## Safety layers (summary)

1. Hysteresis band around target temperature  
2. Critical / Emergency temperature cutoffs  
3. Relay stuck-on detection → Safe Mode  
4. Sensor-loss → predictive fallback, then Safe Mode if confidence collapses  
5. Task Watchdog  
6. Dual-blob diagnostic storage with CRC16 for recovery after reboot  

Exact thresholds and behaviour are defined in the main sketch and `diagnostics.h`.
