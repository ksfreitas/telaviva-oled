# Occlusion benchmark

- Model: `assets/model/face_detection_yunet_2023mar.onnx`
- Family: `yunet`
- Fixture images: 45 (29 with a face, 16 face-free)
- Insufficient light (scored 0): none

## Threshold sweep

| Threshold | Occluded recall | Unoccluded recall | Face-free false positives |
|---|---|---|---|
| 0.30 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 2/16 (0.03–0.36) |
| 0.35 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.40 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.45 | 18/19 (0.75–0.99) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.50 | 17/19 (0.69–0.97) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.55 | 16/19 (0.62–0.94) | 10/10 (0.72–1.00) | 1/16 (0.01–0.28) |
| 0.60 | 16/19 (0.62–0.94) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.65 | 16/19 (0.62–0.94) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.70 | 16/19 (0.62–0.94) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.75 | 15/19 (0.57–0.91) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.80 | 14/19 (0.51–0.88) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.85 | 13/19 (0.46–0.85) | 10/10 (0.72–1.00) | 0/16 (0.00–0.19) |
| 0.90 | 7/19 (0.19–0.59) | 8/10 (0.49–0.94) | 0/16 (0.00–0.19) |

## Results by category

At threshold 0.75.

| Category | Faces | Recall | Face-free images | False positives |
|---|---|---|---|---|
| mask | 8 | 4/8 (0.22–0.78) | 0 | - |
| hand_mouth | 11 | 11/11 (0.74–1.00) | 0 | - |
| none | 3 | 3/3 (0.44–1.00) | 0 | - |
| glasses | 4 | 4/4 (0.51–1.00) | 0 | - |
| tilt | 3 | 3/3 (0.44–1.00) | 0 | - |
| no_face_hand | 0 | - | 6 | 0/6 (0.00–0.39) |
| no_face_mask | 0 | - | 4 | 0/4 (0.00–0.49) |
| no_face_object | 0 | - | 6 | 0/6 (0.00–0.39) |

## Cost

- Latency: p50 40.8 ms, p95 42.9 ms (100 detect calls on `mask-01.jpg` after 5 warm-up calls)
- Memory increase on model load (RSS): 28.9 MB

## Coverage gaps

- Totals in `manifest.toml`: 45 entries, 29 with a face and 16 without. By category: `none` 3, `mask` 8, `hand_mouth` 11, `hand_eyes` 0, `glasses` 4, `tilt` 3, `no_face_hand` 6, `no_face_mask` 4, `no_face_object` 6.
- The occluded set is small (8 `mask` and 11 `hand_mouth` images), so every recall figure is indicative, with wide confidence intervals.
- Every occluded image is marked `occlusion = "partial"`; no image is marked `heavy`.
- The 8 mask images are 1918 archive photographs (mask-01, 02, 03, 10) or official U.S. military photographs (mask-08, 09, 12, 13). None is a modern webcam frame.
- The 11 `hand_mouth` images include 6 where the hand only partly covers the mouth (hand-mouth-01 to 04, 15, 16) and 5 sneezes or colds with a tissue or hand over nose and mouth (hm-b-01, hm-b-02, hm-c-01 to hm-c-03), three of which have closed eyes (hm-c-01 to hm-c-03). hm-b-02 and hm-c-03 show the same model and pose in different clothes.
- `tilt` has only 3 images (tilt-01 to tilt-03).
- No low-resolution color webcam frames, no night or backlit frames and no children's faces are included. neg-hand-01 shows a baby's closed fist, with no face.
- The face-free negatives include a tarantula on a hand (neg-hand-06), respirators (neg-mask-02 to neg-mask-04), a dim study with bookshelves and framed pictures (neg-object-02), and a conference room with a bust and framed paintings (neg-object-03).

Update this section as images are collected, listing the categories or occlusion levels that still have no images.
