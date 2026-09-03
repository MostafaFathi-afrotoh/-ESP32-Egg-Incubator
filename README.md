# 🥚 ESP32 Smart Egg Incubator — Firmware v2.0.1

**A safety-critical embedded control system for precision egg incubation**, built on the ESP32, featuring closed-loop thermal regulation, a finite-state safety supervisor, predictive sensor-loss fallback, self-learning cycle diagnostics, and remote monitoring via Telegram and Adafruit IO.

This firmware was designed with the same rigor expected of a medical-grade embedded controller: deterministic state transitions, hardware-level fail-safes that cannot be overridden by software faults, watchdog-protected execution, and a quantitative system health score computed from live process data.

---

## 📌 Table of Contents

- [Overview](#-overview)
- [System Architecture](#-system-architecture)
- [Hardware](#-hardware)
- [Core Control Logic](#-core-control-logic)
- [Safety Architecture — Finite State Machine](#-safety-architecture--finite-state-machine)
- [Diagnostics & Health Score Engine](#-diagnostics--health-score-engine)
- [Predictive Thermal Fallback Model](#-predictive-thermal-fallback-model)
- [Cycle Learning & Archive](#-cycle-learning--archive)
- [Remote Monitoring (Telegram Bot)](#-remote-monitoring-telegram-bot)
- [Cloud Telemetry (Adafruit IO)](#-cloud-telemetry-adafruit-io)
- [Repository Structure](#-repository-structure)
- [Getting Started](#-getting-started)
- [Serial Command Reference](#-serial-command-reference)
- [Design Notes & Engineering Decisions](#-design-notes--engineering-decisions)
- [Roadmap](#-roadmap)

---

## 🔬 Overview

The controller regulates temperature, humidity, and egg rotation inside an incubation chamber over a 24‑hour operating cycle, while continuously self-monitoring for sensor faults, actuator faults, and thermal excursions. Every subsystem is designed to degrade gracefully: losing a temperature sensor does not stop the process — it hands control to a predictive thermal model; losing WiFi does not stop the process — it queues locally and reconnects with exponential backoff; a stuck relay does not go unnoticed — it is detected and logged as a fault.

**Key engineering goals:**

| Goal | Implementation |
|---|---|
| Never let a software fault leave the heater on | Hardware-level fallback check runs every loop, independent of state-machine logic |
| Never lose the process on a single sensor failure | Predictive thermal model with confidence decay + safe-mode entry |
| Make performance drift observable, not silent | 6-factor weighted Health Score + fault ring buffer |
| Detect degradation *before* it becomes failure | Linear-regression forecasting across the last 10 cycles |
| Stay controllable without a laptop attached | Telegram bot with 11 diagnostic/report commands |
| Survive real network conditions | Non-blocking WiFi with soft-reconnect and exponential backoff |
| Survive ESP32 Arduino-core 3.x watchdog quirks | Single-registration WDT reconfiguration (see [Design Notes](#-design-notes--engineering-decisions)) |

---

## 🏗 System Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                         SENSING LAYER                                  │
│  DS18B20 ×2 (OneWire, GPIO4)      DHT22 (GPIO15, humidity + temp)      │
└───────────────────────┬──────────────────────────────────────────────┘
                         │  EMA-smoothed readings
┌────────────────────────▼──────────────────────────────────────────────┐
│                    SAFETY-CRITICAL SUPERVISOR (ESP32)                  │
│   Finite State Machine:  BOOT → NORMAL ⇄ SAFE ⇄ EMERGENCY              │
│   Hysteresis heater control · NVS-persisted config · 30 s watchdog     │
└───┬─────────────┬─────────────┬─────────────┬─────────────┬───────────┘
    │              │             │             │             │
┌───▼───┐     ┌────▼───┐   ┌─────▼────┐  ┌─────▼─────┐  ┌────▼────────┐
│ Fan   │     │ Turner │   │  Heater  │  │Evaporator │  │  16×2 I2C   │
│(GPIO13)│    │(GPIO14)│   │ (GPIO27) │  │ (GPIO12)  │  │ LCD (0x27)  │
└───────┘     └────────┘   └──────────┘  └───────────┘  └─────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                     DIAGNOSTIC & INTELLIGENCE LAYER                   │
│  Health Score Engine (6 weighted factors) · Fault Ring Buffer (32)    │
│  Predictive Thermal Model (sensor-loss fallback + confidence decay)   │
│  Cycle Archive (last 10 cycles, NVS) · Linear-regression forecasting  │
└───────────────────────┬────────────────────────────────────────────┘
                         │
┌────────────────────────▼──────────────────────────────────────────────┐
│                    CONNECTIVITY LAYER (non-blocking)                   │
│  WiFi (soft-reconnect, exponential backoff, internet-loss detection)   │
│  → Adafruit IO (5 telemetry feeds)   → Telegram Bot (alerts + 11 cmds) │
└──────────────────────────────────────────────────────────────────────┘
```

*A high-resolution version of this architecture, `docs/architecture.svg`, is included in the repository and rendered below.*

![ESP32 Egg Incubator Architecture](docs/architecture.svg)

---

## ⚡ Hardware

| Subsystem | Component | Pin (ESP32) | Notes |
|---|---|---|---|
| Primary temperature | 2× DS18B20 (Dallas OneWire) | GPIO4 | Shared OneWire bus, redundant sensing |
| Humidity + secondary temp | DHT22 | GPIO15 | 60 s auto-retry on read failure |
| Ventilation | Fan | GPIO13 | Always-on fail-safe actuator |
| Egg rotation | Turner motor/relay | GPIO14 | 60 s off / 15 s on cycle |
| Heating element | Heater relay | GPIO27 | Hysteresis control, 30 s max continuous runtime |
| Humidification | Evaporator relay | GPIO12 | Timed safe-mode cycling available |
| Fault indicator | Sensor-fail LED | GPIO5 | Lit when all DS18B20 sensors are invalid |
| Local display | 16×2 LCD | I2C (0x27) | Auto-reinitializes if the bus drops |
| Controller | ESP32 (WiFi/BLE SoC) | — | Arduino-ESP32 core 3.x |

---

## 🌡 Core Control Logic

- **Target temperature:** 37.7 °C, with a hysteresis band of 37.4–37.8 °C (all values are runtime-configurable and persisted in NVS).
- **Critical threshold:** 39.5 °C — heater is force-cut.
- **Emergency threshold:** 39.8 °C — system enters `EMERGENCY` state.
- **Signal conditioning:** raw DS18B20 readings are smoothed with an exponential moving average (`α = 0.25`) before being used for control decisions; the *raw* value is still checked independently against the emergency threshold so smoothing can never mask a real excursion.
- **Egg turning:** 60 s rest / 15 s active turn cycle, suspended automatically while the predictive fallback model is active or the system is outside `NORMAL` state.
- **Humidification:** active in `NORMAL` and `SAFE`, force-disabled (hardware + software) in `EMERGENCY`.
- **24-hour cycle timer** with automatic archival and forecasting on completion.

---

## 🛡 Safety Architecture — Finite State Machine

```
        ┌────────┐
        │  BOOT  │
        └───┬────┘
            │
   ┌────────▼────────┐        sensor loss / fault        ┌────────┐
   │      NORMAL      │ ───────────────────────────────▶ │  SAFE  │
   │ full closed-loop  │ ◀─────────────────────────────── │        │
   │ control active     │        conditions cleared        └───┬────┘
   └────────┬───────────┘                                      │
            │                     T ≥ 39.8 °C                  │ T ≥ 39.8 °C
            └───────────────────────┬──────────────────────────┘
                                     ▼
                          ┌─────────────────────┐
                          │      EMERGENCY       │
                          │ heater hard-cut ·     │
                          │ evaporator hard-cut · │
                          │ turner forced off      │
                          └──────────┬────────────┘
                                     │ 10 s stable-temperature dead-band
                                     ▼
                          returns to SAFE or NORMAL
```

Two independent layers enforce safety at every loop iteration, not just at state transitions:

1. **State-machine layer** — gates which control routines (heater PID/hysteresis, turner, evaporator) are permitted to run.
2. **Hardware fallback layer** — on *every single loop*, regardless of state-machine bookkeeping, the heater output pin is force-driven `LOW` if `currentState == EMERGENCY || emergencyMode`. This closes the timing window where a software fault could otherwise leave a heating element energized.

Additional protections:
- **Relay-stuck detection** — compares commanded actuator state against expected thermal response to flag a heater relay that is welded on or off.
- **10-second stability dead-band** before exiting `EMERGENCY`, preventing oscillation around the emergency threshold.
- **Fan is a fail-safe actuator** — forced on in every state as a passive cooling/venting measure.
- **30-second hardware watchdog** — reconfigured once at boot (see [Design Notes](#-design-notes--engineering-decisions)) with periodic resets around every blocking or network operation.

---

## 📊 Diagnostics & Health Score Engine

Every diagnostic cycle, the firmware computes a **0–100 Health Score** from six independently weighted sub-scores:

| Factor | Penalizes |
|---|---|
| **Thermal** | Deviation from setpoint, temperature spread, time spent outside the ideal band |
| **Overshoot** | Average and 95th-percentile temperature overshoot |
| **Sensor** | Inter-sensor delta (disagreement between DS18B20 units), disconnect events |
| **Heater** | Excessive switching frequency (cycles/hour), runaway cycling |
| **Software** | Unexpected resets, watchdog resets, brownout resets |
| **Actuator** | Abnormal switching rate on fan/heater relays |

A rolling **fault ring buffer** (32 entries) logs every abnormal event with code, severity (`INFO` / `WARNING` / `CRITICAL` / `EMERGENCY`), timestamp, and the triggering value — queryable locally over Serial or remotely over Telegram.

<details>
<summary>Fault code reference</summary>

| Code | Meaning |
|---|---|
| `0x01` | Sensor disconnect |
| `0x02` | Sensor delta too high (inter-sensor disagreement) |
| `0x05` | High overshoot |
| `0x08` | Emergency temperature |
| `0x0B` | Watchdog reset |
| `0x0C` | Brownout reset |
| `0x10` | Safe-mode entered |
| `0x20` | Predictive model failure (confidence lost / estimate out of band) |
| `0x21` | Performance degraded (cycle-over-cycle health drop) |

</details>

---

## 🧠 Predictive Thermal Fallback Model

When **all** temperature sensors are invalid, the system does not simply stall or blindly keep the heater on a timer — it switches to a physics-informed estimator:

1. While sensors are healthy, the model continuously learns the chamber's **heating rate** and **cooling rate** (°C/min) from observed slope data, using a 10-sample moving average.
2. On sensor loss, it projects temperature forward from the last known good reading using the currently active rate (heating or cooling, based on heater state).
3. **Confidence decays linearly** at 0.5 %/minute from the moment of sensor loss.
4. The estimate is clamped to a physically plausible band (20–42 °C) so it cannot itself become a fault source.
5. **Automatic safety exit:** if confidence drops below 60 % or the estimate exceeds 39 °C, the model self-disqualifies, logs `FAULT_PREDICTIVE_MODEL_FAILED (0x20)`, and the system enters `SAFE` mode (or stays in `EMERGENCY` if already there) rather than continuing to operate on an estimate it no longer trusts.

This means a temporary sensor glitch does not abort the incubation cycle, but a *prolonged* sensor outage cannot silently degrade into an uncontrolled thermal condition.

---

## 📚 Cycle Learning & Archive

At the end of each incubation cycle, a compact record (`cycleNum`, health score, average temperature, duty cycle, overshoot, timestamp) is written to a circular NVS-backed archive of the **last 10 cycles**.

- **Automatic regression alert:** if the current cycle's health score falls more than 10 points below the average of the previous three, a `FAULT_PERFORMANCE_DEGRADED (0x21)` warning is raised automatically — surfacing slow hardware drift (e.g., a heater element weakening, sensor calibration drift) that would otherwise be invisible cycle-to-cycle.
- **Forecasting:** a linear regression is fit across archived health scores to project when the score will cross a health = 70 threshold, reported with an approximate 95 % confidence interval (via residual standard error), accessible through the `FORECAST` serial command and the Telegram `/cycle` report.

---

## 📲 Remote Monitoring (Telegram Bot)

A non-blocking Telegram integration provides push alerts and a full pull-based command interface, so the incubator can be supervised without physical or Serial access.

**Push alerts:** every fault above `INFO` severity, plus an optional periodic status report.

**Pull commands:**

| Command | Returns |
|---|---|
| `/status` | Current state, temperature, humidity, actuator summary |
| `/temps` | Detailed per-sensor temperature and humidity readings |
| `/actuators` | Live state of fan, turner, heater, evaporator |
| `/cycle` | Cycle progress, duty cycle, overshoot, forecast |
| `/health` | Current 0–100 health score |
| `/diagnose` | Full diagnostic report (health + thermal + faults) |
| `/sysinfo` | Firmware version, uptime, memory, reset stats |
| `/resetreason` | Cause of the last reboot (WDT / brownout / power-on / etc.) |
| `/faults` | Recent entries from the fault ring buffer |
| `/monitor` | Toggle automatic periodic reporting on/off |
| `/help` | Full command list |

---

## ☁️ Cloud Telemetry (Adafruit IO)

WiFi connectivity is fully non-blocking, with soft (netif-preserving) reconnects to avoid the ESP32 core's known `wifi_init_default: netstack cb reg failed` regression on repeated hard `disconnect()`/`begin()` cycles, exponential backoff (1 min → 1 hour cap) on repeated failures, and active internet-reachability verification separate from link-layer WiFi status.

Five feeds are published at two cadences to respect API rate limits:

| Feed | Rate | Data |
|---|---|---|
| `health-score` | 5 min | Composite health score |
| `temperature-avg` | 5 min | Current or predictive-model temperature |
| `overshoot-p95` | 30 min | 95th-percentile overshoot |
| `sensor-delta` | 30 min | Inter-sensor disagreement |
| `duty-cycle` | 30 min | Heater duty cycle |

A compact JSON payload builder (`buildJsonPayload()`) is also available for lightweight local export or custom endpoints.

---

## 📁 Repository Structure

```
.
├── Memory10_2_4.ino          # Main sketch: sensors, actuators, FSM, control loop
├── diagnostics.cpp / .h      # Fault ring buffer, health score, cycle statistics
├── predictive_model.cpp / .h # Thermal fallback estimator (Phase 6)
├── cycle_archive.cpp / .h    # 10-cycle NVS archive + degradation forecasting (Phase 7)
├── diag_io.cpp / .h          # Non-blocking WiFi, Adafruit IO publishing, JSON export
├── telegram_alerts.cpp / .h  # Telegram bot: alerts + 11 diagnostic commands
├── build_opt.h               # Core-dump / crash-diagnostics build flags
├── secrets.h.example         # Template for WiFi / Adafruit IO / Telegram credentials
└── docs/
    └── architecture.svg      # System architecture diagram
```

---

## 🚀 Getting Started

1. **Install the Arduino-ESP32 core** (3.x) via Boards Manager.
2. **Install required libraries:** `DHT sensor library`, `OneWire`, `DallasTemperature`, `LiquidCrystal_I2C`, `Preferences` (bundled), `HTTPClient` (bundled).
3. **Clone the repository** and open `Memory10_2_4.ino` — keep every `.cpp`/`.h` file in the same sketch folder.
4. **Configure credentials:** copy `secrets.h.example` → `secrets.h` and fill in your WiFi SSID/password, Adafruit IO username/key, and Telegram bot token — **never commit `secrets.h`**.
5. **Wire the hardware** per the [Hardware](#-hardware) table.
6. **Flash and open the Serial monitor at 115200 baud.** On boot you should see:
   ```
   WDT: config=ESP_OK add=ESP_OK timeout=30s
   ```
   with no `task not found` or `TWDT already initialized` errors, and no ~71-second reboot loop.

---

## 🖥 Serial Command Reference

| Command | Action |
|---|---|
| `RESET` | Software reset with logged cause |
| `FORECAST` | Print health-score trend projection |
| `DIAG` | Full diagnostic report |
| `REPORT` | Full detailed report |

*(See `telegram_alerts.cpp`/`diagnostics.cpp` for the authoritative, complete list as the firmware evolves.)*

---

## 🛠 Design Notes & Engineering Decisions

- **Watchdog reconfiguration (ESP32 Arduino core 3.x):** the Task WDT API changed from a single `init()` call to `esp_task_wdt_reconfigure()` + `esp_task_wdt_add(NULL)`. Calling `init()` twice, or registering `syncTime()`'s task before completing this reconfiguration, previously produced a `task not found` error and a hard reset roughly every 71 seconds. The fix performs the reconfigure/add sequence exactly once at the very top of `setup()`, before any other task registration.
- **`httpPostFeed()` avoids the Arduino `String` class** for the HTTP body and URL, using fixed-size `char` buffers with `snprintf` instead — eliminating heap fragmentation risk on a long-running (24 h+) embedded process.
- **Boot-time Telegram notification** is gated on a *confirmed* WiFi connection (`wifiConnected == true`), not merely on `WiFi.begin()` having been called, to avoid silently swallowing the first boot alert.
- **All cloud/telemetry code is strictly observational** — `diag_io` and `telegram_alerts` never write to control variables; they only read state and publish it. This keeps the safety-critical control path free of network-dependent side effects.

---

## 🗺 Roadmap

- [ ] PID (vs. hysteresis) heater control option
- [ ] OTA firmware updates
- [ ] Web dashboard (local, no cloud dependency)
- [ ] Configurable species incubation profiles (chicken / duck / quail presets)
- [ ] Multi-chamber support

---

## 👤 Author

**Mostafa Fathy** — Embedded Systems & Firmware Developer
Designed and built as an independent embedded-systems project applying closed-loop control, fault-tolerant design, and predictive diagnostics to a real physical process.
