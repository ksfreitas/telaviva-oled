import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("DDCTransactionTests: \(message)") }
}

private func reply(code: UInt8, current: UInt16 = 41, maximum: UInt16 = 100) -> [UInt8] {
    var bytes: [UInt8] = [0x6e, 0x88, 0x02, 0x00, code,
                          0x00, UInt8(maximum >> 8), UInt8(maximum & 0xff),
                          UInt8(current >> 8), UInt8(current & 0xff), 0]
    bytes[10] = bytes[0..<10].reduce(UInt8(0x50)) { $0 ^ $1 }
    return bytes
}

@main
private struct DDCTransactionTests {
    static func main() {
        readUsesDuplicateRequestAndParsesElevenByteReply()
        readFallsBackToLegacyChecksumAndRetriesInvalidReplies()
        malformedRepliesAreRejectedWithinRetryBound()
        staleReplyThenCorrectReplyOnOuterRetry()
        writeFramesWideValuesAndRetriesTransportErrors()
        ignoredFirstWriteIsAppliedBySecondTransmission()
        oneShotReadReturnsReadingOnFirstSeed()
        oneShotReadFallsBackToLegacyChecksumWithoutRetrySleep()
        oneShotReadReturnsNilAfterBothSeedsWithoutRetrySleep()
        oneShotWriteReportsSuccessOnlyWhenBothSendsSucceed()
        retryingReadAndWriteKeepBackoffSchedule()
        edidManufacturerDecodesThreeLetterCodes()
        edidManufacturerRejectsOutOfRangeNumbers()
        print("DDC transaction tests passed")
    }

