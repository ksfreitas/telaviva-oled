import Foundation
import Combine

/// What the About screen shows for the update check.
enum UpdateState: Equatable {
    case idle
    case checking
    case upToDate(remote: String)
    case updateAvailable(remote: String, url: URL)
    case noRelease
    case rateLimited
    case failed
}

/// The core's UpdateOutcome JSON, decoded. Only `status` is always present.
struct UpdateOutcome: Equatable {
    let status: String
    let remoteVersion: String?
    let releaseURL: String?
}

/// Performs the HTTPS GET and returns the status and body; throws on transport failure.
protocol UpdateFetching {
    func fetch(_ request: URLRequest) async throws -> (status: Int, body: Data)
}

/// Runs the update check and caches its result. Lives on the main actor so the UI reads `state` directly.
@MainActor
final class UpdateChecker: ObservableObject {
    static let endpoint = URL(string: "https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest")!

    private static let successLifetime: TimeInterval = 6 * 3600
    private static let failureLifetime: TimeInterval = 30

    @Published private(set) var state: UpdateState = .idle

    private let installedVersion: String
    private let fetcher: UpdateFetching
    private let evaluate: (String, Int, Data) -> UpdateOutcome?
    private let now: () -> Date
    private var lastAttempt: Date?
    private var settledAt: Date?

    init(installedVersion: String, fetcher: UpdateFetching,
         evaluate: @escaping (String, Int, Data) -> UpdateOutcome?,
         now: @escaping () -> Date = Date.init) {
        self.installedVersion = installedVersion
        self.fetcher = fetcher
        self.evaluate = evaluate
        self.now = now
    }

    /// Called when About appears. Reuses a settled result while it is still fresh.
    func checkIfNeeded() {
        guard state != .checking else { return }
        if let settledAt, now().timeIntervalSince(settledAt) < reuseLifetime(for: state) { return }
        startCheck()
    }

    /// Called by the "Check now" button. Ignored while a check runs or within 30 s of the last attempt.
    func checkNow() {
        guard state != .checking else { return }
        if let lastAttempt, now().timeIntervalSince(lastAttempt) < Self.failureLifetime { return }
        startCheck()
    }

    /// Maps the core's outcome to a state. Anything unusable is `.failed`.
    static func state(from outcome: UpdateOutcome?) -> UpdateState {
        guard let outcome else { return .failed }
        switch outcome.status {
        case "update_available":
            guard let remote = outcome.remoteVersion, !remote.isEmpty,
                  let text = outcome.releaseURL, let url = URL(string: text),
                  url.scheme?.lowercased() == "https", url.host?.isEmpty == false else { return .failed }
            return .updateAvailable(remote: remote, url: url)
        case "up_to_date":
            return .upToDate(remote: outcome.remoteVersion ?? "")
        case "no_release":
            return .noRelease
        case "rate_limited":
            return .rateLimited
        default:
            return .failed
        }
    }

    private func reuseLifetime(for state: UpdateState) -> TimeInterval {
        switch state {
        case .upToDate, .updateAvailable, .noRelease:
            return Self.successLifetime
        default:
            return Self.failureLifetime
        }
    }

    private func startCheck() {
        let installed = installedVersion
        let request = Self.makeRequest(installedVersion: installed)
        let fetcher = self.fetcher
        let evaluate = self.evaluate
        lastAttempt = now()
        state = .checking
        Task { [weak self] in
            let result: UpdateState
            do {
                let response = try await fetcher.fetch(request)
                result = UpdateChecker.state(from: evaluate(installed, response.status, response.body))
            } catch {
                result = .failed
            }
            self?.settle(result)
        }
    }

    private func settle(_ result: UpdateState) {
        state = result
        settledAt = now()
    }

    private static func makeRequest(installedVersion: String) -> URLRequest {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "GET"
        request.timeoutInterval = 10
        request.httpShouldHandleCookies = false
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        request.setValue("TelaVivaOLED/\(installedVersion)", forHTTPHeaderField: "User-Agent")
        return request
    }
}

/// Production fetcher: ephemeral session with no cookies, no cache and a 1 MiB body cap.
struct URLSessionUpdateFetcher: UpdateFetching {
    private static let maxBodyBytes = 1_048_576

    private let session: URLSession

    init() {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.urlCache = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 10
        configuration.timeoutIntervalForResource = 10
        configuration.waitsForConnectivity = false
        session = URLSession(configuration: configuration)
    }

    func fetch(_ request: URLRequest) async throws -> (status: Int, body: Data) {
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        guard data.count <= Self.maxBodyBytes else { throw URLError(.dataLengthExceedsMaximum) }
        return (http.statusCode, data)
    }
}

#if TELAVIVA_CORE
/// Calls the Rust core for the version and the pure update comparison.
enum CoreUpdate {
    static func version() -> String? {
        var buffer = TvBuffer(data: nil, len: 0)
        defer { tv_buffer_free(&buffer) }
        guard tv_core_version(&buffer) == 0, let data = buffer.data else { return nil }
        return String(data: Data(bytes: data, count: buffer.len), encoding: .utf8)
    }

    static func evaluate(installed: String, status: Int, body: Data) -> UpdateOutcome? {
        let installedBytes = Array(installed.utf8)
        var buffer = TvBuffer(data: nil, len: 0)
        defer { tv_buffer_free(&buffer) }
        let code: Int32 = installedBytes.withUnsafeBufferPointer { installedBuffer in
            body.withUnsafeBytes { bodyBytes -> Int32 in
                // An empty body is passed as a null pointer with length 0.
                let bodyPointer: UnsafePointer<UInt8>? = bodyBytes.isEmpty
                    ? nil : bodyBytes.baseAddress?.assumingMemoryBound(to: UInt8.self)
                return tv_update_evaluate(installedBuffer.baseAddress, installedBuffer.count,
                                          UInt32(clamping: status), bodyPointer, bodyBytes.count, &buffer)
            }
        }
        guard code == 0, let data = buffer.data else { return nil }
        let json = Data(bytes: data, count: buffer.len)
        guard let object = (try? JSONSerialization.jsonObject(with: json)) as? [String: Any],
              let statusText = object["status"] as? String else { return nil }
        return UpdateOutcome(status: statusText,
                             remoteVersion: object["remote_version"] as? String,
                             releaseURL: object["release_url"] as? String)
    }
}
#endif
