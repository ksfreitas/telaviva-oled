# Model decision: occlusion-robust face detection

- Feature: [spec](../spec.md)
- Date: 2026-10-29
- Decision record: [ADR 0042](../../../../adr/0042-occlusion-robust-face-detection.md)
- Platform measured: macOS, Apple Silicon (this Mac). Windows was measured on 2026-10-30; see [windows-model-check.md](windows-model-check.md).
- Status: decision recorded. This is a benchmark and a CoreML parity probe, not an acceptance test on hardware.
- Raw outputs: [benchmark-yunet.md](benchmark-yunet.md), [benchmark-retinaface.md](benchmark-retinaface.md)

## Purpose

Choose the single production detector that counts partly covered faces (masks, hand over mouth or nose) as present, without new false presence on face-free frames, within the CPU and memory budget of the design.

Fixtures: 45 images, 29 with a face and 16 face-free. Occluded faces: 19 (8 mask, 11 hand over mouth or nose). Unoccluded faces: 10 (3 plain, 4 glasses, 3 tilted). Face-free: 16 (hand, mask, object, including a tarantula on a hand).

## Threshold sweep (summary)

| Model | Threshold | Occluded recall | Unoccluded recall | Face-free false positives |
|---|---|---|---|---|
| YuNet | 0.30 | 18/19 | 10/10 | 2/16 |
| YuNet | 0.50 | 17/19 | 10/10 | 1/16 |
| YuNet | 0.65 | 16/19 | 10/10 | 0/16 |
| YuNet | 0.80 (production) | 14/19 | 10/10 | 0/16 |
| RetinaFace | 0.30 | 19/19 | 10/10 | 4/16 |
| RetinaFace | 0.50 (chosen) | 19/19 | 10/10 | 1/16 |
| RetinaFace | 0.65 | 17/19 | 10/10 | 1/16 |
| RetinaFace | 0.80 | 17/19 | 10/10 | 1/16 |

RetinaFace has 1/16 false positives from 0.40 to 0.90 (the tarantula on a hand). It has 3/16 at 0.35 and 4/16 at 0.30. The full grid 0.30 to 0.90 and the Wilson 95% intervals are in the raw outputs.

## Per-category recall

RetinaFace at 0.50 (derived from the per-image scores; the raw file computes no category table for this model):

| Category | Faces | Recall |
|---|---|---|
| mask | 8 | 8/8 |
| hand_mouth | 11 | 11/11 |
| none | 3 | 3/3 |
| glasses | 4 | 4/4 |
| tilt | 3 | 3/3 |

