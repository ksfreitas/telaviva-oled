import AppKit
import SwiftUI
import Darwin

@main
struct TelaVivaApp: App {
    @NSApplicationDelegateAdaptor(MenuBarController.self) private var appDelegate

    init() {
        if CommandLine.arguments.contains("--list-displays") || CommandLine.arguments.contains("--diagnostics") {
            let diagnostics = CommandLine.arguments.contains("--diagnostics")
            let controller = MacDDCController(core: CoreBridge())
            let finished = DispatchSemaphore(value: 0)
            controller.probeDisplays { displays, report in
                let outputObject: Any = diagnostics ? ["diagnostics": report, "monitors": displays] : displays
                if let data = try? JSONSerialization.data(withJSONObject: outputObject, options: [.prettyPrinted, .sortedKeys]),
                   let output = String(data: data, encoding: .utf8) {
                    print(output)
                } else {
                    print(diagnostics ? "{}" : "[]")
                }
                finished.signal()
            }
            if finished.wait(timeout: .now() + 45) == .timedOut {
                fputs("TelaViva: DDC probe timed out\n", stderr)
                exit(2)
            }
            exit(0)
        }
        if let probeIndex = CommandLine.arguments.firstIndex(of: "--probe-media-keys") {
            let next = CommandLine.arguments.index(after: probeIndex)
            let seconds = next < CommandLine.arguments.count ? Int(CommandLine.arguments[next]) : nil
            exit(MediaKeyProbe.run(seconds: seconds ?? 20))
        }
        if CommandLine.arguments.contains("--smoke-test") {
            #if TELAVIVA_CORE
            exit(Int32(CoreBridge.smokeTest()))
            #else
            exit(64)
            #endif
        }
    }

    var body: some Scene {
        Settings {
            SettingsView(core: appDelegate.core)
                .frame(minWidth: 420, minHeight: 420)
        }
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
