import Foundation

@main
enum CameraLocalizationTests {
    static func main() {
        expect(
            AppText.cameraInlineStatus(status: "ready", locale: "pt-BR"),
            equals: "Câmera: funcionando"
        )
        expect(
            AppText.cameraInlineStatus(status: "ready", locale: "en-US"),
            equals: "Camera: functioning"
        )
    }

    private static func expect(_ actual: String, equals expected: String) {
        guard actual == expected else {
            fputs("Expected `\(expected)`, received `\(actual)`\n", stderr)
            exit(1)
        }
    }
}
