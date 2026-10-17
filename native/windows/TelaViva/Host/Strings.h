#pragma once

#include <string>
#include <string_view>

namespace telaviva::host {

// Returns the text for a dotted key such as "tray.menu.quit" in the active
// language. A missing key returns "[key]" so the gap is visible; never throws.
std::wstring Str(std::string_view key);

// Accepts "en-US", "pt-BR" or "auto". Any other value is treated as "auto".
// Takes effect on the next Str call, without a restart. Thread-safe.
void setLanguage(std::string_view bcp47);

}  // namespace telaviva::host
