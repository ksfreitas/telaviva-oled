import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import os

extension Notification.Name {
    /// Raw names of the AVCaptureDevice connect and disconnect notifications. The typed
    /// `AVCaptureDevice.wasConnectedNotification` members are missing from older SDKs, such as the macos-14 runner's.
    static let cameraDeviceConnected = Notification.Name("AVCaptureDeviceWasConnectedNotification")
    static let cameraDeviceDisconnected = Notification.Name("AVCaptureDeviceWasDisconnectedNotification")
}

private let wakeLog = Logger(subsystem: Bundle.main.bundleIdentifier ?? "com.telaviva.oled", category: "wake")

/// Wall-clock milliseconds for wake timing logs, on the same clock as `captured_at_ms`.
private func wakeMilliseconds() -> Int64 { Int64(Date().timeIntervalSince1970 * 1000) }

/// Camera queue QoS. Background keeps frame callbacks and inference on the efficiency cores; revert to .utility here if hardware runs show deferred watchdogs.
private let cameraQueueQoS: DispatchQoS = .background

/// Uses one-frame sessions by default and can retain a healthy session in opt-in always-on mode.
/// Return frames are discarded between deadlines; frame bytes never enter the ABI.
final class PresenceCamera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let inferenceReport = InferenceDeviceSelection.Report()
    private let queue = DispatchQueue(label: "com.telaviva.camera", qos: cameraQueueQoS)
    /// Times the watchdog and settle deadlines on the utility queue, so a deferred background queue cannot delay them.
    private let deadlineQueue = DispatchQueue(label: "com.telaviva.camera.deadlines", qos: .utility)
    private var session: AVCaptureSession?
    private var output: AVCaptureVideoDataOutput?
    private weak var core: CoreBridge?
    private var mode = "disabled"
    private var captureMode = "single_frame"
    private var generation: UInt64 = 0
    private var selectedCameraID: String?
    private var sampling = false
    private var inferenceRequested = false
    private var receivedFirstFrame = false
    private var stopped = false
    private var sampleTimer: DispatchSourceTimer?
    private var timeoutTimer: DispatchWorkItem?
    private var settleTimer: DispatchWorkItem?
    private var detectorReleaseTimer: DispatchWorkItem?
    private var detector: OpaquePointer?
    private lazy var inferenceHost = InferenceDeviceSelection.DetectorHost<OpaquePointer>(
        session: InferenceDeviceSelection.Session(preference: "auto"),
        create: { [weak self] candidate in
            self?.createDetector(provider: candidate.provider, deviceIndex: candidate.deviceIndex)
        },
        destroy: { tv_inference_destroy($0) },
        selectedProvider: { tv_inference_selected_provider($0) },
        validate: { [weak self] handle in InferenceDeviceSelection.validate(self, handle) { $0.validate($1) } },
        publish: { [weak self] value, decided in
            guard let self else { return }
            self.inferenceReport.update(value, decided: decided)
            guard decided else { return }
            let computeUnits: String
            switch value.activeId {
            case "ane": computeUnits = "CPUAndNeuralEngine"
            case "gpu": computeUnits = "CPUAndGPU"
            case "auto": computeUnits = "ALL"
            default: computeUnits = "CPU"
            }
            let reason = value.reason?.rawValue ?? "none"
            wakeLog.info("inference_selection requested=\(value.requestedId, privacy: .public) active=\(value.activeId, privacy: .public) compute_units=\(computeUnits, privacy: .public) state=\(value.state.rawValue, privacy: .public) reason=\(reason, privacy: .public)")
        })
    private var lifecycle = CameraLifecycle()
    private var sampleGeneration: UInt64 = 0
    private var timeoutEpoch: UInt64 = 0
    private var sampleTimerEpoch: UInt64 = 0
    private var retryTimer: DispatchSourceTimer?
    private var retryTimerEpoch: UInt64 = 0
    private var retryTimerGeneration: UInt64 = 0
    private var sampleStartedAt: TimeInterval = 0
    private var timeoutAssessmentToken: UInt64?
    private var latestSnapshotRevision: UInt64?
    private var cameraHistory: [String] = []
    private var activeCameraID: String?
    private var activeDevice: AVCaptureDevice?
    private var lastReportedCameraStatus: String?
    private var deviceObservers: [NSObjectProtocol] = []
    /// Last lid state from `setLidClosed`. Camera queue only.
    private var lidClosed = false

    /// The effective selection history; a legacy selection without a history counts as a one-entry history.
    private var selectionHistory: [String] {
        cameraHistory.isEmpty ? (selectedCameraID.map { [$0] } ?? []) : cameraHistory
    }

    override init() {
        super.init()
        let center = NotificationCenter.default
        // Registered once. Both handlers hop to the camera queue before touching capture state.
        deviceObservers.append(center.addObserver(
            forName: .cameraDeviceConnected, object: nil, queue: nil
        ) { [weak self] notification in
            guard let device = notification.object as? AVCaptureDevice else { return }
            self?.queue.async { [weak self] in self?.cameraDeviceChanged(device, connected: true) }
        })
        deviceObservers.append(center.addObserver(
            forName: .cameraDeviceDisconnected, object: nil, queue: nil
        ) { [weak self] notification in
            guard let device = notification.object as? AVCaptureDevice else { return }
            self?.queue.async { [weak self] in self?.cameraDeviceChanged(device, connected: false) }
        })
    }

    deinit {
        for token in deviceObservers { NotificationCenter.default.removeObserver(token) }
    }

    /// Updates the detector preference on the camera queue. Re-selecting the same id is a no-op;
    /// a changed choice releases the session and makes its next creation validate the new route.
    func setInferenceDevice(_ id: String) {
        queue.async { [weak self] in
            guard let self, !self.stopped, self.inferenceHost.session.preference != id else { return }
            self.releaseDetector()
            self.inferenceHost.setPreference(id)
        }
    }

    /// Called from the lid monitor and the wake handlers on any queue. While the lid is closed the built-in camera
    /// is never used. A repeated value is ignored, so wake reads cost nothing when the lid did not change.
    func setLidClosed(_ closed: Bool) {
        queue.async { [weak self] in
            guard let self, !self.stopped, self.lidClosed != closed else { return }
            self.lidClosed = closed
            if closed {
                // Moves a session off the built-in camera the way a disconnect does. With no other camera,
                // the next sample reports the camera unavailable.
                guard self.sampling, self.activeDevice?.deviceType == .builtInWideAngleCamera else { return }
                self.stopCapture()
                if self.mode == "face" || self.mode == "light_only" { self.scheduleSample(after: 0) }
            } else {
                self.resendUnknownStatusIfNeeded()
                if self.sampling {
                    // The lid-open priority order may now pick a different camera, such as the built-in one that
                    // ranks ahead of a fallback. Move the session onto it; the history is never written.
                    if let preferred = self.pickCamera(), preferred.uniqueID != self.activeCameraID {
                        self.stopCapture()
                        if self.mode == "face" || self.mode == "light_only" { self.scheduleSample(after: 0) }
                    }
                } else if self.mode == "face" || self.mode == "light_only", self.sampleTimer == nil {
                    // The built-in camera may be usable again, so sample now unless a timer is already pending.
                    self.scheduleSample(after: 0)
                }
            }
        }
    }

    func apply(_ snapshot: CoreSnapshot, core: CoreBridge) {
        self.core = core
        let nextMode = snapshot.camera?.mode ?? "disabled"
        let nextCaptureMode = snapshot.camera?.capture_mode ?? "single_frame"
        let nextGeneration = snapshot.camera?.generation ?? 0
        let nextCameraID = snapshot.settings?.selected_camera_id
        let nextCameraHistory = snapshot.settings?.camera_history ?? []
        // A history change counts as a camera change, like a changed selected_camera_id. With no history,
        // the legacy ID is the key, which matches the one-entry history it implies.
        let nextCameraKey = nextCameraHistory.isEmpty ? nextCameraID : nextCameraHistory.joined(separator: "\n")
        let nextAlwaysOn = snapshot.settings?.always_on_camera ?? true
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            if let revision = snapshot.revision {
                if let latest = self.latestSnapshotRevision, revision < latest { return }
                self.latestSnapshotRevision = revision
            }
            let previousGeneration = self.generation
            let previousMode = self.mode
            let previousCaptureMode = self.captureMode
            let policyChanged = nextGeneration != previousGeneration || nextMode != previousMode
                || nextCaptureMode != previousCaptureMode
            let stop = self.lifecycle.reconcile(mode: nextMode, captureMode: nextCaptureMode,
                generation: nextGeneration, cameraID: nextCameraKey, alwaysOn: nextAlwaysOn)
            if stop { self.stopCapture() }
            if !stop && policyChanged {
                self.sampleTimer?.cancel()
                self.sampleTimer = nil
                self.sampleTimerEpoch &+= 1
                self.settleTimer?.cancel()
                self.settleTimer = nil
                self.timeoutTimer?.cancel()
                self.timeoutTimer = nil
                self.lifecycle.cancelDeadlines()
                self.inferenceRequested = false
            }
            self.mode = nextMode
            self.captureMode = nextCaptureMode
            self.generation = nextGeneration
            // The core owns the retry schedule; the shell only obeys retry_in_ms.
            if let retryMs = snapshot.camera?.retry_in_ms {
                if self.retryTimer == nil || self.retryTimerGeneration != nextGeneration {
                    self.scheduleRetry(after: Double(retryMs) / 1000.0)
                }
            } else {
                self.retryTimer?.cancel()
                self.retryTimer = nil
                self.retryTimerEpoch &+= 1
            }
            self.selectedCameraID = nextCameraID
            self.cameraHistory = nextCameraHistory
            if self.mode == "face" {
                self.cancelDetectorRelease()
            } else {
                self.scheduleDetectorRelease()
            }
            // Pre-warm after scheduleSample: the deferred call runs at closure exit, so the sample deadline is
            // already set and detector creation falls inside the first-sample wait, on every exit path.
            let shouldPrewarmDetector = self.mode == "face" && previousMode != "face" && self.detector == nil
            defer { if shouldPrewarmDetector { _ = self.ensureDetector() } }
            guard self.mode == "face" || self.mode == "light_only" else {
                self.stopCapture()
                return
            }
            let canSchedule = self.captureMode == "continuous_return" || self.captureMode == "continuous_monitoring"
                ? !self.inferenceRequested
                : !self.sampling
            guard canSchedule, self.sampleTimer == nil else { return }
            let nextSampleMs = snapshot.camera?.next_sample_in_ms ?? 0
            wakeLog.debug("sample_scheduled next_sample_ms=\(nextSampleMs, privacy: .public) mode=\(self.mode, privacy: .public) at_ms=\(wakeMilliseconds(), privacy: .public)")
            self.scheduleSample(after: Double(nextSampleMs) / 1000.0)
        }
    }

    /// Replaces the pending sample timer. A newer schedule or a generation change drops the older timer.
    /// Leeway is 10% of the delay, at most one second, so wakeups can coalesce; the 5 s deadlines stay precise.
    private func scheduleSample(after delay: TimeInterval) {
        sampleTimerEpoch &+= 1
        let expectedTimerEpoch = sampleTimerEpoch
        let expectedGeneration = generation
        sampleTimer?.cancel()
        let clamped = max(0, delay)
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + clamped, leeway: .milliseconds(Int(min(0.1 * clamped, 1.0) * 1000)))
        timer.setEventHandler { [weak self] in
            guard let self, !self.stopped, self.sampleTimerEpoch == expectedTimerEpoch,
                  self.generation == expectedGeneration else { return }
            self.requestSample()
        }
        sampleTimer = timer
        timer.resume()
    }

    /// Arms the one-shot retry requested by the core, on the same pattern as scheduleSample. Camera queue only.
    private func scheduleRetry(after delay: TimeInterval) {
        retryTimerEpoch &+= 1
        let expectedTimerEpoch = retryTimerEpoch
        let expectedGeneration = generation
        retryTimerGeneration = generation
        retryTimer?.cancel()
        let clamped = max(0, delay)
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + clamped, leeway: .milliseconds(Int(min(0.1 * clamped, 1.0) * 1000)))
        timer.setEventHandler { [weak self] in
            guard let self, !self.stopped, self.retryTimerEpoch == expectedTimerEpoch,
                  self.generation == expectedGeneration else { return }
            self.retryTimer = nil
            self.resendUnknownStatusIfNeeded()
        }
        retryTimer = timer
        timer.resume()
    }

    private func requestSample() {
        sampleTimer = nil
        guard !stopped, mode == "face" || mode == "light_only" else { return }
        sampleStartedAt = CMClockGetTime(CMClockGetHostTimeClock()).seconds
        if captureMode == "continuous_return" || captureMode == "continuous_monitoring" {
            inferenceRequested = true
            sampleGeneration = generation
            if sampling && mode != "light_only" { armCaptureTimeout() }
        }
        if mode == "light_only" {
            sampleGeneration = generation
            startCapture()
            guard sampling else { return }
            lifecycle.beginLightAssessment(now: cameraClockNow())
            let assessmentToken = lifecycle.assessmentToken
            settleTimer?.cancel()
            let expectedGeneration = generation
            let settle = DispatchWorkItem { [weak self] in
                guard let self, !self.stopped, self.sampling, self.generation == expectedGeneration,
                      self.mode == "light_only", self.lifecycle.isCurrentAssessment(assessmentToken) else { return }
                guard !self.lifecycle.acquisitionPending else { return }
                self.armCaptureTimeout(assessmentToken: assessmentToken)
            }
            settleTimer = settle
            scheduleDeadline(after: 5, settle)
            return
        }
        sampleGeneration = generation
        let requestGeneration = generation
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: startCapture()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                guard let self else { return }
                self.queue.async {
                    guard !self.stopped, self.generation == requestGeneration, self.mode == "face" else { return }
                    if granted { self.startCapture() }
                    else { self.report(status: "unavailable", cameraStatus: "permission") }
                }
            }
        case .denied, .restricted: report(status: "unavailable", cameraStatus: "permission")
        @unknown default: report(status: "unavailable", cameraStatus: "error")
        }
    }

    /// The camera a session should use now: the first connected history entry, else the system default.
    /// Enumerated on every call, so a reconnected camera is used without a settings change. While the lid is closed
    /// the built-in camera is left out, as if it were disconnected, and the system default is never used because it
    /// may be the built-in camera. Shared by a fresh start and the lid-open check, so both follow one priority order.
    /// Reads the selection history and never writes it. Camera queue only.
    private func pickCamera() -> AVCaptureDevice? {
        let discovered = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera, .external], mediaType: .video, position: .unspecified).devices
        let candidates = discovered.map { CameraCandidate(id: $0.uniqueID, isBuiltIn: $0.deviceType == .builtInWideAngleCamera) }
        let connected = selectableCameraIDs(candidates, lidClosed: lidClosed)
        let devices = discovered.filter { connected.contains($0.uniqueID) }
        let resolvedID = resolveCamera(history: selectionHistory, connected: connected)
        let selected = devices.first { $0.uniqueID == resolvedID }
        return selected ?? (lidClosed ? devices.first : AVCaptureDevice.default(for: .video))
    }

    private func startCapture() {
        guard !sampling else { return }
        let capture = AVCaptureSession()
        capture.beginConfiguration()
        guard let camera = pickCamera(),
              let input = try? AVCaptureDeviceInput(device: camera), capture.canAddInput(input) else {
            capture.commitConfiguration(); report(status: "unavailable", cameraStatus: "unavailable"); return
        }
        capture.addInput(input)
        // The detector input is 640x640, so VGA is all the model can use. Fall back for cameras that lack it.
        if let preset = [AVCaptureSession.Preset.vga640x480, .medium, .low].first(where: { capture.canSetSessionPreset($0) }) {
            capture.sessionPreset = preset
        }
        let frames = AVCaptureVideoDataOutput()
        frames.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        frames.alwaysDiscardsLateVideoFrames = true
        guard capture.canAddOutput(frames) else {
            capture.commitConfiguration(); report(status: "unavailable", cameraStatus: "unavailable"); return
        }
        frames.setSampleBufferDelegate(self, queue: queue)
        capture.addOutput(frames)
        capture.commitConfiguration()
        session = capture; output = frames; sampling = true; receivedFirstFrame = false
        activeCameraID = camera.uniqueID
        activeDevice = camera
        if captureMode == "continuous_return" || captureMode == "continuous_monitoring" { inferenceRequested = true }
        lifecycle.beginAcquisition(now: cameraClockNow())
        armCaptureTimeout()
        wakeLog.debug("start_running preset=\(capture.sessionPreset.rawValue, privacy: .public) at_ms=\(wakeMilliseconds(), privacy: .public)")
        capture.startRunning()
        // The preset selects the active format only once the session runs, so the rate is matched to that format.
        // Retained sessions only: a single-frame session is too short to benefit, and the cap would just make the LED flicker at start.
        if captureMode == "continuous_return" || captureMode == "continuous_monitoring" { setCaptureRate() }
    }

    /// Escape hatch if a camera misbehaves with frame-rate control: `defaults write <bundle id> disableCaptureRateControl -bool true`.
    private var captureRateControlDisabled: Bool { UserDefaults.standard.bool(forKey: "disableCaptureRateControl") }

    /// Frame durations the device reported before the first cap of this session, restored when capture stops.
    private var originalFrameDurations: (minimum: CMTime, maximum: CMTime)?

    /// Caps the delivered frame rate of the open session. Best effort: it only ever writes a duration taken from,
    /// or strictly inside, a range the active format reports, because AVFoundation raises an Objective-C
    /// exception, which Swift cannot catch, for anything else. Must run after `startRunning`, when the session
    /// preset has selected the active format. Called once per session: changing the rate of a running camera
    /// restarts its stream and blinks the LED.
    private func setCaptureRate() {
        guard let device = activeDevice, !captureRateControlDisabled else { return }
        let supportedRanges = device.activeFormat.videoSupportedFrameRateRanges
        let ranges = supportedRanges.map { FrameRateRange(minimum: $0.minFrameRate, maximum: $0.maxFrameRate) }
        guard let choice = chooseFrameRate(ranges: ranges),
              supportedRanges.indices.contains(choice.rangeIndex) else { return }
        let supported = supportedRanges[choice.rangeIndex]
        let target: CMTime
        switch choice.edge {
        case .minimum: target = supported.maxFrameDuration   // lowest rate is the longest duration
        case .maximum: target = supported.minFrameDuration
        case .inside:
            let duration = frameDuration(forRate: choice.rate)
            target = CMTime(value: duration.value, timescale: duration.timescale)
        }
        guard CMTimeCompare(target, supported.minFrameDuration) >= 0,
              CMTimeCompare(target, supported.maxFrameDuration) <= 0 else {
            wakeLog.debug("capture_rate_skipped at_ms=\(wakeMilliseconds(), privacy: .public)")
            return
        }
        if originalFrameDurations == nil {
            originalFrameDurations = (device.activeVideoMinFrameDuration, device.activeVideoMaxFrameDuration)
        }
        if applyFrameDurations(minimum: target, maximum: target, to: device) {
            wakeLog.debug("capture_rate fps=\(choice.rate, privacy: .public) at_ms=\(wakeMilliseconds(), privacy: .public)")
        }
    }

    /// Restores the durations the device had before the first cap, so a cap never outlives the session or leaks
    /// into other apps. Runs before `stopRunning`, while the same format is still active. Best effort.
    private func resetCaptureRate() {
        defer { originalFrameDurations = nil }
        guard let device = activeDevice, let original = originalFrameDurations, !captureRateControlDisabled else { return }
        let supportedRanges = device.activeFormat.videoSupportedFrameRateRanges
        guard let shortest = supportedRanges.map(\.minFrameDuration).min(by: { CMTimeCompare($0, $1) < 0 }),
              let longest = supportedRanges.map(\.maxFrameDuration).max(by: { CMTimeCompare($0, $1) < 0 }),
              original.minimum.isValid, original.maximum.isValid,
              CMTimeCompare(original.minimum, shortest) >= 0,
              CMTimeCompare(original.maximum, longest) <= 0 else { return }
        applyFrameDurations(minimum: original.minimum, maximum: original.maximum, to: device)
    }

    /// Writes both frame durations in an order that keeps minimum <= maximum at every step. Best effort.
    @discardableResult
    private func applyFrameDurations(minimum: CMTime, maximum: CMTime, to device: AVCaptureDevice) -> Bool {
        guard minimum.isValid, maximum.isValid, CMTimeCompare(minimum, maximum) <= 0 else { return false }
        let currentMinimum = device.activeVideoMinFrameDuration
        let currentMaximum = device.activeVideoMaxFrameDuration
        guard currentMinimum.isValid, currentMaximum.isValid else { return false }
        if CMTimeCompare(currentMinimum, minimum) == 0, CMTimeCompare(currentMaximum, maximum) == 0 { return true }
        do {
            try device.lockForConfiguration()
            defer { device.unlockForConfiguration() }
            if CMTimeCompare(minimum, currentMaximum) > 0 {
                device.activeVideoMaxFrameDuration = maximum
                device.activeVideoMinFrameDuration = minimum
            } else {
                device.activeVideoMinFrameDuration = minimum
                device.activeVideoMaxFrameDuration = maximum
            }
            return true
        } catch {
            wakeLog.debug("capture_rate_failed at_ms=\(wakeMilliseconds(), privacy: .public)")
            return false
        }
    }

    /// Times a deadline on the utility queue and runs the item on the camera queue. Cancelling the item
    /// before the hop also cancels its work.
    private func scheduleDeadline(after delay: TimeInterval, _ item: DispatchWorkItem) {
        deadlineQueue.asyncAfter(deadline: .now() + max(0, delay)) { [weak self] in
            guard !item.isCancelled else { return }
            self?.queue.async(execute: item)
        }
    }

    /// Bounds initial acquisition and each requested return frame. No watchdog runs between
    /// healthy return deadlines, so an open but idle session is not mistaken for a failure.
    private func armCaptureTimeout(assessmentToken: UInt64? = nil) {
        timeoutTimer?.cancel()
        timeoutAssessmentToken = assessmentToken
        timeoutEpoch &+= 1
        let expectedTimeoutEpoch = timeoutEpoch
        let expectedGeneration = generation
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.sampling, self.generation == expectedGeneration,
                  self.timeoutEpoch == expectedTimeoutEpoch else { return }
            // Token-bound deadlines (light-only settle, face dark settle) are bounded by their assessment
            // deadline. The acquisition watchdog without a token still only fires before the first frame.
            if self.mode == "light_only" || assessmentToken != nil {
                if let assessmentToken {
                    guard self.lifecycle.isCurrentAssessment(assessmentToken) else { return }
                }
                guard self.lifecycle.timedOut(at: self.cameraClockNow()) else { return }
                self.stopCapture()
                self.report(status: "unavailable", cameraStatus: "busy")
                return
            }
            let stillWaiting = self.captureMode == "continuous_return" || self.captureMode == "continuous_monitoring"
                ? self.inferenceRequested
                : !self.receivedFirstFrame
            guard stillWaiting else { return }
            self.stopCapture()
            self.report(status: "unavailable", cameraStatus: "busy")
        }
        timeoutTimer = timeout
        scheduleDeadline(after: 5, timeout)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard !stopped, output === self.output, sampling else { return }
        if (captureMode == "continuous_return" || captureMode == "continuous_monitoring"), !inferenceRequested { return }
        guard let frame = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        guard let activeSession = session, let synchronizationClock = activeSession.synchronizationClock else { return }
        let presentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let frameTime = CMSyncConvertTime(presentationTime, from: synchronizationClock,
                                          to: CMClockGetHostTimeClock()).seconds
        guard frameTime.isFinite, frameTime >= sampleStartedAt else { return }
        if !receivedFirstFrame { wakeLog.debug("first_frame at_ms=\(wakeMilliseconds(), privacy: .public)") }
        receivedFirstFrame = true
        lifecycle.acquiredFrame()
        var keepSessionOpen = lifecycle.shouldKeepSession || captureMode == "continuous_return"
        let decision = lifecycle.frame(at: frameTime)
        if decision == .assessmentExpired {
            stopCapture()
            report(status: "unavailable", cameraStatus: "busy")
            return
        }
        if decision == .discardWarmup {
            if timeoutAssessmentToken == nil {
                timeoutTimer?.cancel(); timeoutTimer = nil
                timeoutEpoch &+= 1
            }
            return
        }
        timeoutTimer?.cancel(); timeoutTimer = nil
        timeoutAssessmentToken = nil
        timeoutEpoch &+= 1
        inferenceRequested = false
        defer { if !keepSessionOpen { stopCapture() } }
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        guard CVPixelBufferGetPixelFormatType(frame) == kCVPixelFormatType_32BGRA,
              let base = CVPixelBufferGetBaseAddress(frame) else {
            report(status: "unavailable", cameraStatus: "error"); return
        }
        let width = CVPixelBufferGetWidth(frame), height = CVPixelBufferGetHeight(frame)
        let stride = CVPixelBufferGetBytesPerRow(frame)
        let length = stride * height
        if decision == .assessLight {
            let code = tv_inference_assess_light_bgra(
                base.assumingMemoryBound(to: UInt8.self), length, UInt32(width), UInt32(height), stride)
            switch code {
            case 0: report(status: "usable_light", cameraStatus: "ready")
            case 2: report(status: "insufficient_light", cameraStatus: "ready")
            default: report(status: "unavailable", cameraStatus: "error")
            }
            return
        }
        guard ensureDetector() else { report(status: "unavailable", cameraStatus: "unavailable"); return }
        var result = TvInferenceResult(status: Int32(TV_INFERENCE_ERROR_RUNTIME), confidence: 0)
        let code = inferenceHost.detect(&result) { handle, output in
            self.detect(handle, bytes: base.assumingMemoryBound(to: UInt8.self), length: length,
                        width: UInt32(width), height: UInt32(height), stride: stride, result: &output)
        }
        detector = inferenceHost.handle
        guard let code, code >= 0 else {
            report(status: "unavailable", cameraStatus: "error"); return
        }
        let status: String
        switch result.status {
        case 1: status = "present"
        case 0: status = "absent"
        case 2: status = "insufficient_light"
        default: status = "unavailable"
        }
        // A dark single-frame sample gets one settle window; insufficient_light is reported only if it stays dark.
        if status == "insufficient_light", captureMode == "single_frame", lifecycle.beginDarkSettle(now: cameraClockNow()) {
            keepSessionOpen = true
            scheduleDarkSettleCheck()
            return
        }
        report(status: status, cameraStatus: status == "unavailable" ? "error" : "ready")
    }

    private func ensureDetector() -> Bool {
        guard inferenceHost.ensure() else { return false }
        detector = inferenceHost.handle
        wakeLog.debug("detector_ready at_ms=\(wakeMilliseconds(), privacy: .public)")
        return true
    }

    private func createDetector(provider: Int32, deviceIndex: UInt32, framework: String? = nil,
                                model: String? = nil) -> OpaquePointer? {
        let runtimePath = framework ?? Bundle.main.bundleURL.appendingPathComponent("Contents/Frameworks/libonnxruntime.dylib").path
        let modelPath = model ?? Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/models/face_detection_yunet_2023mar.onnx").path
        var created: OpaquePointer?
        let result = runtimePath.withCString { runtime in
            modelPath.withCString { model in
                tv_inference_create_with_provider_device(runtime, model, provider, deviceIndex, &created)
            }
        }
        return result == 0 ? created : nil
    }

    private func validate(_ candidate: OpaquePointer) -> InferenceDeviceSelection.FailureReason? {
        guard let frame = loadSelfTestFrame() else {
            return .selfTestMismatch
        }
        guard let reference = createDetector(provider: 0, deviceIndex: 0) else {
            return .selfTestMismatch
        }
        defer { tv_inference_destroy(reference) }
        var candidateResult = TvInferenceResult(status: Int32(TV_INFERENCE_ERROR_RUNTIME), confidence: 0)
        let started = DispatchTime.now().uptimeNanoseconds
        let candidateCode = frame.withUnsafeBytes { raw in
            detect(candidate, bytes: raw.baseAddress!.assumingMemoryBound(to: UInt8.self), length: raw.count,
                   width: 320, height: 240, stride: 1280, result: &candidateResult)
        }
        let elapsedMs = Double(DispatchTime.now().uptimeNanoseconds - started) / 1_000_000
        var referenceResult = TvInferenceResult(status: Int32(TV_INFERENCE_ERROR_RUNTIME), confidence: 0)
        let referenceCode = frame.withUnsafeBytes { raw in
            detect(reference, bytes: raw.baseAddress!.assumingMemoryBound(to: UInt8.self), length: raw.count,
                   width: 320, height: 240, stride: 1280, result: &referenceResult)
        }
        guard candidateCode >= 0, referenceCode >= 0,
              candidateResult.status == 1, referenceResult.status == 1 else {
            return .selfTestMismatch
        }
        guard elapsedMs <= InferenceDeviceSelection.latencyBudgetMilliseconds else {
            return .selfTestSlow
        }
        return nil
    }

    private func loadSelfTestFrame() -> Data? {
        guard let url = Bundle.main.url(forResource: "selftest-face", withExtension: "bgra"),
              let data = try? Data(contentsOf: url), data.count >= 16,
              data.prefix(4) == Data([0x54, 0x56, 0x53, 0x46]) else { return nil }
        func uint32(_ offset: Int) -> UInt32 {
            UInt32(data[offset]) | UInt32(data[offset + 1]) << 8 |
                UInt32(data[offset + 2]) << 16 | UInt32(data[offset + 3]) << 24
        }
        let width = uint32(4), height = uint32(8), stride = uint32(12)
        guard width == 320, height == 240, stride == 1280, data.count == 16 + Int(stride * height) else { return nil }
        return data.subdata(in: 16..<data.count)
    }

    private func detect(_ handle: OpaquePointer, bytes: UnsafePointer<UInt8>, length: Int, width: UInt32,
                        height: UInt32, stride: Int, result: inout TvInferenceResult) -> Int32 {
        tv_inference_detect_bgra(handle, bytes, length, width, height, stride, &result)
    }

    private func scheduleDetectorRelease() {
        guard detector != nil, detectorReleaseTimer == nil else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.detectorReleaseTimer = nil
            guard self.mode != "face" else { return }
            self.releaseDetector()
        }
        detectorReleaseTimer = work
        queue.asyncAfter(deadline: .now() + 30, execute: work)
    }

    private func cancelDetectorRelease() {
        detectorReleaseTimer?.cancel()
        detectorReleaseTimer = nil
    }

    private func releaseDetector() {
        guard detector != nil else { return }
        inferenceHost.release()
        self.detector = nil
    }

    private func report(status: String, cameraStatus: String) {
        guard !stopped, lifecycle.acceptsCallback(generation: sampleGeneration) else { return }
        let timestamp = Int64(Date().timeIntervalSince1970 * 1000)
        lastReportedCameraStatus = cameraStatus
        wakeLog.debug("presence_report status=\(status, privacy: .public) camera_status=\(cameraStatus, privacy: .public) at_ms=\(timestamp, privacy: .public)")
        core?.submit(["type": "presence_observation", "status": status,
                      "captured_at_ms": timestamp, "camera_generation": sampleGeneration,
                      "camera_status": cameraStatus])
    }

    private func cameraClockNow() -> TimeInterval {
        CMClockGetTime(CMClockGetHostTimeClock()).seconds
    }

    /// Ends a face-mode dark settle on the camera queue. Frames stay discarded until the policy deadline;
    /// after it, a stalled stream is bounded by the assessment deadline through the token-checked watchdog.
    private func scheduleDarkSettleCheck() {
        settleTimer?.cancel()
        let delay = max(0, (lifecycle.settlingUntil ?? cameraClockNow()) - cameraClockNow())
        let assessmentToken = lifecycle.assessmentToken
        let expectedGeneration = generation
        let settle = DispatchWorkItem { [weak self] in
            guard let self, !self.stopped, self.sampling, self.generation == expectedGeneration,
                  self.mode == "face", self.lifecycle.isCurrentAssessment(assessmentToken) else { return }
            self.armCaptureTimeout(assessmentToken: assessmentToken)
        }
        settleTimer = settle
        scheduleDeadline(after: delay, settle)
    }

    /// Runs on the camera queue. Restarts capture when the active camera leaves or a camera that outranks
    /// it in the history joins. Also resends an unknown camera status after a busy or unavailable report,
    /// so a camera that re-enumerates after wake is not held back by a sticky status.
    private func cameraDeviceChanged(_ device: AVCaptureDevice, connected: Bool) {
        guard !stopped else { return }
        if sampling, let activeID = activeCameraID {
            let history = selectionHistory
            let activeRank = history.firstIndex(of: activeID) ?? Int.max
            let affectsCapture = connected
                ? (history.firstIndex(of: device.uniqueID).map { $0 < activeRank } ?? false)
                : device.uniqueID == activeID
            if affectsCapture {
                stopCapture()
                if mode == "face" || mode == "light_only" { scheduleSample(after: 0) }
            }
        }
        resendUnknownStatusIfNeeded()
    }

    /// Resends an unknown camera status after a busy or unavailable report. Camera queue only.
    private func resendUnknownStatusIfNeeded() {
        guard let status = lastReportedCameraStatus, status == "busy" || status == "unavailable" else { return }
        lastReportedCameraStatus = "unknown"
        // presence_observation rejects status "unknown", so this uses the existing camera_status command.
        core?.submit(["type": "camera_status", "status": "unknown", "generation": generation])
    }

    private func stopCapture() {
        sampleTimer?.cancel(); sampleTimer = nil
        sampleTimerEpoch &+= 1
        timeoutTimer?.cancel(); timeoutTimer = nil
        timeoutAssessmentToken = nil
        settleTimer?.cancel(); settleTimer = nil
        output?.setSampleBufferDelegate(nil, queue: nil)
        resetCaptureRate()
        if session?.isRunning == true { session?.stopRunning() }
        output = nil; session = nil; sampling = false; activeCameraID = nil; activeDevice = nil
        inferenceRequested = false; receivedFirstFrame = false
        lifecycle.cancelDeadlines()
    }

    func shutdown() {
        queue.sync {
            stopped = true
            stopCapture()
            retryTimer?.cancel()
            retryTimer = nil
            cancelDetectorRelease()
            releaseDetector()
        }
    }
}
