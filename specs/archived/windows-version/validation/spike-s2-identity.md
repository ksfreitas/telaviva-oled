# Spike S2: monitor identity and DDC read on this machine

Date: 2026-10-17. Machine: Windows 11 build 26300, desktop, AMD Radeon iGPU and NVIDIA RTX 4080 SUPER, one external monitor attached.

**Result: go for identity and DDC reads. DDC writes are not tested yet** (task T042 needs the owner present: it changes the real monitor's brightness by one step and restores it).

## Method

Two headless, read-only modes of `TelaViva.exe` (`Host/DisplayDiagnostics.cpp`): `--list-displays <file>` runs the real `Win32DisplayCatalog` (`QueryDisplayConfig`, target and source names, registry EDID, `EnumDisplayMonitors`); `--probe-ddc <file>` opens the physical monitors with the dxva2 API and reads VCP `0x10` (brightness) and `0x62` (volume) through `DdcTransaction::readVcp`. Neither mode ever calls a write. Raw EDIDs are not written to the output.

## Result

| Item | Value |
|------|-------|
| Monitors found | 1 external, no internal panel (desktop) |
| Name / manufacturer | DELL S3225QC, `DEL` |
| Monitor id | `win-DEL-53688-808858963-a9c404a1` |
| Identity | `stable` (unique among attached monitors) |
| EDID size | 384 bytes (three blocks; the id hash uses the 128-byte base block only) |
| Connection | HDMI (`DELL_S3225QC(HDMI)`), `connectionPath` `\\?\DISPLAY#DELD1B8#5&210850f5&0&UID4352#{e6f07b5f-...}` |
| GDI device | `\\.\DISPLAY1`, HMONITOR found |
| Physical monitors for the HMONITOR | 1 |
| VCP `0x10` brightness read | ok, current 35, 59 ms |
| VCP `0x62` volume read | ok, current 1, 71 ms |

Both reads succeeded on the first attempt with no retries. The latency (about 60 to 70 ms per read, retries included) is in the range the research expected for DDC over dxva2.

## What this confirms

- The EDID-based id works without the connector path, which stays in `connectionPath` only.
- Mapping from the GDI source name to the DisplayConfig target and to the physical monitor handle works for a single monitor.
- The read-only DDC path (`Dxva2DdcPort`, `DdcTransaction`, `PhysicalMonitor`) works against real hardware on HDMI.

## Not verified

- DDC writes and the write-then-readback verification (task T042).
- Two identical monitors (the ambiguity rule is covered by unit tests only), several monitors, docks, USB-C and DisplayPort, wake and resume behavior, HDR.
- The maximum value of each VCP code is not reported by the probe (`DdcResult` carries only the current value); the controller keeps the maximum from the last successful read when it builds the discovery payload.
- The same monitor model was read through macOS earlier in the project; this run reports different current values (brightness 35 and volume 1), as expected for a different session and connection.