YuNet at 0.65 (derived; the raw file's category table is at 0.75, where mask is 4/8 and the other categories are full). Recall cannot rise as the threshold rises, so the other categories stay full at 0.65. The occluded total of 16/19 then gives mask 5/8.

| Category | Faces | Recall |
|---|---|---|
| mask | 8 | 5/8 |
| hand_mouth | 11 | 11/11 |
| none | 3 | 3/3 |
| glasses | 4 | 4/4 |
| tilt | 3 | 3/3 |

## Cost

Both runs: 100 detect calls on `mask-01.jpg` after 5 warm-up calls. The raw files do not record the execution provider.

| Model | p50 | p95 | RSS increase on load | Model file |
|---|---|---|---|---|
| YuNet | 41.7 ms (baseline in the RetinaFace run); 40.8 ms (YuNet run) | 42.9 ms (YuNet run) | 28.9 MB | - |
| RetinaFace MobileNet0.25 | 62.4 ms | 63.6 ms | 33.0 MB (+4.1 MB) | 1.7 MB |

The two YuNet p50 values come from separate runs and differ by 0.9 ms. Both are recorded.

CoreML parity probe on the self-test frame (not an acceptance test):

| Provider | Result | p50 |
|---|---|---|
| CoreML | status 1, confidence 1.0 | 43.7 ms |
| CPU | status 1, confidence 1.0 | 70.7 ms |

The two providers return the same result. CoreML is faster on this frame.

## Acceptance rule

The rule is architecture.md, Decision 3. The baseline is YuNet at its production threshold 0.80.

| Rule | Criterion | RetinaFace at 0.50 | Verdict |
|---|---|---|---|
| 1 | Occluded recall at least 10 points above baseline | 19/19 against 14/19 (the 0.60 row gives 18/19; gain +21 to +26 points) | Pass |
| 2 | Unoccluded recall not below baseline | 10/10 against 10/10 | Pass |
| 3 | Face-free false positives not above baseline | 1/16 against 0/16 (tarantula on a hand, 0.994) | Fail by one false positive |
| 4 | CPU p50 at most 3 times YuNet p50, at most 60 ms, memory increase at most 10 MB | p50 62.4 ms: within 3 times YuNet, above the 60 ms cap by 2.4 ms; memory +4.1 MB | Fail on latency (2.4 ms); memory pass |
| 5 | Threshold: highest grid value with maximal occluded recall at zero false positives, plus one step | No grid value gives zero false positives. Highest value with maximal recall (19/19) is 0.50; one step would be 0.55 (18/19) | Not applicable as written; 0.50 kept (see Decision) |

YuNet as a lower-threshold outcome (O1), under the same rules: 0.65 gives 16/19 with 0/16 false positives. Rule 5 selects 0.75 (highest zero-false-positive value with maximal recall, 0.70, plus one step), which gives 15/19. That is one face above baseline, and rule 1 needs about two. O1 fails rule 1.

## Decision

Adopt O2: replace YuNet with RetinaFace MobileNet0.25 at confidence 0.50 and NMS IoU 0.4, on macOS only, as recorded in ADR 0042. The decision fails rule 3 by one false positive and rule 4 by 2.4 ms on latency. It is adopted as a recorded exception under the owner's instruction to take the decisions.

Reasons:

- A missed face is the harmful error. It can dim or lock a present user. A false presence only leaves the screen lit or the session unlocked.
- The one false positive is a tarantula on a hand (0.994). Every other face-free image scores 0.39 or less.
- The extra cost is about 20 ms per sample. That is negligible at the 10 s default cadence and about 4 percentage points of one core at the 0.5 s return cadence.
- Rules 3 and 4 are judgement values, and the occluded set has 19 faces, below the 30-image level at which architecture.md, Decision 3 requires owner confirmation.

## Near-threshold and notable scores

| File | Category | Score | Note |
|---|---|---|---|
| mask-03.jpg | mask | 0.508 | Just above 0.50; below 0.60 |
| mask-13.jpg | mask | 0.621 | Just above 0.60 |
| hand-mouth-04.jpg | hand_mouth | 0.988 | Lowest hand score; every hand image is 0.988 or more |
| neg-hand-06.jpg | no_face_hand (tarantula on a hand) | 0.994 | Only false positive at 0.40 to 0.90 |
| neg-object-03.jpg | no_face_object | 0.387 | Highest negative after neg-hand-06 |
| neg-hand-05.jpg | no_face_hand | 0.381 | |
| neg-mask-03.jpg | no_face_mask | 0.348 | |

## CoreML and CPU

The CoreML result equals the CPU result on the self-test frame (status 1, confidence 1.0), and CoreML is faster (p50 43.7 ms against 70.7 ms). The selectable device and CPU fallback of ADR 0027 are unchanged. This is a parity probe on one frame on this Mac, not hardware verification.

## Limits

- The occluded set is small (19 faces). Its masks are 1918 archive photographs (mask-01, 02, 03, 10) or U.S. military photographs (mask-08, 09, 12, 13). None is a modern webcam frame. All occlusions are partial; none is heavy. Every figure is indicative, with wide intervals.
- No low-resolution colour webcam frames, night or backlit frames, or children's faces are included. Tilt has 3 images.
- Margins are thin: mask-03 (0.508) and mask-13 (0.621) sit near the 0.50 and 0.60 thresholds.
- A hairy or round object may be taken for a face (false presence). The fixture set does not test this case beyond the tarantula on a hand.
- Latency is one frame on this Mac. The execution provider of the 62.4 ms run is not recorded. The CPU result on the self-test frame (70.7 ms) is higher than the 60 ms cap, so the latency margin depends on the frame and the run.
- RetinaFace is trained on WIDER FACE (CC BY-NC-ND). The legal check is left to the owner, as for YuNet.
- Windows was measured on 2026-10-30, including the DirectML check; see [windows-model-check.md](windows-model-check.md).

## Follow-ups

1. Real webcam validation on hardware: capture masked and hand-over-mouth frames with the target camera, re-run the threshold sweep, and confirm or move 0.50.
2. Windows port: same model and decoder, latency check under rule 4, DirectML check with `occlusion_bench`, and packaging of the model file. Done on 2026-10-30.
3. Optional smaller input export (480 or 320): evaluated only if latency fails on target hardware (architecture.md, Decision 5).
4. Grow the fixture set: heavy occlusion, webcam-resolution colour frames, night and backlit frames, children's faces, and more tilted heads. Re-run the benchmark after each addition.
5. Owner: review the WIDER FACE licence caveat for the RetinaFace model.
