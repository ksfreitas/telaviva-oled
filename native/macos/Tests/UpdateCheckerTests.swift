import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("UpdateCheckerTests: \(message)\n".utf8))
        exit(1)
    }
}

/// Returns a scripted reply or throws, and records every request it receives.
private final class FakeFetcher: UpdateFetching, @unchecked Sendable {
    enum Reply {
        case response(Int, Data)
        case failure
    }

    private let lock = NSLock()
    private let reply: Reply
    private var callCount = 0
    private var lastRequest: URLRequest?

    init(reply: Reply) {
        self.reply = reply
    }

    var calls: Int {
        locked { callCount }
    }

    var request: URLRequest? {
        locked { lastRequest }
    }

    func fetch(_ request: URLRequest) async throws -> (status: Int, body: Data) {
        let current = record(request)
        switch current {
        case let .response(status, body):
            return (status, body)
        case .failure:
            throw URLError(.notConnectedToInternet)
        }
    }

    private func record(_ request: URLRequest) -> Reply {
        locked { () -> Reply in
            callCount += 1
            lastRequest = request
            return reply
        }
    }

    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

/// A clock the test moves forward by hand.
private final class FakeClock {
    var date: Date

    init(_ date: Date) {
        self.date = date
    }
}

private struct Harness {
    let checker: UpdateChecker
    let fetcher: FakeFetcher
    let clock: FakeClock
}

@main
@MainActor
private struct UpdateCheckerTests {
    static func main() async {
        initialStateIsIdle()
        await checkIfNeededRunsOneFetchAndSettles()
        await successStatesAreReusedForSixHours()
        await failureStatesAreReusedForThirtySeconds()
        await checkNowIsThrottledForThirtySeconds()
        await noSecondFetchWhileChecking()
        await fetchErrorBecomesFailed()
        await nilEvaluationBecomesFailed()
        await unknownStatusBecomesFailed()
        everyStatusMapsThroughStateFrom()
        unusableUpdateOutcomesAreFailed()
        await requestHasTheContractedShape()
        print("Update checker tests passed")
    }

    private static func initialStateIsIdle() {
        let harness = makeHarness(reply: .response(200, Data()), outcome: makeOutcome("up_to_date", remote: "0.1.0"))
        expect(harness.checker.state == .idle, "a new checker starts idle")
        expect(harness.fetcher.calls == 0, "a new checker does not fetch before About asks")
    }

    private static func checkIfNeededRunsOneFetchAndSettles() async {
        let harness = makeHarness(reply: .response(200, Data()), outcome: makeOutcome("up_to_date", remote: "0.1.0"))
        harness.checker.checkIfNeeded()
        expect(harness.checker.state == .checking, "checking starts immediately when About appears")
        let settled = await waitUntilSettled(harness.checker)
        expect(settled && harness.checker.state == .upToDate(remote: "0.1.0"), "the check settles as up to date")
        expect(harness.fetcher.calls == 1, "one check performs one fetch")
    }

    private static func successStatesAreReusedForSixHours() async {
        let lifetime: TimeInterval = 6 * 3600
        await expectReuse(reply: .response(200, Data()), outcome: makeOutcome("up_to_date", remote: "0.1.0"),
                          expected: .upToDate(remote: "0.1.0"), lifetime: lifetime, label: "up to date")
        await expectReuse(reply: .response(200, Data()),
                          outcome: makeOutcome("update_available", remote: "0.2.0", url: releaseURLString),
                          expected: .updateAvailable(remote: "0.2.0", url: releaseURL), lifetime: lifetime,
                          label: "update available")
        await expectReuse(reply: .response(404, Data()), outcome: makeOutcome("no_release"),
                          expected: .noRelease, lifetime: lifetime, label: "no release")
    }

    private static func failureStatesAreReusedForThirtySeconds() async {
        await expectReuse(reply: .failure, outcome: nil, expected: .failed, lifetime: 30, label: "failed")
        await expectReuse(reply: .response(403, Data()), outcome: makeOutcome("rate_limited"),
                          expected: .rateLimited, lifetime: 30, label: "rate limited")
    }

