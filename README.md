# 🥚 ESP32 Egg Incubator Controller – Professional Version

**Safety-critical, offline-first firmware for egg incubation on ESP32 (Arduino framework).**  
Hysteresis temperature control, multi-layer safety modes, predictive thermal fallback, dual-blob diagnostics, cycle learning, and optional cloud alerts — without ever depending on the network to keep eggs alive.

[![Platform](https://img.shields.io/badge/Platform-ESP32-blue)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/Framework-Arduino-00979D)](https://www.arduino.cc/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-orange)](https://isocpp.org/)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)

---

## 📑 Table of Contents

- [Introduction](#-introduction)
- [Features](#-features)
- [Hardware & Pin Mapping](#-hardware--pin-mapping)
- [System Flow](#-system-flow)
- [Installation & Setup](#-installation--setup)
- [Usage & Serial Commands](#-usage--serial-commands)
- [Software Architecture](#-software-architecture)
- [Customization](#-customization)
- [Important Notes & Warnings](#-important-notes--warnings)
- [Testing](#-testing)
- [Contributing](#-contributing)
- [License](#-license)
- [Acknowledgments](#-acknowledgments)
- [Contact & Support](#-contact--support)

---

## 📖 Introduction

An **egg incubator** must hold temperature near **37.7 °C** for days, turn eggs on a schedule, and manage humidity — while surviving sensor failures, power glitches, and operator mistakes.

This project is a **professional ESP32 controller** built with safety-critical and reliability-engineering practices:

| Problem | How this firmware addresses it |
|--------|--------------------------------|
| Overheating kills embryos | Hard emergency cutoff at **39.8 °C**, critical cutoff at **39.5 °C**, hysteresis control |
| Sensor loss | **Predictive thermal model** estimates temperature; then **Safe Mode** if confidence collapses |
| Silent hardware faults | **Relay stuck-on detection**, fault ring, health score |
| Lost history after reboot | **Dual-blob NVS storage** with **CRC16** |
| Long-term degradation | **Cycle archive** (last 10 cycles) + linear regression **FORECAST** with ~95 % CI |
| Remote monitoring | Optional **Adafruit IO**, **Telegram** (debounced), crash **email** (off by default) |

**Design principle:** *network is optional*. Control, safety, and diagnostics always run offline. Cloud and messaging never block the heater loop.

**Version:** v2.0 (final production-oriented release)  
**Main sketch:** `Memory10_2.ino`

---

## ✨ Features

### ⚙️ Core control

- **Heater** — hysteresis around `TEMP_TARGET` (37.7 °C): ON ≤ 37.2 °C, OFF ≥ 37.8 °C (via `TEMP_LOW` / `TEMP_HIGH`)
- **Turner** — timed cycle (default 60 s OFF / 15 s ON); **frozen** while predictive mode is active
- **Fan** — always ON in normal and safety modes
- **Evaporator** — humidity-based control (≈45–55 %); timed safe cycle if DHT is lost
- **EMA temperature smoothing** on control path; **raw** temperature used for emergency decisions

### 🚨 Safety

- **Safe Mode** — sensors failed or relay stuck; heaters forced OFF; exit only via `RESET` / `EXITSAFE`
- **Emergency Mode** — raw temp ≥ **39.8 °C**; requires ~**10 s** stable below exit threshold before auto-exit
- **Relay stuck-on** — heater software OFF but temp stays high for 30 s → Safe Mode
- **Watchdog (WDT)** — 8 s task WDT with panic; fed in `loop()` and during short waits
- **GPIO absolute cutoff** — actuators driven to safe states on mode entry

### 📊 Diagnostics & intelligence

- **Health Score** (0–100) — weighted thermal, overshoot, sensors, heater, software, actuators
- **Welford** online mean/variance, band time, overshoot histogram / P95, sensor delta
- **Fault ring** (32 events) with severity levels
- **Predictive model** — adaptive heat/cool rates; confidence decay; clamp 20–42 °C; does not enter Safe Mode if Emergency is already active
- **Cycle archive** — last **10** cycles in NVS; duty/overshoot **per cycle** (not lifetime cumulative)
- **Degradation detection** — daily health ring (30 days, circular) + slope vs `DEGRADATION_THRESHOLD`
- **FORECAST** — linear regression to health = 70 with approximate **95 % CI**

### 📡 Monitoring & alerts

- Serial: `REPORT`, `STATUS`, `HEALTH`, `DIAG`, `FAULTS`, `FORECAST`, …
- **Telegram** — multi-code debounce (~1 h), sanitized JSON text, daily health summary
- **Adafruit IO** — rate-limited feeds (health/temp ~5 min; overshoot/delta/duty ~30 min)
- **Email crash report** — disabled by default (`EMAIL_ENABLED 0`); WDT-protected send; mark sent only on success

### 🛡️ Reliability

- **Dual-blob** diagnostics (`diag_blk_a` / `diag_blk_b`) + **CRC16**
- WiFi **exponential backoff** (1 min → up to 1 h); continues offline if WiFi fails
- No `String` on critical paths; static buffers; short network timeouts (HTTP ≤ 2.5 s)
- Core dump flags in `build_opt.h` for post-crash analysis

---

## 🔌 Hardware & Pin Mapping

| Component | GPIO | Type | Description |
|-----------|------|------|-------------|
| DS18B20 (OneWire bus) | **4** | Digital | 2× temperature sensors on one bus |
| DHT22 | **15** | Digital | Humidity + backup temperature |
| Fan | **13** | OUT | Circulation / cooling |
| Turner | **14** | OUT | Egg turner motor |
| Heater | **27** | OUT | Heating element (via relay) |
| Evaporator | **12** | OUT | Humidifier / evaporator |
| LED Sensor Fail | **5** | OUT | All DS18B20 failed |
| LCD 16×2 (I2C) | SDA/SCL | I2C **0x27** | Status display |

**Notes**

- Use suitable **relays** (optocoupled recommended) for heater/turner/fan/evaporator loads.
- DS18B20: 4.7 kΩ pull-up on the data line to 3.3 V.
- Confirm LCD address (`0x27` common; some modules use `0x3F`).

---

## 🔄 System Flow

```text
setup()
  ├─ WDT (8 s)
  ├─ Pins + sensors + LCD
  ├─ Restore NVS (timer, safe/emergency flags)
  ├─ diag_init()          → dual-blob load, fault ring, cycle baseline
  ├─ diag_io_init()       → WiFi attempt, telegram, predictive, cycle archive
  └─ email_reporter_*     → optional crash mail (if EMAIL_ENABLED)

loop()  [WDT reset every pass]
  ├─ Fan always ON
  ├─ Turner cycle (skipped if predictive active)
  ├─ Timer / cycle complete → archive + diag_cycleReset()
  ├─ Serial commands
  ├─ Every ~2 s:
  │     readSensors → controlHeater → controlEvaporator
  │     checkRelayStuck → diag_sample → LCD
  ├─ Every ~10 s:  diag_periodic()
  └─ Every ~100 ms: diag_io_loop()  → WiFi/AIO + Telegram + predictive
```

Control and safety never wait on the network.

---

## 🚀 Installation & Setup

### Prerequisites

- **Board:** ESP32 Dev Module (or compatible)
- **IDE:** Arduino IDE 2.x **or** PlatformIO
- **Libraries** (Library Manager / PlatformIO):

| Library | Typical source |
|---------|----------------|
| DHT sensor library | Adafruit |
| OneWire | Paul Stoffregen |
| DallasTemperature | Miles Burton |
| LiquidCrystal_I2C | (common forks; match your LCD) |
| EMailSender | xreef (only if you enable email) |

Built-in: `WiFi`, `HTTPClient`, `Preferences`, `esp_task_wdt`, `Wire`.

### Steps

1. **Clone / copy** the project into a single Arduino sketch folder.  
   Keep **one** main `.ino` only (`Memory10_2.ino`). Do not mix older `Memory5` / `Memory9` sketches in the same folder.

2. **Secrets**
   ```bash
   cp secrets.h.example secrets.h
   ```
   Edit `secrets.h` with your WiFi, Adafruit IO, Telegram, and optional email values.  
   **Never commit `secrets.h`.** Add it to `.gitignore`.

3. **Wire hardware** according to the pin table above.

4. **Install libraries** listed above.

5. **Board settings:** ESP32 Dev Module, upload speed as needed, serial **115200**.

6. **Upload**, then open **Serial Monitor @ 115200**.

7. Optional: place `build_opt.h` so the build system applies core-dump defines (Arduino: sketch folder / build options as documented for your toolchain).

---

## 💻 Usage & Serial Commands

Type a command and press **Enter** (newline). Commands are case-insensitive where both forms are listed in firmware.

| Command | Description |
|---------|-------------|
| `STATUS` | Live status: temps, heater/turner, Safe/Emergency, cycle |
| `REPORT` | Full reliability report (health, thermal, faults, degradation) |
| `HEALTH` | Short health line |
| `DIAG` | Internal diagnostics counters |
| `FAULTS` | Last fault-ring events |
| `FORECAST` | Project cycles until health ≈ 70 (needs ≥ 3 archived cycles) |
| `RESET` | Reset cycle timer; clear Safe and Emergency |
| `EXITSAFE` | Exit Safe Mode only |
| `EXITEMG` | Exit Emergency Mode only |
| `CLEARDIAG` | Erase diagnostics NVS — then type **`CONFIRM`** within 5 s |
| `RESETDHT` | Re-`begin()` humidity sensor(s) without heap churn |

Example after boot:

```text
Boot #3
System Starting...
✅ System Ready!
📡 diag_io_init: attempting WiFi (max 5 s)...
🧠 Predictive model ready
📚 Cycle archive loaded: 0 records
T1: 37.51 | T2: 37.48 | Avg: 37.5 | ...
```

---

## 🗂️ Software Architecture

| File | Role |
|------|------|
| `Memory10_2.ino` | Main loop, sensors, heater/turner/fan/evaporator, Safe/Emergency, serial |
| `diagnostics.h` / `.cpp` | Health score, Welford, dual-blob, fault ring, day degradation, cycle baselines |
| `diag_io.h` / `.cpp` | Non-blocking WiFi, Adafruit IO, ties telegram + predictive + archive |
| `predictive_model.h` / `.cpp` | Open-loop thermal estimate on sensor loss |
| `cycle_archive.h` / `.cpp` | Last 10 cycles, compare, FORECAST + CI |
| `telegram_alerts.h` / `.cpp` | Debounced Telegram alerts |
| `email_reporter.h` / `.cpp` | Optional crash email (EMailSender) |
| `email_config.h` | Email enable flag and non-secret settings |
| `secrets.h` / `secrets.h.example` | Credentials (local only) |
| `build_opt.h` | Core dump compiler flags |

**Shared structs** (`Actuator`, `HumiditySensor`, `DSTemperatureSensor`) live in **`diagnostics.h`** only.

**Actuator indices** (must stay consistent with REPORT):

```text
0 = Fan, 1 = Turner, 2 = Heater, 3 = Evaporator
```

---

## 🔧 Customization

### Temperature thresholds (`Memory10_2.ino`)

```cpp
#define TEMP_TARGET     37.7
#define TEMP_LOW        (TEMP_TARGET - 0.3)   // heater ON
#define TEMP_HIGH       (TEMP_TARGET + 0.1)   // heater OFF
#define TEMP_CRITICAL   39.5
#define TEMP_EMERGENCY  39.8
```

`TEMP_ABSOLUTE_MAX` is tied to `TEMP_EMERGENCY` (single source).

### Turner timing

```cpp
#define TURNER_OFF_TIME  60000   // ms
#define TURNER_ON_TIME   15000
```

### Health penalties & degradation (`diagnostics.h`)

```cpp
#define PENALTY_MEAN_DEV_HIGH   15.0f
// ... other PENALTY_* ...
#define DEGRADATION_THRESHOLD  (-0.5f)   // health points per day
```

### Email

In `email_config.h`:

```cpp
#define EMAIL_ENABLED  0   // set to 1 only after field SMTP + WDT test
```

Credentials come only from `secrets.h`.

### Adafruit feeds

Create feeds matching the names used in `diag_io.cpp` (e.g. `health-score`, `temperature-avg`, `overshoot-p95`, `sensor-delta`, `duty-cycle`) under your Adafruit IO username.

---

## ⚠️ Important Notes & Warnings

1. **Watchdog** — Do not add long blocking `delay()` in control paths. Existing waits call `esp_task_wdt_reset()`.
2. **Email is OFF by default** — SMTP can block longer than the WDT if the library ignores timeouts. Test on hardware before `EMAIL_ENABLED 1`.
3. **Secrets** — Only in `secrets.h`; never hardcode WiFi/tokens in `.cpp` files.
4. **Safe Mode** does **not** auto-exit after “stable readings”; use `RESET` or `EXITSAFE` (by design).
5. **Offline first** — Missing WiFi does not stop incubation control.
6. **One sketch folder** — Multiple main `.ino` files in one project cause confusing link errors.
7. This firmware is **not** a substitute for proper incubator hardware, insulation, calibration, or biosecurity practices.

---

## 🧪 Testing

| Goal | How |
|------|-----|
| Safe Mode | Disconnect both DS18B20 (or force invalid readings) → heaters OFF, LED on |
| Emergency | Raise sensor reading ≥ 39.8 °C → Emergency; cool and wait ≥ ~10 s stable for auto-exit (or `EXITEMG`) |
| Relay stuck | Simulate high temp with heater software OFF for > 30 s (not in Safe/Emergency) |
| Predictive | Fail DS sensors while rates were previously learned → predictive log; low confidence → Safe (if not Emergency) |
| FORECAST | Complete ≥ 3 full cycles (or inject archive data in lab) then `FORECAST` |
| CLEARDIAG | `CLEARDIAG` then `CONFIRM` within 5 s; verify with `REPORT` |
| Offline | Wrong WiFi credentials → control continues; backoff messages only |
| Email | Keep disabled until intentional crash + SMTP test with WDT observed |

Recommended: **24 h** soak with Serial logging and occasional `REPORT` / `HEALTH`.

---

## 🤝 Contributing

1. Fork / branch from the current v2.0 baseline.
2. Do **not** change heater hysteresis or Emergency thresholds without documenting risk.
3. Prefer static buffers; avoid `String` and heap allocation in `loop` / `diag_sample` / `controlHeater`.
4. Keep actuator index map (0–3) aligned with diagnostics REPORT.
5. Open a PR with: what changed, why, how you tested (Serial commands / hardware scenario).

Optional style notes: bilingual comments are acceptable; public APIs stay in English identifiers.

---

## 📄 License

This project is released under the **MIT License** (or the license file included in the repository, if different).  
You may use, modify, and distribute with attribution. **No warranty** — use at your own risk for biological processes.

---

## 🙏 Acknowledgments

- Espressif ESP32 Arduino core and WDT / NVS APIs  
- Adafruit DHT library  
- OneWire / DallasTemperature communities  
- LiquidCrystal_I2C maintainers  
- EMailSender (xreef) for optional SMTP  
- Everyone iterating on reliability patterns (dual-blob, fault rings, offline-first IoT)

---

## 📬 Contact & Support

- Open an **issue** on the project repository for bugs and feature requests.  
- For private deployment questions, use the contact channel provided by the project maintainer.  
- Security: report credential leaks or unsafe defaults privately when possible.

---

**Keep the network optional. Keep the eggs safe.**
