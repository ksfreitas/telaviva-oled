use std::collections::VecDeque;

// Lines kept between two drains. The oldest line is evicted first and counted.
pub const MAX_DIAGNOSTIC_LINES: usize = 256;

const FNV_OFFSET_BASIS: u32 = 2_166_136_261;
const FNV_PRIME: u32 = 16_777_619;

/// Bounded ring of decision lines. Each line is `event monitor=id-xxxxxxxx key=value ...`. A
/// monitor is named only by its hashed id, never by the raw id, name or connection path.
#[derive(Debug, Default)]
pub struct Diagnostics {
    lines: VecDeque<String>,
    dropped: u64,
}

impl Diagnostics {
    /// Appends one line. A full ring evicts its oldest line and counts it as dropped.
    pub fn push(&mut self, event: &str, monitor_id: Option<&str>, fields: &str) {
        let mut line = event.to_owned();
        if let Some(id) = monitor_id {
            line.push_str(" monitor=");
            line.push_str(&short_id(id));
        }
        if !fields.is_empty() {
            line.push(' ');
            line.push_str(fields);
        }
        if self.lines.len() >= MAX_DIAGNOSTIC_LINES {
            self.lines.pop_front();
            self.dropped = self.dropped.saturating_add(1);
        }
        self.lines.push_back(line);
    }

    /// Returns the pending lines in order and empties the ring. A non-zero drop count is
    /// reported first as `diagnostics_dropped count=N`, and the count is then reset.
    pub fn drain(&mut self) -> Vec<String> {
        let mut lines = Vec::with_capacity(self.lines.len() + 1);
        if self.dropped > 0 {
            lines.push(format!("diagnostics_dropped count={}", self.dropped));
            self.dropped = 0;
        }
        lines.extend(self.lines.drain(..));
        lines
    }
}

/// Hashed monitor name: FNV-1a 32-bit over the UTF-8 bytes. The Windows shell computes the same
/// value in `shortId` (`native/windows/Platform/Diagnostics.cpp`), so the lines correlate.
pub fn short_id(id: &str) -> String {
    let hash = id.bytes().fold(FNV_OFFSET_BASIS, |hash, byte| {
        (hash ^ u32::from(byte)).wrapping_mul(FNV_PRIME)
    });
    format!("id-{hash:08x}")
}

/// Formats an optional brightness number for a field. Whole numbers print without a fraction.
pub fn number_or_none(value: Option<f64>) -> String {
    value.map_or_else(|| "none".to_owned(), |n| format!("{n}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_id_matches_the_shared_cross_platform_vectors() {
        assert_eq!(short_id("abc"), "id-1a47e90b");
        assert_eq!(short_id(""), "id-811c9dc5");
    }

    #[test]
    fn ring_keeps_only_the_newest_lines() {
        let mut ring = Diagnostics::default();
        for n in 0..MAX_DIAGNOSTIC_LINES + 10 {
            ring.push("tick", None, &format!("n={n}"));
        }
        assert_eq!(ring.lines.len(), MAX_DIAGNOSTIC_LINES);
        assert_eq!(ring.lines.front().map(String::as_str), Some("tick n=10"));
        assert_eq!(ring.lines.back().map(String::as_str), Some("tick n=265"));
    }

    #[test]
    fn evictions_are_counted_and_reported_first_on_drain() {
        let mut ring = Diagnostics::default();
        for n in 0..MAX_DIAGNOSTIC_LINES + 3 {
            ring.push("tick", None, &format!("n={n}"));
        }
        assert_eq!(ring.dropped, 3);
        let lines = ring.drain();
        assert_eq!(lines.len(), MAX_DIAGNOSTIC_LINES + 1);
        assert_eq!(lines[0], "diagnostics_dropped count=3");
        assert_eq!(lines[1], "tick n=3");
        assert_eq!(ring.dropped, 0);
        assert!(ring.drain().is_empty());
    }

    #[test]
    fn push_names_the_monitor_by_hashed_id_and_omits_empty_parts() {
        let mut ring = Diagnostics::default();
        ring.push("monitor_disconnected", Some("secret-serial-123"), "");
        ring.push("display_state", Some("secret-serial-123"), "state=awake");
        ring.push("tick", None, "count=1");
        let lines = ring.drain();
        let id = short_id("secret-serial-123");
        assert_eq!(lines[0], format!("monitor_disconnected monitor={id}"));
        assert_eq!(lines[1], format!("display_state monitor={id} state=awake"));
        assert_eq!(lines[2], "tick count=1");
        assert!(lines.iter().all(|line| !line.contains("secret")));
    }

    #[test]
    fn whole_numbers_print_without_a_fraction() {
        assert_eq!(number_or_none(Some(70.0)), "70");
        assert_eq!(number_or_none(Some(37.5)), "37.5");
        assert_eq!(number_or_none(None), "none");
    }
}
