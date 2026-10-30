# Occlusion benchmark fixtures

## Purpose

This folder holds the images used by `examples/occlusion_bench.rs` (feature `bench`) to compare face detectors on faces partly covered by a mask or a hand. The benchmark also includes images with no face, so false positives can be counted.

## Categories

Each image belongs to one category, which is recorded in `manifest.toml`.

- `none`: an unobstructed face. `has_face = true`.
- `mask`: a face covered by a surgical or cloth mask. `has_face = true`.
- `hand_mouth`: a face with a hand or a tissue held by the hands over the mouth and/or nose (including sneezes with closed eyes), or a hand held at the mouth and chin. `has_face = true`.
- `hand_eyes`: a face with a hand covering the eyes. `has_face = true`. This category has no images: the owner scoped the hand test to the mouth and nose.
- `glasses`: a face wearing glasses or sunglasses. `has_face = true`.
- `tilt`: a face turned or tilted well away from the camera. `has_face = true`.
- `no_face_hand`: a scene with a hand and no face. `has_face = false`.
- `no_face_mask`: a scene with a mask and no face. `has_face = false`.
- `no_face_object`: a scene with an object and no face. `has_face = false`.

## Rules

- Every image must be CC0-1.0 or public domain (PDM-1.0). Check the license on the source page, one file at a time, before downloading.
- Every entry in `manifest.toml` records `source_url`, `license`, `author` and `subject_rights_checked`.
- `subject_rights_checked = true` means the Commons file page carries no restriction flag and declares CC0 or public domain; for identifiable people it does not certify their consent. The images are historical, agency or self-published releases. If a depicted person asks, remove the image.
- Only CC0 and public-domain files were accepted (owner decision 7). Images from Unsplash and Pexels were refused even when Commons tags them CC0.
- Images are downscaled to at most 640 px on the longest side and stored as JPEG.
- Pexels, Unsplash, WIDER FACE, MAFA, FFHQ and MaskedFace-Net images are NOT allowed.

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

## How to run

```bash
cargo run -p telaviva-inference --features bench --example occlusion_bench -- --model <path> --family <yunet> [--runtime <libonnxruntime path>]
```

- `--model <path>`: the detector model file to benchmark.
- `--family <yunet>`: the detector family, which selects the pre- and post-processing. Only `yunet` is available for now.
- `--runtime <libonnxruntime path>`: optional path to the ONNX Runtime library, if it is not found by default.
