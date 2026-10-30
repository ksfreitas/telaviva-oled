//! Decoder for the RetinaFace MobileNet0.25 detector on its 640x640 input. The model has three
//! outputs (`loc`, `conf` and `landmarks`); this module turns the anchor offsets and face scores
//! into pixel boxes. The landmark output is ignored.

use super::{non_maximum_suppression_with_threshold, Box2d};
use std::sync::OnceLock;

/// Anchors in the 640x640 model output: 80x80x2 + 40x40x2 + 20x20x2.
pub(crate) const ANCHOR_COUNT: usize = 16800;
/// Square model input size in pixels.
const INPUT_PIXELS: f32 = 640.0;
/// Feature-map levels as (stride, anchor sizes in pixels), finest first.
const LEVELS: [(usize, [f32; 2]); 3] =
    [(8, [16.0, 32.0]), (16, [64.0, 128.0]), (32, [256.0, 512.0])];
/// Scale applied to the centre offsets before adding them to the prior centre.
const CENTRE_VARIANCE: f32 = 0.1;
/// Scale applied to the size offsets before the exponential.
const SIZE_VARIANCE: f32 = 0.2;

/// Returns the anchor priors as `[cx, cy, w, h]` in normalised `[0, 1]` image units.
///
/// Anchors are ordered by level, then feature-map row, then column, then anchor size. For a
/// level with stride `s`, cell `(row, column)` and size `m`, the prior is
/// `cx = (column + 0.5) * s / 640`, `cy = (row + 0.5) * s / 640` and `w = h = m / 640`.
pub(crate) fn retinaface_priors() -> Vec<[f32; 4]> {
    let mut priors = Vec::with_capacity(ANCHOR_COUNT);
    for (stride, sizes) in LEVELS {
        let side = INPUT_PIXELS as usize / stride;
        for row in 0..side {
            for column in 0..side {
                let cx = (column as f32 + 0.5) * stride as f32 / INPUT_PIXELS;
                let cy = (row as f32 + 0.5) * stride as f32 / INPUT_PIXELS;
                for size in sizes {
                    let size_norm = size / INPUT_PIXELS;
                    priors.push([cx, cy, size_norm, size_norm]);
                }
            }
        }
    }
    priors
}

/// Decodes one anchor offset against its prior, in normalised image units. The result stays in
/// centre form `[cx, cy, w, h]`.
///
/// With prior `[pcx, pcy, pw, ph]` and offsets `[dx, dy, dw, dh]`:
/// `cx = pcx + dx * 0.1 * pw`, `cy = pcy + dy * 0.1 * ph`,
/// `w = pw * exp(dw * 0.2)` and `h = ph * exp(dh * 0.2)`.
pub(crate) fn decode_retinaface_box(prior: [f32; 4], delta: [f32; 4]) -> [f32; 4] {
    let [cx, cy, w, h] = prior;
    [
        cx + delta[0] * CENTRE_VARIANCE * w,
        cy + delta[1] * CENTRE_VARIANCE * h,
        w * (delta[2] * SIZE_VARIANCE).exp(),
        h * (delta[3] * SIZE_VARIANCE).exp(),
    ]
}

