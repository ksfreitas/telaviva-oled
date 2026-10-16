import Foundation

enum AppText {
    private static let translations: [String: [String: String]] = [
        "en-US": ["app":"TelaViva OLED", "refresh":"Refresh status", "noDisplays":"No controllable displays", "ddcUnavailable":"External display control is unavailable until a supported DDC connection is detected.", "coreUnavailable":"Core service is not linked", "presenceUnavailable":"Presence detection unavailable", "settings":"Settings", "quit":"Quit", "brightness":"Brightness", "volume":"Volume", "unavailable":"unavailable", "general":"General", "language":"Language", "launch":"Launch at login", "protection":"OLED protection", "presence":"Dim when no face is detected", "presenceHelp":"Face detection is enabled only when the local detector is available.", "alwaysOnCamera":"Keep camera on", "alwaysOnCameraHelp":"Keeps the camera on between checks while respecting battery and display sleep pauses.", "saver":"Dim while the screensaver is active", "protectionBrightness":"Protection brightness", "battery":"Suspend presence checks on battery", "check":"Check interval", "return":"Return check interval", "availability":"Availability", "camera":"Camera", "cameraPicker":"Use default camera", "cameraHelp":"Camera access is requested only after a validated local detector is available and protection is enabled.", "displayHelp":"Display support depends on the monitor, cable, dock, and connection path.", "systemLanguage":"Use system language", "english":"English (US)", "portuguese":"Português (Brasil)", "loginItemFailureTitle":"Could not update launch at login", "loginItemFailure":"TelaViva restored the previous setting. Details:", "ok":"OK", "cameraStatus.ready":"functioning", "cameraStatus.unavailable":"unavailable", "cameraStatus.busy":"busy", "cameraStatus.permission":"permission required", "cameraStatus.error":"error", "cameraStatus.unknown":"unknown", "cameraStatus.disabled":"disabled", "cameraStatus.suspended":"suspended", "cameraStatus.suspended_battery":"suspended on battery", "cameraStatus.suspended_power_unknown":"waiting for power status", "cameraStatus.suspended_no_monitor":"no compatible monitor", "cameraStatus.suspended_display_sleep":"suspended while displays sleep", "cameraStatus.checking":"waiting for first image", "cameraStatus.present":"face detected", "cameraStatus.absent":"no face detected", "cameraStatus.insufficient_light":"insufficient light"],
        "pt-BR": ["app":"TelaViva OLED", "refresh":"Atualizar estado", "noDisplays":"Nenhum monitor controlável", "ddcUnavailable":"Controle externo indisponível até detectar uma conexão DDC compatível.", "coreUnavailable":"Serviço principal não conectado", "presenceUnavailable":"Detecção de presença indisponível", "settings":"Ajustes", "quit":"Encerrar", "brightness":"Brilho", "volume":"Volume", "unavailable":"indisponível", "general":"Geral", "language":"Idioma", "launch":"Abrir ao iniciar", "protection":"Proteção OLED", "presence":"Reduzir brilho quando não houver rosto", "presenceHelp":"A detecção facial só é ativada quando o detector local está disponível.", "alwaysOnCamera":"Manter a câmera ligada", "alwaysOnCameraHelp":"Mantém a câmera ligada entre as verificações, respeitando as pausas por bateria e tela desligada.", "saver":"Reduzir durante o protetor de tela", "protectionBrightness":"Brilho na proteção", "battery":"Suspender verificações na bateria", "check":"Intervalo de verificação", "return":"Intervalo para retorno", "availability":"Disponibilidade", "camera":"Câmera", "cameraPicker":"Usar câmera padrão", "cameraHelp":"A câmera só será solicitada quando houver um detector local validado e a proteção estiver ativa.", "displayHelp":"O suporte depende do monitor, cabo, dock e caminho de conexão.", "systemLanguage":"Usar idioma do sistema", "english":"Inglês (EUA)", "portuguese":"Português (Brasil)", "loginItemFailureTitle":"Não foi possível alterar a inicialização", "loginItemFailure":"A configuração anterior foi restaurada. Detalhes:", "ok":"OK", "cameraStatus.ready":"funcionando", "cameraStatus.unavailable":"indisponível", "cameraStatus.busy":"ocupada", "cameraStatus.permission":"permissão necessária", "cameraStatus.error":"erro", "cameraStatus.unknown":"desconhecido", "cameraStatus.disabled":"desativada", "cameraStatus.suspended":"suspensa", "cameraStatus.suspended_battery":"suspensa na bateria", "cameraStatus.suspended_power_unknown":"aguardando estado de energia", "cameraStatus.suspended_no_monitor":"sem monitor compatível", "cameraStatus.suspended_display_sleep":"suspensa enquanto os monitores dormem", "cameraStatus.checking":"aguardando primeira imagem", "cameraStatus.present":"rosto detectado", "cameraStatus.absent":"nenhum rosto detectado", "cameraStatus.insufficient_light":"pouca iluminação"]
    ]

    static func string(_ key: String, locale: String?, messages: [String: String]? = nil) -> String {
        let coreKey: [String: String] = ["app":"app-title", "settings":"settings-title", "brightness":"control-brightness", "volume":"control-volume", "presenceUnavailable":"presence-disabled"]
        if let message = coreKey[key].flatMap({ messages?[$0] }) { return message }
        let selected = locale == "pt-BR" || (locale == "auto" && Locale.preferredLanguages.first?.hasPrefix("pt") == true) ? "pt-BR" : "en-US"
        return translations[selected]?[key] ?? key
    }

    static func cameraStatus(_ snapshot: CoreSnapshot, locale: String?) -> String {
        guard let camera = snapshot.camera else {
            return string("cameraStatus.unavailable", locale: locale)
        }
        let status = camera.display_status ?? camera.status ?? "unknown"
        return string("cameraStatus.\(status)", locale: locale, messages: snapshot.messages)
    }

    static func cameraInlineStatus(status: String, locale: String?, messages: [String: String]? = nil) -> String {
        "\(string("camera", locale: locale, messages: messages)): \(string("cameraStatus.\(status)", locale: locale, messages: messages))"
    }

    static func cameraInlineStatus(_ snapshot: CoreSnapshot, locale: String?) -> String {
        let status = snapshot.camera?.display_status ?? snapshot.camera?.status ?? "unavailable"
        return cameraInlineStatus(status: status, locale: locale, messages: snapshot.messages)
    }
}
