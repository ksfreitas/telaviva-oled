import SwiftUI
import AppKit

/// About page: app identity, project and credits, and the on-demand update check.
struct AboutView: View {
    static let projectURL = URL(string: "https://github.com/ksfreitas/telaviva-oled")!
    static let architect = "Cássio de Freitas e Silva"
    static let productName = "TelaViva OLED"

    let installedVersion: String
    let locale: String
    @ObservedObject var checker: UpdateChecker

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                Image(nsImage: NSApplication.shared.applicationIconImage)
                    .resizable()
                    .frame(width: 96, height: 96)
                    .accessibilityHidden(true)
                Text(AboutView.productName)
                    .font(.title2.bold())
                Text("\(string("about.version")) \(installedVersion)")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
                creditsCard
                updatesSection
            }
            .frame(maxWidth: 480)
            .frame(maxWidth: .infinity)
            .padding(24)
        }
        .onAppear { checker.checkIfNeeded() }
    }

    private func string(_ key: String) -> String {
        AppText.string(key, locale: locale)
    }

    /// Release URL offered only when an update is available.
    private var releaseURL: URL? {
        if case .updateAvailable(_, let url) = checker.state {
            return url
        }
        return nil
    }

    private var creditsCard: some View {
        VStack(spacing: 12) {
            infoRow(string("about.project")) {
                Link(AboutView.projectURL.absoluteString, destination: AboutView.projectURL)
            }
            infoRow(string("about.credits.architect")) {
                Text(AboutView.architect)
            }
            infoRow(string("about.credits.coder")) {
                Text(string("about.credits.coders"))
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity)
        .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
    }

    /// Label on the left in secondary style, value on the right.
    private func infoRow<Value: View>(_ label: String, @ViewBuilder value: () -> Value) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
            Spacer(minLength: 16)
            value()
                .multilineTextAlignment(.trailing)
        }
        .accessibilityElement(children: .combine)
    }

    private var updatesSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(string("about.update.title"))
                .font(.headline)
            VStack(alignment: .leading, spacing: 12) {
                statusRow
                HStack(spacing: 8) {
                    Button(string("about.update.checkNow")) {
                        checker.checkNow()
                    }
                    .disabled(checker.state == .checking)
                    if let url = releaseURL {
                        Button(string("about.update.openRelease")) {
                            _ = NSWorkspace.shared.open(url)
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// Icon (nil shows a spinner), tint and text for the current update state.
    private var statusLine: (symbol: String?, tint: Color, text: String) {
        switch checker.state {
        case .idle, .checking:
            return (nil, .secondary, string("about.update.checking"))
        case .upToDate:
            return ("checkmark.circle.fill", .green, string("about.update.upToDate"))
        case .updateAvailable(let remote, _):
            let text = string("about.update.available").replacingOccurrences(of: "{version}", with: remote)
            return ("arrow.down.circle.fill", .accentColor, text)
        case .noRelease:
            return ("info.circle", .secondary, string("about.update.noRelease"))
        case .rateLimited:
            return ("exclamationmark.triangle.fill", .orange, string("about.update.rateLimited"))
        case .failed:
            return ("exclamationmark.triangle", .secondary, string("about.update.failed"))
        }
    }

    private var statusRow: some View {
        let line = statusLine
        return HStack(spacing: 8) {
            if let symbol = line.symbol {
                Image(systemName: symbol)
                    .foregroundStyle(line.tint)
                    .accessibilityHidden(true)
            } else {
                ProgressView()
                    .controlSize(.small)
            }
            Text(line.text)
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }
}
