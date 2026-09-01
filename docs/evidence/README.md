# Operational Evidence

Screenshots and sample telemetry from live test runs of the ESP32 Egg Incubator (August–September 2026).

## Telegram bot (`telegram/`)

| File | Content |
|------|---------|
| `01_diagnose_report.jpg` | Full diagnostic report (temp, humidity, health, Safe/Emergency) |
| `02_faults_actuators.jpg` | `/faults` and `/actuators` commands |
| `03_diagnose_resetreason.jpg` | Diagnose + reset reason breakdown |
| `04_resetreason_faults.jpg` | Reset statistics including WDT counts |
| `05_status_commands.jpg` | `/status` and related commands |

## Adafruit IO (`adafruit/`)

| File | Content |
|------|---------|
| `01_dashboard_overview.jpg` | Live temperature (~37.17°C) and gauges |
| `02_dashboard_charts.jpg` | Temperature and health-score charts |
| `03_feeds_list.jpg` | Published feeds (temperature-avg, health-score, duty-cycle, …) |

## Sample CSV (`samples/`)

Short excerpts from Adafruit IO exports (not full history):

- `temperature-avg_sample.csv`
- `health-score_sample.csv`
- `duty-cycle_sample.csv`

## Notes

- These are records from a working breadboard prototype, not a finished product.
- Reset statistics may show Watchdog (WDT) events from earlier test iterations; later firmware revisions targeted non-blocking I/O and WDT-safe paths.
- Always treat live thresholds and pin maps in the firmware source as authoritative.
