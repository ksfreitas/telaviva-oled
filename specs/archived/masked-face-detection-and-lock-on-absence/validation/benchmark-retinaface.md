# Occlusion benchmark

- Model: `/tmp/telaviva-retina.Xxtdii/axelera-retinaface.onnx`
- Family: `retinaface`
- Fixture images: 45 (29 with a face, 16 face-free)
- Insufficient light (scored 0): none

## Threshold sweep

| Threshold | Occluded recall | Unoccluded recall | Face-free false positives |
|---|---|---|---|
| 0.30 | 19/19 (0.83–1.00) | 10/10 (0.72–1.00) | 4/16 (0.10–0.49) |
| 0.35 | 19/19 (0.83–1.00) | 10/10 (0.72–1.00) | 3/16 (0.07–0.43) |
| 0.40 | 19/19 (0.83–1.00) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.45 | 19/19 (0.83–1.00) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.50 | 19/19 (0.83–1.00) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.55 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.60 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.65 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.70 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.75 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.80 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.85 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.90 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |

## Results by category

Not computed: there is no operating threshold. The adopted operating threshold (0.50) and the rule verdicts are in [model-decision.md](model-decision.md).

## Per-image scores

| File | Category | Has face | Occlusion | Max score |
|---|---|---|---|---|
| glasses-01.jpg | glasses | yes | none | 1.000 |
| glasses-02.jpg | glasses | yes | none | 1.000 |
| glasses-03.jpg | glasses | yes | none | 1.000 |
| glasses-04.jpg | glasses | yes | none | 1.000 |
| hand-mouth-01.jpg | hand_mouth | yes | partial | 0.999 |
| hand-mouth-02.jpg | hand_mouth | yes | partial | 0.999 |
| hand-mouth-03.jpg | hand_mouth | yes | partial | 0.999 |
| hand-mouth-04.jpg | hand_mouth | yes | partial | 0.988 |
| hand-mouth-15.jpg | hand_mouth | yes | partial | 0.999 |
| hand-mouth-16.jpg | hand_mouth | yes | partial | 0.999 |
| hm-b-01.jpg | hand_mouth | yes | partial | 0.999 |
| hm-b-02.jpg | hand_mouth | yes | partial | 0.996 |
| hm-c-01.jpg | hand_mouth | yes | partial | 0.999 |
| hm-c-02.jpg | hand_mouth | yes | partial | 0.997 |
| hm-c-03.jpg | hand_mouth | yes | partial | 0.995 |
| mask-01.jpg | mask | yes | partial | 0.999 |
| mask-02.jpg | mask | yes | partial | 0.987 |
| mask-03.jpg | mask | yes | partial | 0.508 |
| mask-08.jpg | mask | yes | partial | 0.996 |
| mask-09.jpg | mask | yes | partial | 0.997 |
| mask-10.jpg | mask | yes | partial | 0.918 |
| mask-12.jpg | mask | yes | partial | 0.997 |
| mask-13.jpg | mask | yes | partial | 0.621 |
| neg-hand-01.jpg | no_face_hand | no | none | 0.000 |
| neg-hand-02.jpg | no_face_hand | no | none | 0.000 |
| neg-hand-03.jpg | no_face_hand | no | none | 0.286 |
| neg-hand-04.jpg | no_face_hand | no | none | 0.140 |
| neg-hand-05.jpg | no_face_hand | no | none | 0.381 |
| neg-hand-06.jpg | no_face_hand | no | none | 0.994 |
| neg-mask-01.jpg | no_face_mask | no | none | 0.000 |
| neg-mask-02.jpg | no_face_mask | no | none | 0.056 |
| neg-mask-03.jpg | no_face_mask | no | none | 0.348 |
| neg-mask-04.jpg | no_face_mask | no | none | 0.000 |
| neg-object-01.jpg | no_face_object | no | none | 0.000 |
| neg-object-02.jpg | no_face_object | no | none | 0.136 |
| neg-object-03.jpg | no_face_object | no | none | 0.387 |
| neg-object-04.jpg | no_face_object | no | none | 0.052 |
| neg-object-05.jpg | no_face_object | no | none | 0.254 |
| neg-object-06.jpg | no_face_object | no | none | 0.000 |
| plain-01.jpg | none | yes | none | 0.999 |
| plain-02.jpg | none | yes | none | 0.999 |
| plain-03.jpg | none | yes | none | 1.000 |
| tilt-01.jpg | tilt | yes | none | 1.000 |
| tilt-02.jpg | tilt | yes | none | 1.000 |
| tilt-03.jpg | tilt | yes | none | 0.999 |

## Cost

- Latency: p50 62.4 ms, p95 63.6 ms (100 detect calls on `mask-01.jpg` after 5 warm-up calls)
- Memory increase on model load (RSS): 33.0 MB

## Acceptance rules

Baseline `assets/model/face_detection_yunet_2023mar.onnx` (yunet) is judged at the production threshold 0.80: p50 41.7 ms.
Not evaluated: the candidate has no operating threshold. The adopted operating threshold (0.50) and the rule verdicts are in [model-decision.md](model-decision.md).

## Coverage gaps

- Totals in `manifest.toml`: 45 entries, 29 with a face and 16 without. By category: `none` 3, `mask` 8, `hand_mouth` 11, `hand_eyes` 0, `glasses` 4, `tilt` 3, `no_face_hand` 6, `no_face_mask` 4, `no_face_object` 6.
- No fixture image shows a hand over the eyes; the owner scoped that category out (see plan T018).
- The occluded set is small (8 `mask` and 11 `hand_mouth` images), so every recall figure is indicative, with wide confidence intervals.
- Every occluded image is marked `occlusion = "partial"`; no image is marked `heavy`.
- The 8 mask images are 1918 archive photographs (mask-01, 02, 03, 10) or official U.S. military photographs (mask-08, 09, 12, 13). None is a modern webcam frame.
- The 11 `hand_mouth` images include 6 where the hand only partly covers the mouth (hand-mouth-01 to 04, 15, 16) and 5 sneezes or colds with a tissue or hand over nose and mouth (hm-b-01, hm-b-02, hm-c-01 to hm-c-03), three of which have closed eyes (hm-c-01 to hm-c-03). hm-b-02 and hm-c-03 show the same model and pose in different clothes.
- `tilt` has only 3 images (tilt-01 to tilt-03).
- No low-resolution color webcam frames, no night or backlit frames and no children's faces are included. neg-hand-01 shows a baby's closed fist, with no face.
- The face-free negatives include a tarantula on a hand (neg-hand-06), respirators (neg-mask-02 to neg-mask-04), a dim study with bookshelves and framed pictures (neg-object-02), and a conference room with a bust and framed paintings (neg-object-03).

Update this section as images are collected, listing the categories or occlusion levels that still have no images.
