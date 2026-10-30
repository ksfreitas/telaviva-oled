# ADR 0042: Occlusion-robust face detection model

- Status: Accepted for macOS; Windows port and hardware validation pending
- Date: 2026-10-29
- Feature: [Masked face detection and lock on absence](../specs/masked-face-detection-and-lock-on-absence/spec.md)

## Context

YuNet accepts a face only at confidence 0.80. Faces behind a mask, or with a hand over the mouth or nose, often score below that and are reported absent. Dimming on absence then affects a present user, and the lock stage would lock their session. The owner wanted these faces to count as present while keeping CPU, memory and battery cost low. The literature had no numbers for YuNet on masks or hands over the face, so the choice was made by measurement.

A benchmark harness (`crates/telaviva-inference`, feature `bench`, example `occlusion_bench`) was built with 45 images from Wikimedia Commons, all CC0 or public domain: 29 with a face (8 masked, 11 hand or tissue at the mouth or nose, 10 plain, with glasses or tilted) and 16 face-free (a hand alone, a mask alone, objects, and a tarantula on a hand). The acceptance rule was fixed before the measurement (architecture.md, Decision 3).

YuNet at its production threshold 0.80 reaches 14/19 occluded recall, 10/10 unoccluded, 0/16 face-free false positives, with p50 41.7 ms and 28.9 MB after load. At 0.65 it reaches 16/19 with 0/16 false positives, and three masked faces stay missed at any threshold that keeps false positives at zero. RetinaFace MobileNet0.25 at 0.50 reaches 19/19 occluded, 10/10 unoccluded and 1/16 false positives (the tarantula, 0.994; every other face-free image scores 0.39 or less), with p50 62.4 ms and 4.1 MB more memory than YuNet. The full results are in [validation/model-decision.md](../specs/masked-face-detection-and-lock-on-absence/validation/model-decision.md).

## Decision

Replace YuNet with RetinaFace MobileNet0.25 on macOS as the single production detector. The model is `assets/model/face_detection_retinaface_mb025.onnx` (Axelera ONNX export, MIT, 1.7 MB, opset 17, input 640x640), prepared and verified by `tools/inference/prepare.py`, with SHA-256 `f4c83825f41dba8e17b522377f77cb7e2b69ec4df75168d4cb1b18dcaf03b54c`.

The inference crate recognises models by SHA-256 in a known-model table. Each row has its own decoder family, input size, mean (104, 117, 123), confidence threshold (0.50) and NMS IoU (0.4). The C ABI does not change, the light gate still runs first, and outputs are identified by their last dimension. YuNet stays in the table as the benchmark baseline and as the Windows detector until the Windows port. The threshold is not a user setting.

The decision does not meet the acceptance rule in full. It passes rules 1 and 2, fails rule 3 by one false positive, and fails rule 4 on latency by 2.4 ms (62.4 ms against the 60 ms cap). It is adopted as a recorded exception, under the owner's instruction to take the decisions, for three reasons. A missed face is the harmful error: it can dim or lock a present user, while a false presence only leaves the screen lit or the session unlocked. The extra cost is about 20 ms per sample, negligible at the 10 s default cadence and about 4 percentage points of one core at the 0.5 s return cadence. The failed rules are judgement values measured on a small set.

Rule 5 cannot be applied literally to RetinaFace, because no threshold on the grid gives zero face-free false positives. The operating point is therefore 0.50, the highest grid value at which occluded recall is maximal (19/19). The one-step margin of rule 5 would give 0.55 (18/19); the owner keeps 0.50.

## Alternatives

- Keep YuNet with a lower threshold: rejected. Zero false positives give at most 16/19 occluded recall (thresholds 0.60 to 0.70). Lower thresholds add face-free false positives (1/16 at 0.55, 2/16 at 0.30), and three masked faces stay missed at 0.65. Rule 5 selects 0.75 for YuNet, which reaches 15/19 and fails rule 1.
- BlazeFace as a second stage (O3): not built. It is needed only if no single model passed the rules (architecture.md, Decisions 1 and 6).
- SCRFD and YOLO-face (license), CenterFace, UltraFace and CAFACLite (weights license or accuracy): rejected in the design (architecture.md, Decision 1).
- A smaller or dynamic input export (480 or 320): kept as a future optimisation, to be evaluated only if latency fails on target hardware.

## Consequences

The fixture set is small. It has 19 occluded faces, mostly 1918 archive and military photographs and no modern webcam frame, so every figure is indicative with wide intervals and must be re-checked with real webcam captures on hardware. Two masked faces sit near the thresholds: mask-03 (0.508) and mask-13 (0.621). A hairy or round object may be taken for a face, which is a false presence that leaves the screen lit.

RetinaFace is trained on WIDER FACE (CC BY-NC-ND), the same legal caveat as YuNet. The owner decides on it. The model costs about 20 ms more CPU per sample and 4 MB more memory. Windows packaging and the DirectML check are pending, so the Windows build keeps YuNet. The macOS shell loads `face_detection_retinaface_mb025.onnx`.

Implementation details and validation evidence remain in [architecture.md](../specs/masked-face-detection-and-lock-on-absence/architecture.md), [contracts.md](../specs/masked-face-detection-and-lock-on-absence/contracts.md) and [validation/model-decision.md](../specs/masked-face-detection-and-lock-on-absence/validation/model-decision.md).
