# Windows model check

- Date: 2026-10-30
- Machine: AMD Ryzen 7 9800X3D, Windows 11
- Runtimes: CPU `tools/inference/runtime/windows-x64`; DirectML `windows-x64-directml`
- Model: `assets/model/face_detection_retinaface_mb025.onnx`
- Model SHA-256: `f4c83825f41dba8e17b522377f77cb7e2b69ec4df75168d4cb1b18dcaf03b54c`
- Confidence threshold: 0.50

## Commands

```text
cargo run -q --release -p telaviva-inference --features bench --example occlusion_bench -- --model <file> --family <retinaface|yunet> --runtime <cpu onnxruntime.dll>
cargo test -p telaviva-inference --test ort_yunet_smoke_windows --locked
cargo test -p telaviva-inference --features directml --test directml_provider_windows --locked
```

The DirectML test ran with `TV_TEST_DIRECTML_RUNTIME` set to the DirectML `onnxruntime.dll`.

## Cost

| Model | Latency p50 | Latency p95 | Memory increase on model load (RSS) |
|---|---|---|---|
| RetinaFace MobileNet0.25 | 12.4 ms | 13.8 ms | 17.3 MB |
| YuNet baseline | 4.9 ms | 5.5 ms | 15.9 MB |

Latency ratio, RetinaFace to YuNet: 2.5x.

## Acceptance rule 4

- Latency: within 3x of the YuNet baseline and within 60 ms. Passed (2.5x, 12.4 ms p95 13.8 ms).
- Memory: the absolute 10 MB rule is exceeded by both models (17.3 MB and 15.9 MB). RetinaFace is 1.4 MB above YuNet. This is the same kind of exception as on macOS (33.0 MB against 28.9 MB there), see ADR 0042.

## Recall

At the 0.50 threshold:

| Occluded recall | Unoccluded recall | Face-free false positives |
|---|---|---|
| 19/19 | 10/10 | 1/16 |

The model and preprocessing are the same as on macOS, so the threshold sweep has the same shape as the macOS report in `benchmark-retinaface.md`.

## Smoke test

`ort_yunet_smoke_windows` passed on CPU:

- Dark frame: status 2.
- Neutral frame: absent.
- CC0 portrait: present, confidence at or above 0.5.

## DirectML

`directml_provider_windows` passed:

- Selected provider 2 (DirectML) is available.
- CC0 portrait: status 1, confidence 0.999938.

## Limits

- One machine and one GPU.
- The DirectML result shows that the model runs through DirectML and detects the portrait. It is not a latency measurement.
- The packaged app `--smoke-test` exited 0 on 2026-10-30.
- This check is automated. It is not a lock-screen check and does not verify any lock row in `hardware-checklist.md`.
