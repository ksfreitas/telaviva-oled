# Windows performance measurements

Measured on 2026-10-17. Machine: Windows 11 Pro build 26300, AMD Ryzen 7 9800X3D (16 logical processors), desktop, one DELL S3225QC over HDMI. Build: Release x64, packaged folder `dist\TelaVivaOLED` (86.5 MB, 279 files).

## Idle cost of the full app (SC-003, partial)

Method: start `TelaViva.exe` (tray only, all protections off, which is the default), wait 20 s, then compare `TotalProcessorTime` over a 60 s window and read the working set.

| Measure | Value |
|---------|-------|
| CPU over 60 s | 0.000 s used, 0.000% of one core (below the timer granularity of `TotalProcessorTime`) |
| Working set | 58.8 MB |
| Private bytes | 58.9 MB |
| Threads, handles | 28, 540 |

Earlier measurement of the shell alone (spike S10): 0.078% of one core and 57.6 MB. The two numbers are consistent: the idle cost is dominated by process baseline memory, and CPU is effectively zero while the app only waits.

Not measured:
- Presence protection on with the camera running (SC-003 second case, T138).
- EcoQoS on versus off (T104) and forced Efficiency mode latencies (T103).
- Energy of any kind. This machine is a desktop; no claim about battery or power draw is made.

## Inference latency

CPU 5.27 ms mean per detection on the fixture, DirectML 2.58 ms and 10.22 ms on the two adapters (energy not measured): see `measurements.md`.
