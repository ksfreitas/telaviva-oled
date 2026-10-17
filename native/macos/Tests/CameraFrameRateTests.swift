import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CameraFrameRateTests: \(message)") }
}

@main
private struct CameraFrameRateTests {
    static func main() {
        // Frame durations are exact rationals over a timescale of 1000 and fall back to the session rate.
        let duration2 = frameDuration(forRate: 2)
        expect(duration2.value == 1000 && duration2.timescale == 2000, "2 fps must map to 1000/2000")
        let duration75 = frameDuration(forRate: 7.5)
        expect(duration75.value == 1000 && duration75.timescale == 7500, "7.5 fps must map to 1000/7500")
        let durationSession = frameDuration(forRate: sessionFrameRate)
        let durationZero = frameDuration(forRate: 0)
        expect(durationZero.value == durationSession.value && durationZero.timescale == durationSession.timescale,
               "zero rate must fall back to the session frame duration")
        let durationNaN = frameDuration(forRate: Double.nan)
        expect(durationNaN.value == durationSession.value && durationNaN.timescale == durationSession.timescale,
               "NaN rate must fall back to the session frame duration")
        for rate in [2.0, 5.0, 15.0, 7.5] {
            let duration = frameDuration(forRate: rate)
            let seconds = Double(duration.value) / Double(duration.timescale)
            expect(abs(seconds - 1 / rate) < 1e-9, "frame duration must round-trip to 1/rate")
        }

        // A continuous MacBook-style range of 15...30 resolves the session rate to its minimum edge.
        let macBookRanges = [FrameRateRange(minimum: 15, maximum: 30)]
        expect(chooseFrameRate(ranges: macBookRanges) == FrameRateChoice(rangeIndex: 0, rate: 15, edge: .minimum),
               "continuous 15...30 range must resolve the session rate to its minimum edge")

        // A continuous 1...30 range keeps the session target and explicit targets inside the range.
        let wideRanges = [FrameRateRange(minimum: 1, maximum: 30)]
        expect(chooseFrameRate(ranges: wideRanges) == FrameRateChoice(rangeIndex: 0, rate: 2, edge: .inside),
               "session rate must choose 2 fps inside a 1...30 range")
        expect(chooseFrameRate(target: 5, ranges: wideRanges) == FrameRateChoice(rangeIndex: 0, rate: 5, edge: .inside),
               "a 5 fps target must choose 5 fps inside a 1...30 range")
        expect(chooseFrameRate(target: 15, ranges: wideRanges) == FrameRateChoice(rangeIndex: 0, rate: 15, edge: .inside),
               "a 15 fps target must choose 15 fps inside a 1...30 range")

        // A single discrete 30 fps range is the only choice for the session rate and for explicit targets.
        let fixedRanges = [FrameRateRange(minimum: 30, maximum: 30)]
        expect(chooseFrameRate(ranges: fixedRanges) == FrameRateChoice(rangeIndex: 0, rate: 30, edge: .minimum),
               "single 30 fps range must resolve the session rate to 30 fps")
        expect(chooseFrameRate(target: 15, ranges: fixedRanges) == FrameRateChoice(rangeIndex: 0, rate: 30, edge: .minimum),
               "single 30 fps range must resolve a 15 fps target to 30 fps")

        // A list of discrete rates picks the rate nearest the session rate or an explicit target.
        let discreteRates: [Double] = [30.00003, 24.00003840006144, 20.0, 15.000015000015, 10.0, 7.500001875000469, 5.0]
        let discreteRanges = discreteRates.map { FrameRateRange(minimum: $0, maximum: $0) }
        expect(chooseFrameRate(ranges: discreteRanges) == FrameRateChoice(rangeIndex: 6, rate: 5.0, edge: .minimum),
               "session rate must choose the nearest discrete rate, 5 fps")
        expect(chooseFrameRate(target: 15, ranges: discreteRanges)
               == FrameRateChoice(rangeIndex: 3, rate: 15.000015000015, edge: .minimum),
               "a 15 fps target must choose the discrete rate nearest 15 fps")

        // Equal distance from the target prefers the lower rate.
        let tieRanges = [FrameRateRange(minimum: 10, maximum: 10), FrameRateRange(minimum: 20, maximum: 20)]
        expect(chooseFrameRate(target: 15, ranges: tieRanges) == FrameRateChoice(rangeIndex: 0, rate: 10, edge: .maximum),
               "equal-distance tie must prefer the lower rate")

        // Invalid or empty range lists choose nothing; invalid ranges are skipped in favor of valid ones.
        expect(chooseFrameRate(ranges: []) == nil, "empty range list must choose nothing")
        expect(chooseFrameRate(ranges: [FrameRateRange(minimum: 0, maximum: 30)]) == nil,
               "zero minimum must be skipped")
        expect(chooseFrameRate(ranges: [FrameRateRange(minimum: 30, maximum: 5)]) == nil,
               "inverted range must be skipped")
        expect(chooseFrameRate(ranges: [FrameRateRange(minimum: Double.nan, maximum: 30)]) == nil,
               "NaN minimum must be skipped")
        expect(chooseFrameRate(ranges: [FrameRateRange(minimum: 5, maximum: Double.infinity)]) == nil,
               "infinite maximum must be skipped")
        let mixedRanges = [FrameRateRange(minimum: 0, maximum: 30), FrameRateRange(minimum: 1, maximum: 30)]
        expect(chooseFrameRate(ranges: mixedRanges) == FrameRateChoice(rangeIndex: 1, rate: 2, edge: .inside),
               "an invalid range followed by a valid range must choose the valid range")

        print("Camera frame-rate tests passed")
    }
}
