import CoreAudio
import Foundation

/// Reports changes to the Mac's default sound output device UID.
///
/// Listeners and the reader run on a private serial queue. The observer holds no timer
/// and does no work while stopped. It must never be called from the keyboard event-tap
/// callback, because the reader calls CoreAudio.
final class DefaultAudioOutputMonitor {
    typealias Reader = () -> String?

    /// Receives the default output UID, or `nil` for pass-through. Set it before `start()`.
    /// It runs on the private queue while the monitor lock is held, so it must not wait
    /// for the thread that calls `stop()`.
    var onChange: ((String?) -> Void)?

    private static let systemObject = AudioObjectID(kAudioObjectSystemObject)
    private static let listenedAddresses: [AudioObjectPropertyAddress] = [
        AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain),
        AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain),
    ]

    private let readDefaultOutputUID: Reader
    private let queue = DispatchQueue(label: "com.telaviva.default-audio-output")
    private let lock = NSRecursiveLock()
    private var running = false
    private var listeners: [(address: AudioObjectPropertyAddress, block: AudioObjectPropertyListenerBlock)] = []
    /// The outer `nil` means nothing was reported since `start()`, so the first read always reports.
    private var lastReported: String?? = nil

    init(readDefaultOutputUID: @escaping Reader = DefaultAudioOutputMonitor.systemDefaultOutputUID) {
        self.readDefaultOutputUID = readDefaultOutputUID
    }

    /// Installs the CoreAudio listeners and reports the current value once. Idempotent.
    /// Must not be called from `onChange`.
    func start() {
        lock.lock()
        guard !running else {
            lock.unlock()
            return
        }
        running = true
        for var address in Self.listenedAddresses {
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                self?.report()
            }
            let status = AudioObjectAddPropertyListenerBlock(Self.systemObject, &address, queue, block)
            if status == noErr {
                listeners.append((address: address, block: block))
            }
        }
        lock.unlock()

        queue.sync { report() }
    }

    /// Removes exactly the listeners installed by `start()` and silences later reports.
    /// Idempotent. When it returns, `onChange` will not run until the next `start()`.
    func stop() {
        lock.lock()
        guard running else {
            lock.unlock()
            return
        }
        running = false
        lastReported = nil
        let installed = listeners
        listeners.removeAll()
        lock.unlock()

        // Removed outside the lock: CoreAudio may wait for an in-flight listener call
        // that needs this lock. `running` is already false, so no report can follow.
        for entry in installed {
            var address = entry.address
            _ = AudioObjectRemovePropertyListenerBlock(Self.systemObject, &address, queue, entry.block)
        }
    }

    /// Runs the same read, compare, and report path synchronously on the private queue.
    func refreshForTesting() {
        queue.sync { report() }
    }

    /// Reads the default output UID and reports it only when it differs from the last report.
    /// A failed or missing read is reported as `nil`.
    private func report() {
        let value = readDefaultOutputUID()
        lock.lock()
        defer { lock.unlock() }
        guard running else { return }
        if let last = lastReported, last == value {
            return
        }
        lastReported = value
        onChange?(value)
    }

    /// Reads the default output device UID from CoreAudio. Returns `nil` when there is no
    /// default output or any read fails. Must never be called from the event-tap callback.
    static func systemDefaultOutputUID() -> String? {
        var deviceID = AudioObjectID(kAudioObjectUnknown)
        var deviceSize = UInt32(MemoryLayout<AudioObjectID>.size)
        var deviceAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let deviceStatus = AudioObjectGetPropertyData(
            systemObject, &deviceAddress, 0, nil, &deviceSize, &deviceID)
        guard deviceStatus == noErr, deviceID != AudioObjectID(kAudioObjectUnknown) else {
            return nil
        }

        var uidRef: Unmanaged<CFString>?
        var uidSize = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        var uidAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let uidStatus = AudioObjectGetPropertyData(deviceID, &uidAddress, 0, nil, &uidSize, &uidRef)
        guard uidStatus == noErr, let uid = uidRef else {
            return nil
        }
        // CoreAudio returns a retained CFString for the device UID.
        return uid.takeRetainedValue() as String
    }
}
