//! Built-in (laptop) display protection state. The built-in screen is not a core monitor: the
//! core decides and publishes `builtin_protection` in the snapshot; the shell applies it.

/// Reserved id used in `protection_disabled` and `screensaver_targets` for the built-in screen.
pub(crate) const BUILTIN_ID: &str = "builtin";

/// Runtime state reported by the shell. Never persisted.
#[derive(Debug, Default)]
pub(crate) struct BuiltIn {
    /// The shell opted in: a built-in screen exists and its brightness is adjustable.
    pub protectable: bool,
    /// The shell currently holds the screen dimmed and has not finished restoring.
    pub holding: bool,
    /// Consecutive absent samples while the built-in drives the camera.
    pub misses: u8,
    /// The core has dimmed the built-in for presence.
    pub presence_dimmed: bool,
    /// Reasons last written to diagnostics, so only changes are logged.
    pub last_reasons: String,
}

impl BuiltIn {
    /// Drops the presence dim and the absence count, as for a monitor.
    pub fn clear_presence(&mut self) {
        self.misses = 0;
        self.presence_dimmed = false;
    }
}

/// Why the built-in screen is dimmed. Empty when it is not protected, not available (asleep or
/// offline), calibrating, or the core is closed. Presence comes before screensaver.
pub(crate) fn reasons(
    protected: bool,
    available: bool,
    calibrating: bool,
    closed: bool,
    presence_dimmed: bool,
    saver_active: bool,
) -> Vec<&'static str> {
    let mut reasons = Vec::new();
    if !protected || !available || calibrating || closed {
        return reasons;
    }
    if presence_dimmed {
        reasons.push("presence");
    }
    if saver_active {
        reasons.push("screensaver");
    }
    reasons
}

/// Percent the built-in screen is dimmed to: presence and the screensaver share one protection
/// `level`, so any reason yields it and the reasons never stack. None without reasons.
pub(crate) fn target_percent(reasons: &[&str], level: f64) -> Option<f64> {
    if reasons.is_empty() {
        None
    } else {
        Some(level)
    }
}
