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
        expect(
            AppText.cameraInlineStatus(status: "suspended_low_brightness", locale: "pt-BR"),
            equals: "Câmera: suspensa com brilho baixo"
        )
        expect(
            AppText.cameraInlineStatus(status: "suspended_low_brightness", locale: "en-US"),
            equals: "Camera: suspended at low brightness"
        )
        expectMatchingMessageKeys()
    }

    private static func expectMatchingMessageKeys() {
        guard let enUS = AppText.translations["en-US"], let ptBR = AppText.translations["pt-BR"] else {
            fputs("Missing en-US or pt-BR message dictionary\n", stderr)
            exit(1)
        }
        let missingInPtBR = Set(enUS.keys).subtracting(ptBR.keys).sorted()
        let missingInEnUS = Set(ptBR.keys).subtracting(enUS.keys).sorted()
        guard missingInPtBR.isEmpty && missingInEnUS.isEmpty else {
            fputs("Message keys differ. Missing in pt-BR: \(missingInPtBR). Missing in en-US: \(missingInEnUS)\n", stderr)
            exit(1)
        }
    }

    private static func expect(_ actual: String, equals expected: String) {
        guard actual == expected else {
            fputs("Expected `\(expected)`, received `\(actual)`\n", stderr)
            exit(1)
        }
    }
}
