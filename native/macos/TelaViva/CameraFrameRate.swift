import Foundation

/// Frame rate of a retained capture session, in frames per second. It is set once, right after the session
/// starts, and never changed while the session runs: changing the rate of a running camera restarts its
/// stream and makes the camera LED blink. Matched to the nearest rate the camera supports.
let sessionFrameRate: Double = 2

/// One supported frame-rate range of a capture format, in frames per second. Cameras may expose several
/// ranges, each continuous (15...30) or a single discrete rate (min == max, such as 15.000015).
struct FrameRateRange: Equatable {
    let minimum: Double
    let maximum: Double
}

/// Where a chosen rate sits in its range. At an edge the caller must use the device's own duration value for
/// that edge, because a rate rounded to a rational can land just outside the supported duration range, and
/// AVFoundation raises an uncatchable Objective-C exception for that.
enum FrameRateEdge: Equatable {
    case inside
    case minimum
    case maximum
}

struct FrameRateChoice: Equatable {
    let rangeIndex: Int
    let rate: Double
    let edge: FrameRateEdge
}

/// Within this many frames per second of a range edge, the edge itself is used.
let frameRateEdgeTolerance: Double = 0.01

/// Picks the supported range and rate closest to the target, or nil when no range is usable.
/// Ranges that are non-finite, non-positive or inverted are skipped. Ties prefer the lower rate, then the lower index.
func chooseFrameRate(target: Double = sessionFrameRate, ranges: [FrameRateRange]) -> FrameRateChoice? {
    var best: FrameRateChoice?
    var bestDistance = Double.infinity
    for (index, range) in ranges.enumerated() {
        guard range.minimum.isFinite, range.maximum.isFinite, range.minimum > 0,
              range.minimum <= range.maximum else { continue }
        let candidate: FrameRateChoice
        if target <= range.minimum + frameRateEdgeTolerance {
            candidate = FrameRateChoice(rangeIndex: index, rate: range.minimum, edge: .minimum)
        } else if target >= range.maximum - frameRateEdgeTolerance {
            candidate = FrameRateChoice(rangeIndex: index, rate: range.maximum, edge: .maximum)
        } else {
            candidate = FrameRateChoice(rangeIndex: index, rate: target, edge: .inside)
        }
        let distance = abs(candidate.rate - target)
        if distance < bestDistance || (distance == bestDistance && candidate.rate < (best?.rate ?? .infinity)) {
            best = candidate
            bestDistance = distance
        }
    }
    return best
}

/// Frame duration for a rate as a rational (value / timescale seconds) that stays free of CoreMedia.
/// Uses a fixed value of 1000 so fractional rates such as 7.5 fps stay exact. A non-finite or
/// non-positive rate falls back to the session rate.
func frameDuration(forRate rate: Double) -> (value: Int64, timescale: Int32) {
    let safe = (rate.isFinite && rate > 0) ? rate : sessionFrameRate
    return (1000, Int32((safe * 1000).rounded()))
}
