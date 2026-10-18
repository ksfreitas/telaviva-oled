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
| `windows-adapter-probe.md` | T002 | pending |
| `windows-device-mapping.md` | T003 | pending |
| `windows-auto-igpu.md` | T023 | pending |
| `macos-ort-spike.md` | T004 | pending (needs a Mac) |
| `macos-coreml.md` | T043 | pending (needs a Mac) |

No record claims an energy saving (spec FR-013); the justification for Automatic is offloading.