    private static func checkNowIsThrottledForThirtySeconds() async {
        let harness = makeHarness(reply: .response(200, Data()), outcome: makeOutcome("up_to_date", remote: "0.1.0"))
        harness.checker.checkNow()
        let first = await waitUntilSettled(harness.checker)
        expect(first && harness.fetcher.calls == 1, "the first check now runs one fetch")

        harness.clock.date.addTimeInterval(10)
        harness.checker.checkNow()
        expect(harness.checker.state != .checking && harness.fetcher.calls == 1,
               "check now within 30 s of the last attempt is ignored")

        harness.clock.date.addTimeInterval(20)
        harness.checker.checkNow()
        expect(harness.checker.state == .checking, "check now 30 s after the last attempt starts a fetch")
        let second = await waitUntilSettled(harness.checker)
        expect(second && harness.fetcher.calls == 2, "check now after the throttle runs a second fetch")
    }

    private static func noSecondFetchWhileChecking() async {
        let harness = makeHarness(reply: .response(200, Data()), outcome: makeOutcome("no_release"))
        harness.checker.checkIfNeeded()
        harness.checker.checkIfNeeded()
        harness.checker.checkNow()
        expect(harness.checker.state == .checking, "repeated requests keep the checker in checking")
        let settled = await waitUntilSettled(harness.checker)
        expect(settled && harness.checker.state == .noRelease && harness.fetcher.calls == 1,
               "requests made while checking do not start a second fetch")
    }

    private static func fetchErrorBecomesFailed() async {
        let harness = makeHarness(reply: .failure, outcome: nil)
        harness.checker.checkIfNeeded()
        let settled = await waitUntilSettled(harness.checker)
        expect(settled && harness.checker.state == .failed, "a thrown fetch error settles as failed")
    }

    private static func nilEvaluationBecomesFailed() async {
        let harness = makeHarness(reply: .response(200, Data("{}".utf8)), outcome: nil)
        harness.checker.checkIfNeeded()
        let settled = await waitUntilSettled(harness.checker)
        expect(settled && harness.checker.state == .failed, "an evaluation that returns nil settles as failed")
    }

    private static func unknownStatusBecomesFailed() async {
        let harness = makeHarness(reply: .response(200, Data("{}".utf8)), outcome: makeOutcome("mystery"))
        harness.checker.checkIfNeeded()
        let settled = await waitUntilSettled(harness.checker)
        expect(settled && harness.checker.state == .failed, "an unknown status settles as failed")
    }

    private static func everyStatusMapsThroughStateFrom() {
        expect(UpdateChecker.state(from: nil) == .failed, "a missing outcome is failed")
        expect(UpdateChecker.state(from: makeOutcome("update_available", remote: "0.2.0", url: releaseURLString))
               == .updateAvailable(remote: "0.2.0", url: releaseURL),
               "update_available with a version and an https release URL is offered")
        expect(UpdateChecker.state(from: makeOutcome("up_to_date", remote: "0.1.0")) == .upToDate(remote: "0.1.0"),
               "up_to_date keeps the remote version")
        expect(UpdateChecker.state(from: makeOutcome("up_to_date")) == .upToDate(remote: ""),
               "up_to_date without a remote version has an empty remote")
        expect(UpdateChecker.state(from: makeOutcome("no_release")) == .noRelease, "no_release maps to noRelease")
        expect(UpdateChecker.state(from: makeOutcome("rate_limited")) == .rateLimited, "rate_limited maps to rateLimited")
        expect(UpdateChecker.state(from: makeOutcome("http_error")) == .failed, "http_error maps to failed")
        expect(UpdateChecker.state(from: makeOutcome("invalid_response")) == .failed, "invalid_response maps to failed")
        expect(UpdateChecker.state(from: makeOutcome("mystery")) == .failed, "an unknown status maps to failed")
    }

    private static func unusableUpdateOutcomesAreFailed() {
        let badURLs = [
            "http://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0",
            "not a url",
            "https://",
            "/releases/tag/v0.2.0",
        ]
        for badURL in badURLs {
            expect(UpdateChecker.state(from: makeOutcome("update_available", remote: "0.2.0", url: badURL)) == .failed,
                   "an update with a non-https or malformed URL is failed: \(badURL)")
        }
        expect(UpdateChecker.state(from: makeOutcome("update_available", remote: nil, url: releaseURLString)) == .failed,
               "an update without a remote version is failed")
        expect(UpdateChecker.state(from: makeOutcome("update_available", remote: "0.2.0", url: nil)) == .failed,
               "an update without a release URL is failed")
    }

