# Windows adapter classification probe (plan T002)

- Date: 2026-10-18
- Machine: desktop, AMD Ryzen 7 9800X3D (Radeon integrated graphics), NVIDIA GeForce RTX 4080 SUPER, no NPU, no battery
- OS build: Windows 11 10.0.26300.0
- GPU driver versions: AMD Radeon(TM) Graphics 32.0.21041.1000, NVIDIA 32.0.16.1742
- App build / commit: repository at `032b7d4` plus the uncommitted feature branch; probe is a throwaway C++ program kept outside the repo
- Command or steps: `probe.exe adapters` (DXGI `EnumAdapters1` loop, then DXCore GPU adapter list with `IsIntegrated`, `IsHardware`, `InstanceLuid`, then a LUID comparison)

## Result

| DXGI index | Adapter | Vendor / device | DXGI LUID | Dedicated MB | DXCore LUID match | DXCore `IsIntegrated` |
|---|---|---|---|---|---|---|
| 0 | NVIDIA GeForce RTX 4080 SUPER | 0x10DE / 0x2702 | 0x000139AE | 16045 | yes | no |
| 1 | AMD Radeon(TM) Graphics | 0x1002 / 0x13C0 | 0x0001A6B8 | 2021 | yes | **yes** |
| 2 | Microsoft Basic Render Driver | 0x1414 / 0x008C | 0x0001A667 | 0 | yes | no (`IsHardware` no) |

- DXCore `InstanceLuid` equals `DXGI_ADAPTER_DESC1.AdapterLuid` for all three adapters on this machine. This confirms the hypothesis in architecture.md Decision 3 here.
- `IsIntegrated` classifies the Radeon as integrated even though it reports 2 GB of dedicated memory, so a dedicated-memory heuristic would have been wrong or fragile.
- The NPU adapter list was not queried: the Windows SDK in use does not define `DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU` (not needed, NPU is out of scope).

## Not checked

Hybrid laptops, eGPUs, Intel iGPUs and Intel Arc cards, other AMD APUs, and Windows builds other than 26300. The LUID equality is observed on this one machine; classification still falls back to `unknown` (Automatic uses the CPU) when no LUID matches.
