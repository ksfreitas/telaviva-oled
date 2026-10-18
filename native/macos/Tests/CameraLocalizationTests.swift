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
        expect(AppText.inferenceActiveLabel(id: "auto", fallback: "Automatic", locale: "en-US"),
               equals: "Automatic (CoreML preference)")
        expect(AppText.inferenceActiveLabel(id: "auto", fallback: "Automatic", locale: "pt-BR"),
               equals: "Automático (preferência CoreML)")
        expect(AppText.inferenceActiveLabel(id: "cpu", fallback: "CPU", locale: "pt-BR"), equals: "CPU")
        expect(AppText.inferenceActiveLabel(id: "ane", fallback: "Neural Engine", locale: "en-US"),
               equals: "Neural Engine preference")
        expect(AppText.inferenceActiveLabel(id: "gpu", fallback: "GPU", locale: "pt-BR"),
               equals: "GPU (preferência)")
        expect(AppText.string("inference.help", locale: "en-US"),
               equals: "Automatic uses validated CoreML and falls back to the CPU if it fails.")
        expect(AppText.string("inference.help", locale: "pt-BR"),
               equals: "O modo Automático usa CoreML validado e retorna à CPU em caso de falha.")
        expect(AppText.inferenceUnavailableLabel(id: "gpu:1002:164e:0", locale: "en-US"),
               equals: "Unavailable device: gpu:1002:164e:0")
        expect(AppText.inferenceUnavailableLabel(id: "gpu:1002:164e:0", locale: "pt-BR"),
               equals: "Dispositivo indisponível: gpu:1002:164e:0")
        expectAdvancedInferenceKeys()
        expectMatchingMessageKeys()
    }

    private static func expectAdvancedInferenceKeys() {
        let expected = [
            "settings.tab.advanced", "inference.title", "inference.help", "inference.discreteHelp",
            "inference.auto", "inference.ane", "inference.gpu", "inference.cpu", "inference.autoCoreML",
            "inference.kind.integrated", "inference.kind.discrete", "inference.missing", "inference.missingWithId",
            "inference.inUse", "inference.notDecided", "inference.notice.unavailable", "inference.notice.failed"
        ]
        for locale in ["en-US", "pt-BR"] {
            for key in expected {
                guard let value = AppText.translations[locale]?[key], !value.isEmpty else {
                    fputs("Missing advanced inference string `\(key)` in \(locale)\n", stderr)
                    exit(1)
                }
            }
        }
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
