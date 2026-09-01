# Contributing

Thank you for your interest in this project.

## Scope

This firmware prioritises **safety of biological incubation** over new features.
Control-loop behaviour, temperature thresholds, and safety modes should only
be changed with careful testing and clear documentation of risk.

## How to contribute

1. Open an issue first describing the problem or proposed change.
2. Fork the repository and create a branch from `main`.
3. Keep changes focused. Prefer small, reviewable commits.
4. Do not commit `secrets.h` or any real credentials.
5. Test on real hardware when possible (temperature control, Safe/Emergency paths, Watchdog).
6. Open a pull request with a clear description of what changed and why.

## Code style

- Prefer static buffers over `String` on critical paths.
- Network I/O must remain non-blocking and must never gate the control loop.
- Document safety-related changes explicitly in the PR.