    private static func requestHasTheContractedShape() async {
        let harness = makeHarness(reply: .response(200, Data()), outcome: makeOutcome("up_to_date", remote: "0.1.0"))
        harness.checker.checkIfNeeded()
        let settled = await waitUntilSettled(harness.checker)
        expect(settled, "the request check settles")
        guard let request = harness.fetcher.request else {
            expect(false, "the fetcher received a request")
            return
        }
        expect(UpdateChecker.endpoint.absoluteString
               == "https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest",
               "the endpoint is the GitHub latest-release URL")
        expect(request.url == UpdateChecker.endpoint, "the request targets the endpoint")
        expect(request.httpMethod == "GET", "the request is a GET")
        expect(request.value(forHTTPHeaderField: "Accept") == "application/vnd.github+json",
               "the request asks for the GitHub JSON media type")
        expect(request.value(forHTTPHeaderField: "X-GitHub-Api-Version") == "2022-11-28",
               "the request pins the GitHub API version")
        expect(request.value(forHTTPHeaderField: "User-Agent") == "TelaVivaOLED/0.1.0",
               "the User-Agent carries only the installed version")
        expect(request.value(forHTTPHeaderField: "Authorization") == nil, "the request sends no Authorization header")
        expect(request.value(forHTTPHeaderField: "Cookie") == nil, "the request sends no Cookie header")
        expect(request.httpBody == nil, "the request has no body")
        expect(request.timeoutInterval == 10, "the request times out after 10 seconds")
        expect(!request.httpShouldHandleCookies, "the request does not handle cookies")
    }

    private static let releaseURLString = "https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0"
    private static let releaseURL = URL(string: releaseURLString)!

    private static func makeHarness(reply: FakeFetcher.Reply, outcome: UpdateOutcome?) -> Harness {
        let clock = FakeClock(Date(timeIntervalSince1970: 1_800_000_000))
        let fetcher = FakeFetcher(reply: reply)
        let checker = UpdateChecker(installedVersion: "0.1.0", fetcher: fetcher,
                                    evaluate: { _, _, _ in outcome },
                                    now: { clock.date })
        return Harness(checker: checker, fetcher: fetcher, clock: clock)
    }

    private static func makeOutcome(_ status: String, remote: String? = nil, url: String? = nil) -> UpdateOutcome {
        UpdateOutcome(status: status, remoteVersion: remote, releaseURL: url)
    }

    /// Settles one check, then moves the clock: reused just inside the lifetime, refetched just after it.
    private static func expectReuse(reply: FakeFetcher.Reply, outcome: UpdateOutcome?, expected: UpdateState,
                                    lifetime: TimeInterval, label: String) async {
        let harness = makeHarness(reply: reply, outcome: outcome)
        harness.checker.checkIfNeeded()
        let first = await waitUntilSettled(harness.checker)
        expect(first && harness.checker.state == expected, "\(label): the first check settles as expected")

        harness.clock.date.addTimeInterval(lifetime - 1)
        harness.checker.checkIfNeeded()
        expect(harness.fetcher.calls == 1 && harness.checker.state == expected,
               "\(label): the result is reused just before its lifetime ends")

        harness.clock.date.addTimeInterval(2)
        harness.checker.checkIfNeeded()
        expect(harness.checker.state == .checking, "\(label): a check after the lifetime starts a fetch")
        let second = await waitUntilSettled(harness.checker)
        expect(second && harness.fetcher.calls == 2, "\(label): exactly one more fetch after the lifetime")
    }

    /// Polls every 10 ms, for at most 2 s, until the checker leaves `checking`. Returns true when it settled.
    private static func waitUntilSettled(_ checker: UpdateChecker) async -> Bool {
        var attempts = 0
        while checker.state == .checking && attempts < 200 {
            try? await Task.sleep(nanoseconds: 10_000_000)
            attempts += 1
        }
        return checker.state != .checking
    }
}