/// Decodes RetinaFace `loc` (`[1, 16800, 4]`) and `conf` (`[1, 16800, 2]`) buffers into pixel
/// boxes on the 640x640 input. An anchor is kept when its face score (`conf` column 1) is finite
/// and at least `confidence`; the kept boxes then go through NMS with `nms_iou`. Returns an empty
/// Vec when either buffer has the wrong length.
///
/// Each box is the decoded centre-form box scaled by 640 and stored as a top-left corner plus
/// size: `x = (cx - w / 2) * 640`, `y = (cy - h / 2) * 640`, `w = w * 640`, `h = h * 640`.
pub(crate) fn decode_retinaface(
    loc: &[f32],
    conf: &[f32],
    confidence: f32,
    nms_iou: f32,
) -> Vec<Box2d> {
    static PRIORS: OnceLock<Vec<[f32; 4]>> = OnceLock::new();
    if loc.len() != ANCHOR_COUNT * 4 || conf.len() != ANCHOR_COUNT * 2 {
        return Vec::new();
    }
    let priors = PRIORS.get_or_init(retinaface_priors);
    let mut detections = Vec::new();
    for (index, prior) in priors.iter().enumerate() {
        let score = conf[index * 2 + 1];
        if !score.is_finite() || score < confidence {
            continue;
        }
        let delta = [
            loc[index * 4],
            loc[index * 4 + 1],
            loc[index * 4 + 2],
            loc[index * 4 + 3],
        ];
        let [cx, cy, w, h] = decode_retinaface_box(*prior, delta);
        let w = w * INPUT_PIXELS;
        let h = h * INPUT_PIXELS;
        let x = cx * INPUT_PIXELS - w / 2.0;
        let y = cy * INPUT_PIXELS - h / 2.0;
        if ![x, y, w, h, score].iter().all(|value| value.is_finite()) || w <= 0.0 || h <= 0.0 {
            continue;
        }
        detections.push(Box2d { x, y, w, h, score });
    }
    non_maximum_suppression_with_threshold(detections, nms_iou)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Anchors in the 640x640 model output: 80x80x2 + 40x40x2 + 20x20x2.
    const ANCHORS: usize = 16800;
    /// Square model input size in pixels.
    const INPUT_SIZE: f32 = 640.0;

    /// Asserts that `actual` is within `tolerance` of `expected`.
    fn approx(actual: f32, expected: f32, tolerance: f32) {
        assert!(
            (actual - expected).abs() <= tolerance,
            "expected {expected} within {tolerance}, got {actual}"
        );
    }

    /// Asserts that each component of `actual` is within `tolerance` of the matching `expected`.
    fn assert_approx4(actual: [f32; 4], expected: [f32; 4], tolerance: f32) {
        for (actual, expected) in actual.into_iter().zip(expected) {
            approx(actual, expected, tolerance);
        }
    }

    /// Builds flat `loc` and `conf` buffers for the whole model output. Every anchor has zero
    /// offsets and the scores `[0.99, 0.01]`; each `(anchor, score)` pair overrides that anchor's
    /// face score, with the background score set to its complement.
    fn model_output(faces: &[(usize, f32)]) -> (Vec<f32>, Vec<f32>) {
        let loc = vec![0.0_f32; ANCHORS * 4];
        let mut conf = [0.99_f32, 0.01].repeat(ANCHORS);
        for &(anchor, score) in faces {
            conf[anchor * 2] = 1.0 - score;
            conf[anchor * 2 + 1] = score;
        }
        (loc, conf)
    }

    /// Pixel-space `[centre_x, centre_y, width, height]` of the prior at `index`. With zero
    /// offsets, the decoder should return this box for that anchor.
    fn prior_pixels(index: usize) -> [f32; 4] {
        let [cx, cy, w, h] = retinaface_priors()[index];
        [
            cx * INPUT_SIZE,
            cy * INPUT_SIZE,
            w * INPUT_SIZE,
            h * INPUT_SIZE,
        ]
    }

    #[test]
    fn priors_follow_the_feature_map_layout() {
        let priors = retinaface_priors();
        assert_eq!(priors.len(), ANCHORS);
        // Level 0 (stride 8, 80x80 cells, sizes 16 and 32).
        assert_approx4(priors[0], [0.00625, 0.00625, 0.025, 0.025], 1e-6);
        assert_approx4(priors[1], [0.00625, 0.00625, 0.05, 0.05], 1e-6);
        // Level 1 (stride 16) starts after 80 * 80 * 2 = 12800 anchors.
        assert_approx4(priors[12800], [0.0125, 0.0125, 0.1, 0.1], 1e-6);
        // Last anchor: level 2 (stride 32), cell (19, 19), size 512.
        assert_approx4(priors[ANCHORS - 1], [0.975, 0.975, 0.8, 0.8], 1e-6);
    }

    #[test]
    fn box_decoding_with_zero_delta_returns_the_prior() {
        let decoded = decode_retinaface_box([0.5, 0.5, 0.1, 0.1], [0.0; 4]);
        assert_approx4(decoded, [0.5, 0.5, 0.1, 0.1], 1e-5);
    }

    #[test]
    fn box_decoding_applies_the_centre_variance() {
        // cx = 0.5 + 1 * 0.1 * 0.1 = 0.51 and cy = 0.5 - 1 * 0.1 * 0.1 = 0.49; size unchanged.
        let decoded = decode_retinaface_box([0.5, 0.5, 0.1, 0.1], [1.0, -1.0, 0.0, 0.0]);
        assert_approx4(decoded, [0.51, 0.49, 0.1, 0.1], 1e-5);
    }

    #[test]
    fn box_decoding_applies_the_size_variance_exponentially() {
        // w = h = 0.1 * exp(5 * 0.2) = 0.1 * e; the centre does not move.
        let size = 0.1 * std::f32::consts::E;
        let decoded = decode_retinaface_box([0.5, 0.5, 0.1, 0.1], [0.0, 0.0, 5.0, 5.0]);
        assert_approx4(decoded, [0.5, 0.5, size, size], 1e-5);
    }

    #[test]
    fn keeps_only_candidates_at_or_above_the_confidence_threshold() {
        let (loc, conf) = model_output(&[(100, 0.9)]);
        let boxes = decode_retinaface(&loc, &conf, 0.5, 0.4);
        assert_eq!(boxes.len(), 1);
        let detection = boxes[0];
        // Boxes are stored as top-left corner plus size, so compare the centre.
        let centre_size = [
            detection.x + detection.w / 2.0,
            detection.y + detection.h / 2.0,
            detection.w,
            detection.h,
        ];
        assert_approx4(centre_size, prior_pixels(100), 1e-3);
        approx(detection.score, 0.9, 1e-6);
    }

    #[test]
    fn returns_nothing_when_no_score_reaches_the_threshold() {
        let (loc, conf) = model_output(&[(100, 0.9)]);
        assert!(decode_retinaface(&loc, &conf, 0.95, 0.4).is_empty());
    }

    #[test]
    fn nms_threshold_decides_whether_overlapping_candidates_survive() {
        // Anchors 100 (16 px) and 101 (32 px) share a cell and centre, so the IoU of the
        // smaller box inside the larger one is 256 / 1024 = 0.25.
        let (loc, conf) = model_output(&[(100, 0.9), (101, 0.9)]);
        assert_eq!(decode_retinaface(&loc, &conf, 0.5, 0.4).len(), 2);
        assert_eq!(decode_retinaface(&loc, &conf, 0.5, 0.2).len(), 1);
    }

    #[test]
    fn ignores_non_finite_face_scores() {
        let (loc, conf) = model_output(&[(100, f32::NAN), (101, f32::INFINITY)]);
        assert!(decode_retinaface(&loc, &conf, 0.5, 0.4).is_empty());
    }
}
