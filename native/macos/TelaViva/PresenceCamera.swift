import AVFoundation
import CoreVideo
import Foundation

/// Uses one-frame sessions while present and holds a shared, read-only session during return checks.
/// Return frames are discarded between deadlines; frame bytes never enter the ABI.
final class PresenceCamera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let queue = DispatchQueue(label: "com.telaviva.camera", qos: .utility)
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
    private var sampleTimer: DispatchWorkItem?
    private var timeoutTimer: DispatchWorkItem?
    private var detectorReleaseTimer: DispatchWorkItem?
    private var detector: OpaquePointer?

    func apply(_ snapshot: CoreSnapshot, core: CoreBridge) {
        self.core = core
        let nextMode = snapshot.camera?.mode ?? "disabled"
        let nextCaptureMode = snapshot.camera?.capture_mode ?? "single_frame"
        let nextGeneration = snapshot.camera?.generation ?? 0
        let nextCameraID = snapshot.settings?.selected_camera_id
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            if nextMode != self.mode || nextCaptureMode != self.captureMode
                || nextGeneration != self.generation || nextCameraID != self.selectedCameraID {
                self.stopCapture()
                self.mode = nextMode
                self.captureMode = nextCaptureMode
                self.generation = nextGeneration
                self.selectedCameraID = nextCameraID
            }
            if self.mode == "face" {
                self.cancelDetectorRelease()
            } else {
                self.scheduleDetectorRelease()
            }
            guard self.mode == "face" || self.mode == "light_only" else {
                self.stopCapture()
                return
            }
            let canSchedule = self.captureMode == "continuous_return"
                ? !self.inferenceRequested
                : !self.sampling
            guard canSchedule, self.sampleTimer == nil else { return }
            let delay = Double(snapshot.camera?.next_sample_in_ms ?? 0) / 1000.0
            let work = DispatchWorkItem { [weak self] in self?.requestSample() }
            self.sampleTimer = work
            self.queue.asyncAfter(deadline: .now() + max(0, delay), execute: work)
        }
    }

    private func requestSample() {
        sampleTimer = nil
        guard !stopped, mode == "face" || mode == "light_only" else { return }
        if captureMode == "continuous_return" {
            inferenceRequested = true
            if sampling { armCaptureTimeout() }
        }
        if mode == "light_only" { startCapture(); return }
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

    private func startCapture() {
        guard !sampling else { return }
        let capture = AVCaptureSession()
        capture.beginConfiguration()
        capture.sessionPreset = .vga640x480
        let selected = selectedCameraID.flatMap { AVCaptureDevice(uniqueID: $0) }
        guard let camera = selected ?? AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: camera), capture.canAddInput(input) else {
            capture.commitConfiguration(); report(status: "unavailable", cameraStatus: "unavailable"); return
        }
        capture.addInput(input)
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
        if captureMode == "continuous_return" { inferenceRequested = true }
        armCaptureTimeout()
        capture.startRunning()
    }

    /// Bounds initial acquisition and each requested return frame. No watchdog runs between
    /// healthy return deadlines, so an open but idle session is not mistaken for a failure.
    private func armCaptureTimeout() {
        timeoutTimer?.cancel()
        let expectedGeneration = generation
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.sampling, self.generation == expectedGeneration else { return }
            let stillWaiting = self.captureMode == "continuous_return"
                ? self.inferenceRequested
                : !self.receivedFirstFrame
            guard stillWaiting else { return }
            self.stopCapture()
            self.report(status: "unavailable", cameraStatus: "busy")
        }
        timeoutTimer = timeout
        queue.asyncAfter(deadline: .now() + 5, execute: timeout)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard !stopped, output === self.output, sampling else { return }
        if captureMode == "continuous_return", !inferenceRequested { return }
        guard let frame = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        inferenceRequested = false
        receivedFirstFrame = true
        timeoutTimer?.cancel(); timeoutTimer = nil
        let keepSessionOpen = captureMode == "continuous_return"
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
        if mode == "light_only" {
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
        let code = tv_inference_detect_bgra(detector!, base.assumingMemoryBound(to: UInt8.self), length, UInt32(width), UInt32(height), stride, &result)
        guard code >= 0 else { report(status: "unavailable", cameraStatus: "error"); return }
        let status: String
        switch result.status {
        case 1: status = "present"
        case 0: status = "absent"
        case 2: status = "insufficient_light"
        default: status = "unavailable"
        }
        report(status: status, cameraStatus: status == "unavailable" ? "error" : "ready")
        if keepSessionOpen && status == "present" { stopCapture() }
    }

    private func ensureDetector() -> Bool {
        if detector != nil { return true }
        let framework = Bundle.main.bundleURL.appendingPathComponent("Contents/Frameworks/libonnxruntime.dylib").path
        let model = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/models/face_detection_yunet_2023mar.onnx").path
        var created: OpaquePointer?
        let result = framework.withCString { runtimePath in
            model.withCString { modelPath in tv_inference_create(runtimePath, modelPath, &created) }
        }
        guard result == 0, let created else { return false }
        detector = created
        return true
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
        guard let detector else { return }
        tv_inference_destroy(detector)
        self.detector = nil
    }

    private func report(status: String, cameraStatus: String) {
        let timestamp = Int64(Date().timeIntervalSince1970 * 1000)
        core?.submit(["type": "presence_observation", "status": status,
                      "captured_at_ms": timestamp, "camera_generation": generation,
                      "camera_status": cameraStatus])
    }

    private func stopCapture() {
        sampleTimer?.cancel(); sampleTimer = nil
        timeoutTimer?.cancel(); timeoutTimer = nil
        output?.setSampleBufferDelegate(nil, queue: nil)
        if session?.isRunning == true { session?.stopRunning() }
        output = nil; session = nil; sampling = false
        inferenceRequested = false; receivedFirstFrame = false
    }

    func shutdown() {
        queue.sync {
            stopped = true
            stopCapture()
            cancelDetectorRelease()
            releaseDetector()
        }
    }
}