    private static func readUsesDuplicateRequestAndParsesElevenByteReply() {
        var writes: [[UInt8]] = []
        var readCounts: [Int] = []
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { writes.append($0); return true }, read: { count in
            readCounts.append(count); return reply(code: 0x10)
        }, sleep: { delays.append($0) })
        expect(transaction.readVCP(code: 0x10) == .init(current: 41, maximum: 100), "valid read parses")
        expect(writes.count == 2 && writes[0] == writes[1], "read request is sent twice")
        expect(writes[0] == [0x82, 0x01, 0x10, 0xac], "read request uses standard checksum")
        expect(readCounts == [11], "read requests exactly eleven bytes")
        expect(delays == [0.01, 0.01, 0.06], "both sends and reply wait use deterministic delays")
    }

    private static func readFallsBackToLegacyChecksumAndRetriesInvalidReplies() {
        var writes: [[UInt8]] = []
        var reads = 0
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { writes.append($0); return true }, read: { _ in
            reads += 1
            if reads == 1 { return Array(reply(code: 0x10).dropLast()) }
            return reply(code: 0x10)
        }, sleep: { delays.append($0) })
        expect(transaction.readVCP(code: 0x10) != nil, "valid legacy-checksum attempt recovers")
        expect(writes.count == 4, "bad first reply triggers legacy request")
        expect(writes[0] == writes[1] && writes[2] == writes[3], "each checksum request is duplicated")
        expect(writes[2][3] == 0xfd, "fallback request omits data address from checksum")

        let failed = DDCTransaction(write: { _ in true }, read: { _ in [UInt8](repeating: 0, count: 11) },
                                    sleep: { delays.append($0) })
        expect(failed.readVCP(code: 0x62) == nil, "invalid replies fail after bounded retries")
        expect(delays.contains(0.2) && delays.contains(1.0), "read retries use bounded backoff")
    }

    private static func writeFramesWideValuesAndRetriesTransportErrors() {
        var writes: [[UInt8]] = []
        var calls = 0
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { packet in
            writes.append(packet); calls += 1; return calls > 1
        }, read: { _ in nil }, sleep: { delays.append($0) })
        expect(transaction.writeVCP(code: 0x10, value: 0x1234), "second attempt succeeds")
        expect(writes.count == 4, "failed transport attempt and successful retry each send twice")
        expect(writes[0] == [0x84, 0x03, 0x10, 0x12, 0x34, 0x8e], "write preserves both value bytes and checksum")
        expect(writes[0] == writes[1] && writes[2] == writes[3], "write packet is duplicated")
        expect(delays.filter { $0 == 0.01 }.count == 4 && delays.contains(0.2), "write delays and retry backoff")
    }

    private static func malformedRepliesAreRejectedWithinRetryBound() {
        let wrongCode = reply(code: 0x62)
        var badChecksum = reply(code: 0x10); badChecksum[10] ^= 0x01
        let wrongStatus = reply(code: 0x10).enumerated().map { $0.offset == 3 ? UInt8(1) : $0.element }
        let zeroMaximum = reply(code: 0x10, current: 0, maximum: 0)
        let currentAboveMaximum = reply(code: 0x10, current: 101, maximum: 100)
        let malformed = [wrongCode, badChecksum, wrongStatus, zeroMaximum, currentAboveMaximum]
        for bytes in malformed {
            var writes = 0
            var reads = 0
            let transaction = DDCTransaction(write: { _ in writes += 1; return true }, read: { _ in
                reads += 1; return bytes
            }, sleep: { _ in })
            expect(transaction.readVCP(code: 0x10) == nil, "malformed VCP reply is rejected")
            expect(reads == 6 && writes == 12, "malformed reply respects six-read/twelve-write bound")
        }
    }

    /// Attempt 0 has two inner reads (one per checksum seed). Both return a valid
    /// reply for another feature, so both are rejected. Attempt 1's first inner
    /// read returns the requested feature and must be accepted there.
    private static func staleReplyThenCorrectReplyOnOuterRetry() {
        var writes = 0
        var reads = 0
        let scripted = [
            reply(code: 0x62, current: 7),  // attempt 0, seed 1: stale reply for another feature
            reply(code: 0x62, current: 7),  // attempt 0, seed 2: stale reply for another feature
            reply(code: 0x10, current: 58), // attempt 1, seed 1: correct reply
        ]
        let transaction = DDCTransaction(write: { _ in writes += 1; return true }, read: { _ in
            reads += 1
            return reads <= scripted.count ? scripted[reads - 1] : nil
        }, sleep: { _ in })
        let result = transaction.readVCP(code: 0x10)
        expect(result == .init(current: 58, maximum: 100), "stale reply is not returned")
        // Reads: attempt 0 has 2 inner reads (one per seed) and attempt 1 succeeds on its
        // first inner read, so 2 + 1 = 3. Writes: every inner read is preceded by a
        // duplicated request (2 writes), so 3 * 2 = 6.
        expect(reads == 3 && writes == 6, "stale attempt-zero replies cost three reads and six writes")
    }

    private static func ignoredFirstWriteIsAppliedBySecondTransmission() {
        var writes = 0
        var brightness: UInt16 = 41
        let transaction = DDCTransaction(write: { packet in
            writes += 1
            if writes.isMultiple(of: 2), packet.count == 6 {
                brightness = UInt16(packet[3]) << 8 | UInt16(packet[4])
            }
            return true
        }, read: { count in
            guard count == 11 else { return nil }
            return reply(code: 0x10, current: brightness)
        }, sleep: { _ in })
        expect(transaction.writeVCP(code: 0x10, value: 40), "duplicate write transaction is submitted")
        expect(writes == 2 && brightness == 40, "device ignores first transmission and applies second")
        expect(transaction.readVCP(code: 0x10) == .init(current: 40, maximum: 100), "readback observes applied target")
    }

    private static func oneShotReadReturnsReadingOnFirstSeed() {
        var writes: [[UInt8]] = []
        var readCounts: [Int] = []
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { writes.append($0); return true }, read: { count in
            readCounts.append(count); return reply(code: 0x10)
        }, sleep: { delays.append($0) })
        expect(transaction.readVCPOnce(code: 0x10) == .init(current: 41, maximum: 100), "one-shot read parses on first seed")
        expect(writes.count == 2 && writes[0] == [0x82, 0x01, 0x10, 0xac] && writes[1] == writes[0],
               "one-shot read sends the standard-checksum request twice")
        expect(readCounts == [11], "one-shot read requests eleven bytes once")
        expect(delays == [0.01, 0.01, 0.06], "one-shot read waits only for sends and reply")
    }

    private static func oneShotReadFallsBackToLegacyChecksumWithoutRetrySleep() {
        var writes: [[UInt8]] = []
        var reads = 0
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { writes.append($0); return true }, read: { _ in
            reads += 1
            if reads == 1 { return Array(reply(code: 0x10).dropLast()) }
            return reply(code: 0x10)
        }, sleep: { delays.append($0) })
        expect(transaction.readVCPOnce(code: 0x10) == .init(current: 41, maximum: 100), "one-shot read recovers on legacy seed")
        expect(writes.count == 4 && writes[2][3] == 0xfd, "one-shot read sends the legacy-checksum request second")
        expect(delays == [0.01, 0.01, 0.06, 0.01, 0.01, 0.06], "one-shot fallback waits only for sends and replies")
    }

    private static func oneShotReadReturnsNilAfterBothSeedsWithoutRetrySleep() {
        var writes = 0
        var reads = 0
        var delays: [TimeInterval] = []
        let transaction = DDCTransaction(write: { _ in writes += 1; return true }, read: { _ in
            reads += 1; return [UInt8](repeating: 0, count: 11)
        }, sleep: { delays.append($0) })
        expect(transaction.readVCPOnce(code: 0x62) == nil, "one-shot read fails after both seeds")
        expect(reads == 2 && writes == 4, "one-shot read tries exactly two seeds")
        expect(delays == [0.01, 0.01, 0.06, 0.01, 0.01, 0.06], "one-shot failure records no 0.2 s or 1.0 s backoff")
    }

    private static func oneShotWriteReportsSuccessOnlyWhenBothSendsSucceed() {
        var writes: [[UInt8]] = []
        var delays: [TimeInterval] = []
        let succeeding = DDCTransaction(write: { writes.append($0); return true }, read: { _ in nil }, sleep: { delays.append($0) })
        expect(succeeding.writeVCPOnce(code: 0x10, value: 0x1234), "one-shot write succeeds when both sends succeed")
        expect(writes == [[0x84, 0x03, 0x10, 0x12, 0x34, 0x8e], [0x84, 0x03, 0x10, 0x12, 0x34, 0x8e]],
               "one-shot write sends the framed packet twice")
        expect(delays == [0.01, 0.01], "one-shot write waits only between sends")

        for failingSend in [1, 2] {
            var calls = 0
            var failingDelays: [TimeInterval] = []
            let failing = DDCTransaction(write: { _ in calls += 1; return calls != failingSend }, read: { _ in nil },
                                         sleep: { failingDelays.append($0) })
            expect(!failing.writeVCPOnce(code: 0x10, value: 0x1234), "one-shot write fails when send \(failingSend) fails")
            expect(calls == 2, "one-shot write always attempts exactly two sends")
            expect(failingDelays == [0.01, 0.01], "one-shot write records no retry backoff")
        }
    }

    private static func retryingReadAndWriteKeepBackoffSchedule() {
        let oneAttempt: [TimeInterval] = [0.01, 0.01, 0.06, 0.01, 0.01, 0.06]
        let readSchedule: [TimeInterval] = oneAttempt + [0.2] + oneAttempt + [1.0] + oneAttempt
        var reads = 0
        var readDelays: [TimeInterval] = []
        let reader = DDCTransaction(write: { _ in true }, read: { _ in reads += 1; return nil }, sleep: { readDelays.append($0) })
        expect(reader.readVCP(code: 0x10) == nil, "retrying read gives up after three attempts")
        expect(reads == 6, "retrying read makes two reads per attempt")
        expect(readDelays == readSchedule, "retrying read backs off 0.2 s then 1.0 s")

        var writes = 0
        var writeDelays: [TimeInterval] = []
        let writer = DDCTransaction(write: { _ in writes += 1; return false }, read: { _ in nil }, sleep: { writeDelays.append($0) })
        expect(!writer.writeVCP(code: 0x10, value: 40), "retrying write gives up after three attempts")
        expect(writes == 6, "retrying write makes two sends per attempt")
        expect(writeDelays == [0.01, 0.01, 0.2, 0.01, 0.01, 1.0, 0.01, 0.01], "retrying write backs off 0.2 s then 1.0 s")
    }

    private static func edidManufacturerDecodesThreeLetterCodes() {
        expect(EDIDManufacturer.code(vendor: 7789) == "GSM", "0x1E6D decodes to GSM")
        expect(EDIDManufacturer.code(vendor: 0x0610) == "APP", "0x0610 decodes to APP")
    }

    private static func edidManufacturerRejectsOutOfRangeNumbers() {
        expect(EDIDManufacturer.code(vendor: 0) == "", "zero vendor has no code")
        expect(EDIDManufacturer.code(vendor: 0x10000) == "", "vendor wider than 16 bits has no code")
        expect(EDIDManufacturer.code(vendor: 0x0021) == "", "first letter group 0 has no code")
        expect(EDIDManufacturer.code(vendor: 0x0401) == "", "middle letter group 0 has no code")
        expect(EDIDManufacturer.code(vendor: 0x001B) == "", "last letter group 27 has no code")
        expect(EDIDManufacturer.code(vendor: 0x6C41) == "", "first letter group 27 has no code")
    }
}
