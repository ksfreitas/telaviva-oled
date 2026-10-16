import Foundation

/// Small, transport-independent DDC/CI transaction layer. The caller supplies
/// IOAV operations so packet framing and retry behavior can be tested without hardware.
struct DDCTransaction {
    struct Reading: Equatable {
        let current: UInt16
        let maximum: UInt16
    }

    private let write: ([UInt8]) -> Bool
    private let read: (Int) -> [UInt8]?
    private let sleep: (TimeInterval) -> Void

    init(write: @escaping ([UInt8]) -> Bool,
         read: @escaping (Int) -> [UInt8]?,
         sleep: @escaping (TimeInterval) -> Void = { Thread.sleep(forTimeInterval: $0) }) {
        self.write = write
        self.read = read
        self.sleep = sleep
    }

    func readVCP(code: UInt8) -> Reading? {
        for attempt in 0..<3 {
            for seed in [UInt8(0x6e ^ 0x51), UInt8(0x6e)] {
                var request: [UInt8] = [0x82, 0x01, code]
                request.append(Self.checksum(request, seed: seed))
                guard writeTwice(request) else { continue }
                sleep(0.06)
                guard let reply = read(11), let reading = Self.parse(reply, code: code) else { continue }
                return reading
            }
            if attempt == 0 { sleep(0.2) }
            else if attempt == 1 { sleep(1.0) }
        }
        return nil
    }

    /// IOAV reports that a packet was submitted, not that the monitor applied it.
    /// The caller must verify the requested value with a subsequent Get VCP read.
    func writeVCP(code: UInt8, value: UInt16) -> Bool {
        var packet: [UInt8] = [0x84, 0x03, code, UInt8(value >> 8), UInt8(value & 0xff)]
        packet.append(Self.checksum(packet, seed: 0x6e ^ 0x51))
        for attempt in 0..<3 {
            if writeTwice(packet) { return true }
            if attempt == 0 { sleep(0.2) }
            else if attempt == 1 { sleep(1.0) }
        }
        return false
    }

    private func writeTwice(_ packet: [UInt8]) -> Bool {
        sleep(0.01)
        let first = write(packet)
        sleep(0.01)
        let second = write(packet)
        return first && second
    }

    private static func parse(_ bytes: [UInt8], code: UInt8) -> Reading? {
        guard bytes.count == 11, bytes[0] == 0x6e, bytes[1] == 0x88,
              bytes[2] == 0x02, bytes[3] == 0x00, bytes[4] == code,
              checksum(bytes, seed: 0x50) == 0 else { return nil }
        let maximum = UInt16(bytes[6]) << 8 | UInt16(bytes[7])
        let current = UInt16(bytes[8]) << 8 | UInt16(bytes[9])
        guard maximum > 0, current <= maximum else { return nil }
        return Reading(current: current, maximum: maximum)
    }

    private static func checksum(_ bytes: [UInt8], seed: UInt8) -> UInt8 {
        bytes.reduce(seed) { $0 ^ $1 }
    }
}
