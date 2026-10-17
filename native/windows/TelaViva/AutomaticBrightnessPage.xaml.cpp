// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "AutomaticBrightnessPage.xaml.h"
#include "CalibrationDialog.xaml.h"
#include "Host/SettingRow.h"
#include "Host/Strings.h"
#include "Platform/SettingsModel.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include("AutomaticBrightnessPage.g.cpp")
#include "AutomaticBrightnessPage.g.cpp"
#endif

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// One second in 100 ns ticks, the unit of TimeSpan.
constexpr std::int64_t kPollIntervalTicks = 10'000'000;

// Monitor percent range of a calibration point, as the core accepts it.
constexpr int kMinPercent = 1;
constexpr int kMaxPercent = 100;

winrt::hstring loc(std::string_view key)
{
    return winrt::hstring{ telaviva::host::Str(key) };
}

winrt::hstring utf8(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return winrt::hstring{ out };
}

// Fills the printf-style %d placeholders of a resource string in order. %% is a literal percent sign.
std::wstring fillInts(std::wstring_view text, std::initializer_list<int> values)
{
    std::wstring out;
    auto next = values.begin();
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'%' && i + 1 < text.size()) {
            if (text[i + 1] == L'd' && next != values.end()) {
                out += std::to_wstring(*next);
                ++next;
                ++i;
                continue;
            }
            if (text[i + 1] == L'%') {
                out += L'%';
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// Numeric entry with a range. Step buttons move by one step; typing outside the range is clamped.
xc::NumberBox makeNumberBox(double minimum, double maximum, double step, double value)
{
    xc::NumberBox box;
    box.Minimum(minimum);
    box.Maximum(maximum);
    box.SmallChange(step);
    box.LargeChange(step * 10);
    box.Value(value);
    box.SpinButtonPlacementMode(xc::NumberBoxSpinButtonPlacementMode::Compact);
    box.ValidationMode(xc::NumberBoxValidationMode::InvalidInputOverwritten);
    box.Width(120);
    return box;
}

// Places a unit caption after a control. The unit carries its own space, so "%" sits against the box.
xaml::FrameworkElement withUnit(xaml::UIElement const& control, std::wstring_view unit)
{
    xc::TextBlock label;
    label.Text(winrt::hstring{ unit });
    label.VerticalAlignment(xaml::VerticalAlignment::Center);

    xc::StackPanel row;
    row.Orientation(xc::Orientation::Horizontal);
    row.Children().Append(control);
    row.Children().Append(label);
    return row;
}

// Switch with empty On and Off text; the row header already names the setting.
xc::ToggleSwitch makeSwitch(bool on, winrt::hstring const& name)
{
    xc::ToggleSwitch toggle;
    toggle.OnContent(winrt::box_value(winrt::hstring{}));
    toggle.OffContent(winrt::box_value(winrt::hstring{}));
    toggle.IsOn(on);
    xaml::Automation::AutomationProperties::SetName(toggle, name);
    return toggle;
}

bool isSaved(const std::vector<telaviva::host::CurvePointView>& points, int monitor)
{
    return std::any_of(points.begin(), points.end(),
        [monitor](const telaviva::host::CurvePointView& point) { return point.monitor == monitor; });
}

// The first of 25, 50, 75 and 100 not saved yet, else the first whole percent not saved.
int defaultAddPercent(const std::vector<telaviva::host::CurvePointView>& points)
{
    for (const int candidate : { 25, 50, 75, 100 }) {
        if (!isSaved(points, candidate)) {
            return candidate;
        }
    }
    for (int candidate = kMinPercent; candidate <= kMaxPercent; ++candidate) {
        if (!isSaved(points, candidate)) {
            return candidate;
        }
    }
    return 25;
}

// Add is refused for a percent that is already saved and once the curve is full.
bool canAddPoint(const std::vector<telaviva::host::CurvePointView>& points, int monitor)
{
    return !isSaved(points, monitor) && points.size() < telaviva::AmbientCurve::kMaxPoints;
}

// A missing or unknown value reads as not calibrated, as on macOS.
std::string statusKey(const std::string& follow)
{
    if (follow == "eligible") {
        return "ambient.status.eligible";
    }
    if (follow == "unsupported") {
        return "ambient.status.unsupported";
    }
    if (follow == "unstable") {
        return "ambient.status.unstable";
    }
    return "ambient.status.uncalibrated";
}

// An empty or unknown reason reads as unknown.
std::string unavailableKey(const std::string& reason)
{
    if (reason == "noBuiltInDisplay" || reason == "brightnessNotAdjustable" || reason == "apiUnavailable" ||
        reason == "displayOffline") {
        return "ambient.unavailable." + reason;
    }
    return "ambient.unavailable.unknown";
}

// Maps the controller's stable notice keys to the resource keys. Empty for an unknown key.
std::string_view noticeKey(const std::string& notice)
{
    if (notice == "builtInUnreadable") {
        return "calibration.notice.builtInUnreadable";
    }
    if (notice == "builtInWriteFailed") {
        return "calibration.notice.builtInWriteFailed";
    }
    if (notice == "builtInUnavailable") {
        return "calibration.notice.builtInUnavailable";
    }
    if (notice == "autoBrightnessUnknown") {
        return "calibration.notice.autoUnknown";
    }
    if (notice == "autoBrightnessNotChanged") {
        return "calibration.notice.autoNotChanged";
    }
    if (notice == "recoveryFailed") {
        return "calibration.notice.recoveryFailed";
    }
    return {};
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    AutomaticBrightnessPage::AutomaticBrightnessPage()
    {
        InitializeComponent();
        Loaded([this](auto const&, auto const&) {
            startPolling();
            // Programmatic focus on the first control, so arriving from the navigation pane lands in the page.
            if (followSwitch_) {
                static_cast<void>(followSwitch_.Focus(xaml::FocusState::Programmatic));
            }
        });
        Unloaded([this](auto const&, auto const&) {
            if (poll_) {
                poll_.Stop();
            }
            // Leaving the tab ends a calibration, as on macOS.
            EndCalibration(telaviva::CalibrationEndReason::LeftTab);
        });
        RefreshStrings();
    }

    void AutomaticBrightnessPage::SetHost(telaviva::host::ISettingsHost* host)
    {
        host_ = host;
    }

    void AutomaticBrightnessPage::RefreshStrings()
    {
        PageTitle().Text(loc("settings.tab.ambient"));

        // Rebuilt rather than edited in place, as the General page does.
        RowsPanel().Children().Clear();
        const telaviva::Snapshot snapshot = host_ ? host_->latestSnapshot() : telaviva::Snapshot{};
        const telaviva::host::LaptopStatus laptop = host_ ? host_->laptopStatus() : telaviva::host::LaptopStatus{};
        const telaviva::host::CalibrationView view = host_ ? host_->calibrationView() : telaviva::host::CalibrationView{};
        builtRevision_ = snapshot.revision;
        layoutSignature_.clear();

        const winrt::hstring followHeader = loc("ambient.toggle");
        followSwitch_ = makeSwitch(snapshot.settings.ambient_follow_enabled, followHeader);
        followSwitch_.Toggled([this](auto const& sender, auto const&) {
            setFollow(sender.as<xc::ToggleSwitch>().IsOn());
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(followHeader, loc("ambient.help"), followSwitch_));

        availabilityBar_ = xc::InfoBar{};
        availabilityBar_.IsClosable(false);
        availabilityBar_.IsOpen(false);
        RowsPanel().Children().Append(availabilityBar_);

        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("ambient.monitors.title")));
        monitorsPanel_ = xc::StackPanel{};
        monitorsPanel_.Spacing(12);
        RowsPanel().Children().Append(monitorsPanel_);

        // The notice is not per monitor, so it sits once under the list. It is never auto-dismissed.
        noticeBar_ = xc::InfoBar{};
        noticeBar_.Severity(xc::InfoBarSeverity::Warning);
        noticeBar_.IsClosable(false);
        noticeBar_.IsOpen(false);
        RowsPanel().Children().Append(noticeBar_);

        updateAvailability(laptop, snapshot.settings.ambient_follow_enabled);
        updateNotice();
        buildMonitors(snapshot, laptop, view, layoutSignature(snapshot, laptop, view));
    }

    void AutomaticBrightnessPage::EndCalibration(telaviva::CalibrationEndReason reason)
    {
        if (host_ && host_->calibrationView().active) {
            host_->endCalibration(reason);
        }
    }

    void AutomaticBrightnessPage::startPolling()
    {
        if (!poll_) {
            poll_ = DispatcherQueue().CreateTimer();
            poll_.Interval(winrt::Windows::Foundation::TimeSpan{ kPollIntervalTicks });
            poll_.Tick([this](auto const&, auto const&) { onPoll(); });
        }
        poll_.Start();
    }

    void AutomaticBrightnessPage::onPoll()
    {
        if (!host_) {
            return;
        }
        const telaviva::Snapshot snapshot = host_->latestSnapshot();
        if (builtRevision_ == 0 && snapshot.revision != 0) {
            // Built before the first snapshot arrived, so the controls hold placeholder values.
            RefreshStrings();
            return;
        }
        const telaviva::host::LaptopStatus laptop = host_->laptopStatus();
        const telaviva::host::CalibrationView view = host_->calibrationView();
        // Keeps the last notice: the controller clears it only when the next calibration starts.
        if (view.notice) {
            lastNotice_ = view.notice;
        }
        updateAvailability(laptop, snapshot.settings.ambient_follow_enabled);
        updateNotice();
        std::string signature = layoutSignature(snapshot, laptop, view);
        if (signature != layoutSignature_) {
            buildMonitors(snapshot, laptop, view, std::move(signature));
        }
    }

    void AutomaticBrightnessPage::setFollow(bool enabled)
    {
        if (!host_) {
            return;
        }
        // Only the follow field changes; the rest of the settings are read fresh.
        auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
        draft.ambientFollowEnabled = enabled;
        host_->submit(telaviva::buildSettingsPayload(draft));
    }

    void AutomaticBrightnessPage::updateAvailability(const telaviva::host::LaptopStatus& laptop, bool followOn)
    {
        // Stays usable to switch off when the setting is already on.
        followSwitch_.IsEnabled(laptop.available || followOn);
        if (laptop.available) {
            availabilityBar_.IsOpen(false);
            return;
        }
        // No built-in display is a fact about the machine, so it informs rather than warns.
        availabilityBar_.Severity(laptop.reason == "noBuiltInDisplay" ? xc::InfoBarSeverity::Informational
                                                                       : xc::InfoBarSeverity::Warning);
        availabilityBar_.Message(loc(unavailableKey(laptop.reason)));
        availabilityBar_.IsOpen(true);
    }

    void AutomaticBrightnessPage::updateNotice()
    {
        const std::string_view key = lastNotice_ ? noticeKey(*lastNotice_) : std::string_view{};
        if (key.empty()) {
            noticeBar_.IsOpen(false);
            return;
        }
        noticeBar_.Message(loc(key));
        noticeBar_.IsOpen(true);
    }

    std::string AutomaticBrightnessPage::layoutSignature(const telaviva::Snapshot& snapshot,
                                                         const telaviva::host::LaptopStatus& laptop,
                                                         const telaviva::host::CalibrationView& view)
    {
        std::string signature = laptop.available ? "available\n" : "unavailable\n";
        // The active point changes its row, because Remove is disabled while it is calibrated.
        if (view.active) {
            signature += view.monitorId + '#' + std::to_string(view.point) + '\n';
        }
        for (const auto& monitor : snapshot.monitors) {
            signature += monitor.id + '\n' + monitor.name + '\n' + monitor.follow + '\n';
            for (const auto& point : host_->curvePoints(monitor.id)) {
                signature += std::to_string(point.monitor) + ':' +
                             std::to_string(static_cast<int>(std::lround(point.builtin))) + ';';
            }
            signature.push_back('\n');
        }
        return signature;
    }

    void AutomaticBrightnessPage::buildMonitors(const telaviva::Snapshot& snapshot,
                                                const telaviva::host::LaptopStatus& laptop,
                                                const telaviva::host::CalibrationView& view,
                                                std::string signature)
    {
        layoutSignature_ = std::move(signature);
        monitorsPanel_.Children().Clear();
        if (snapshot.monitors.empty()) {
            monitorsPanel_.Children().Append(telaviva::host::MakeCaption(loc("ambient.monitors.none")));
            return;
        }

        for (const auto& monitor : snapshot.monitors) {
            monitorsPanel_.Children().Append(telaviva::host::MakeSectionHeader(
                utf8(monitor.name.empty() ? monitor.id : monitor.name)));
            monitorsPanel_.Children().Append(telaviva::host::MakeCaption(loc(statusKey(monitor.follow))));
            // An unsupported or unstable monitor cannot be matched to a Laptop level, and an unavailable Laptop
            // offers none to match, so the calibration controls stay hidden.
            if (!laptop.available || monitor.follow == "unsupported" || monitor.follow == "unstable") {
                continue;
            }
            monitorsPanel_.Children().Append(buildCalibrationPanel(monitor.id, view));
        }
    }

    xc::StackPanel AutomaticBrightnessPage::buildCalibrationPanel(const std::string& monitorId,
                                                                  const telaviva::host::CalibrationView& view)
    {
        std::vector<telaviva::host::CurvePointView> points = host_->curvePoints(monitorId);
        std::sort(points.begin(), points.end(),
            [](const telaviva::host::CurvePointView& a, const telaviva::host::CurvePointView& b) {
                return a.monitor < b.monitor;
            });

        xc::StackPanel panel;
        panel.Spacing(8);
        panel.Children().Append(telaviva::host::MakeCaption(loc("calibration.points.title")));
        for (const auto& point : points) {
            panel.Children().Append(buildPointRow(monitorId, point, view));
        }
        panel.Children().Append(buildAddRow(monitorId, points));
        if (points.size() < telaviva::AmbientCurve::kMinPoints) {
            panel.Children().Append(telaviva::host::MakeCaption(loc("calibration.minPoints")));
        }
        return panel;
    }

    xaml::FrameworkElement AutomaticBrightnessPage::buildPointRow(const std::string& monitorId,
                                                                  const telaviva::host::CurvePointView& point,
                                                                  const telaviva::host::CalibrationView& view)
    {
        const int monitorPercent = point.monitor;
        const int builtinPercent = static_cast<int>(std::lround(point.builtin));
        const bool calibrating = view.active && view.monitorId == monitorId && view.point == monitorPercent;

        xc::TextBlock text;
        text.Text(winrt::hstring{ fillInts(telaviva::host::Str("calibration.point.row"), { monitorPercent, builtinPercent }) });
        text.VerticalAlignment(xaml::VerticalAlignment::Center);

        const winrt::hstring recalibrate = loc("calibration.recalibrate");
        xc::Button calibrateButton;
        calibrateButton.Content(winrt::box_value(recalibrate));
        std::wstring calibrateName = recalibrate.c_str();
        calibrateName += L" " + std::to_wstring(monitorPercent) + L"%";
        xaml::Automation::AutomationProperties::SetName(calibrateButton, winrt::hstring{ calibrateName });
        calibrateButton.Click([this, monitorId, monitorPercent](auto const&, auto const&) {
            startCalibration(monitorId, monitorPercent);
        });

        const winrt::hstring removeTitle = loc("calibration.remove");
        xc::Button removeButton;
        removeButton.Content(winrt::box_value(removeTitle));
        removeButton.IsEnabled(!calibrating);
        std::wstring removeName = removeTitle.c_str();
        removeName += L" " + std::to_wstring(monitorPercent) + L"%";
        xaml::Automation::AutomationProperties::SetName(removeButton, winrt::hstring{ removeName });
        removeButton.Click([this, monitorId, monitorPercent](auto const&, auto const&) {
            if (host_) {
                host_->removeCalibrationPoint(monitorId, monitorPercent);
            }
        });

        xc::StackPanel row;
        row.Orientation(xc::Orientation::Horizontal);
        row.Spacing(8);
        row.Children().Append(text);
        row.Children().Append(calibrateButton);
        row.Children().Append(removeButton);
        return row;
    }

    xaml::FrameworkElement AutomaticBrightnessPage::buildAddRow(const std::string& monitorId,
                                                                const std::vector<telaviva::host::CurvePointView>& points)
    {
        const int initial = defaultAddPercent(points);
        xc::NumberBox box = makeNumberBox(static_cast<double>(kMinPercent), static_cast<double>(kMaxPercent), 1.0,
                                          static_cast<double>(initial));
        xaml::Automation::AutomationProperties::SetName(box, loc("calibration.add.label"));

        xc::Button addButton;
        addButton.Content(winrt::box_value(loc("calibration.add.button")));
        addButton.IsEnabled(canAddPoint(points, initial));
        box.ValueChanged([this, monitorId, addButton](xc::NumberBox const&, xc::NumberBoxValueChangedEventArgs const& args) {
            const double value = args.NewValue();
            if (std::isnan(value) || !host_) {
                return;
            }
            addButton.IsEnabled(canAddPoint(host_->curvePoints(monitorId), static_cast<int>(std::lround(value))));
        });

        addButton.Click([this, monitorId, box](auto const&, auto const&) {
            const double value = box.Value();
            if (std::isnan(value)) {
                return;
            }
            startCalibration(monitorId, static_cast<int>(std::lround(value)));
        });

        xc::TextBlock label;
        label.Text(loc("calibration.add.label"));
        label.VerticalAlignment(xaml::VerticalAlignment::Center);

        xc::StackPanel row;
        row.Orientation(xc::Orientation::Horizontal);
        row.Spacing(8);
        row.Children().Append(label);
        row.Children().Append(withUnit(box, L"%"));
        row.Children().Append(addButton);
        return row;
    }

    void AutomaticBrightnessPage::startCalibration(const std::string& monitorId, int point)
    {
        if (!host_) {
            return;
        }
        // A new calibration clears the last notice. The controller raises one again only if this start fails.
        lastNotice_.reset();
        host_->beginCalibration(monitorId, point);
        const telaviva::host::CalibrationView view = host_->calibrationView();
        if (view.notice) {
            lastNotice_ = view.notice;
        }
        updateNotice();
        if (view.active) {
            showCalibrationDialog(point);
        }
    }

    void AutomaticBrightnessPage::showCalibrationDialog(int point)
    {
        auto dialog = winrt::make<winrt::TelaViva::implementation::CalibrationDialog>();
        winrt::get_self<winrt::TelaViva::implementation::CalibrationDialog>(dialog)->Start(host_, point);
        dialog.XamlRoot(XamlRoot());
        // The dialog ends the calibration itself: Done saves first, and Cancel, Escape or a close discards.
        dialog.ShowAsync();
    }
}
