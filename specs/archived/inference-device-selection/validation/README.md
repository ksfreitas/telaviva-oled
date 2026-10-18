# Validation records

Evidence for the inference device selection feature. Each record is one markdown file in this folder and states only what was observed on the machine named in it. Code presence is not evidence of hardware support.

## Record template

```
# <what was checked>

- Date:
- Machine: <model, CPU, GPUs (iGPU / dGPU), NPU if any, RAM>
- OS build:
- GPU driver versions:
- App build / commit:
- Command or steps:

## Result

<observed output, numbers with units, pass / partial / fail per item>

## Not checked

<what this record does not cover, for example energy>
```

## Records

| File | Plan task | Status |
|---|---|---|
| `windows-adapter-probe.md` | T002 | passed on recorded desktop |
| `windows-device-mapping.md` | T003 | passed on recorded desktop |
| `windows-auto-igpu.md` | T023 | partial; remaining acceptance limits recorded |
| `macos-ort-spike.md` | T004/T005 | passed with ORT 1.23.0 after diagnosing 1.22.0 shutdown abort |
| `macos-coreml.md` | T043 | passed on M4 Pro; Automatic enabled |
| `macos-coreml-device-probe.md` | T035/T043 | production C ABI correctness/latency passed |
| `macos-validation.md` | T042/T050 | integrated check results and acceptance limits |

No record claims an energy saving (spec FR-013); the justification for Automatic is offloading.
