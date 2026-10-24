#include "AmbientCalibrationController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

namespace telaviva {
namespace {

// How long a restore waits for a monitor that is not in the snapshot before it gives up (macOS monitorRecoveryTimeout).
constexpr std::chrono::milliseconds kMonitorRecoveryTimeout{90000};
// Leeway for the give-up timer: the macOS value for a 90 s delay (10%, capped at 1 s).
constexpr std::chrono::milliseconds kMonitorRecoveryTolerance{1000};

// Clamps to the range. The range is ordered (lower <= upper) by construction, so zero width is valid.
double clampTo(double value, const SliderRange& range)
{
    return std::min(std::max(value, range.lower), range.upper);
}

bool isPercent(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

std::string numberText(double value)
{
    // Precision 15 keeps slider fractions exact and prints whole percents without a fraction.
    std::ostringstream out;
    out.precision(15);
    out << value;
    return out.str();
}

std::string jsonString(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        const unsigned int code = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (code < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", code);
            out += buffer;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// The ambient_curves object of update_settings: { "<monitor-id>": { "points": [ { monitor, builtin } ] } }.
std::string curvesText(const AmbientCurveMap& curves)
{
    std::string out = "{";
    bool firstCurve = true;
    for (const auto& [monitorId, curve] : curves) {
        if (!firstCurve) {
            out += ',';
        }
        firstCurve = false;
        out += jsonString(monitorId) + ":{\"points\":[";
        bool firstPoint = true;
        for (const AmbientCurvePoint& point : curve.points) {
            if (!firstPoint) {
                out += ',';
            }
            firstPoint = false;
            out += "{\"monitor\":" + numberText(point.monitor) + ",\"builtin\":" + numberText(point.builtin) + "}";
        }
        out += "]}";
    }
    out += '}';
    return out;
}

}  // namespace

bool AmbientCurve::isComplete() const
{
    return points.size() >= kMinPoints;
}

std::optional<AmbientCurvePoint> AmbientCurve::point(int monitor) const
{
    const double value = static_cast<double>(monitor);
    for (const AmbientCurvePoint& candidate : points) {
        if (candidate.monitor == value) {
            return candidate;
        }
    }
    return std::nullopt;
}

void AmbientCurve::set(int monitor, double builtin)
{
    const double value = static_cast<double>(monitor);
    for (AmbientCurvePoint& candidate : points) {
        if (candidate.monitor == value) {
            candidate.builtin = builtin;
            return;
        }
    }
    if (points.size() >= kMaxPoints) {
        return;
    }
    const auto insertAt = std::find_if(points.begin(), points.end(), [value](const AmbientCurvePoint& candidate) {
        return candidate.monitor > value;
    });
    points.insert(insertAt, AmbientCurvePoint{value, builtin});
}

void AmbientCurve::remove(int monitor)
{
    const double value = static_cast<double>(monitor);
    std::erase_if(points, [value](const AmbientCurvePoint& candidate) { return candidate.monitor == value; });
}

std::string buildCalibrationPayload(const SettingsDraft& draft, const AmbientCurveMap& curves)
{
    // buildSettingsPayload ends with the settings object and the message object. The curves are added inside
    // the settings object, which the draft's builder does not carry.
    const std::string payload = buildSettingsPayload(draft);
    const std::string closing = "}}";
    if (payload.size() < closing.size() ||
        payload.compare(payload.size() - closing.size(), closing.size(), closing) != 0) {
        return std::string();
    }
    return payload.substr(0, payload.size() - closing.size()) + ",\"ambient_curves\":" + curvesText(curves) + "}}";
}

bool isValidCurveMap(const AmbientCurveMap& curves)
{
    for (const auto& [monitorId, curve] : curves) {
        if (monitorId.empty() || monitorId.size() > 512 || curve.points.size() > AmbientCurve::kMaxPoints) {
            return false;
        }
        double previous = 0.0;
        for (const AmbientCurvePoint& point : curve.points) {
            const bool monitorOk =
                std::isfinite(point.monitor) && point.monitor >= 1.0 && point.monitor <= 100.0 && point.monitor > previous;
            const bool builtinOk = std::isfinite(point.builtin) && point.builtin >= 0.0 && point.builtin <= 100.0;
            if (!monitorOk || !builtinOk) {
                return false;
            }
            previous = point.monitor;
        }
    }
    return true;
}

AmbientCalibrationController::AmbientCalibrationController(ICalibrationPanel& panel, ICalibrationSettings& settings,
                                                           IFollowPause& follow, ICalibrationMonitors& monitors,
                                                           AutoBrightnessOwner& owner, AdaptiveBrightness& adaptive,
                                                           CalibrationSession& session, IScheduler& scheduler)
    : panel_(panel),
      settings_(settings),
      follow_(follow),
      monitors_(monitors),
      owner_(owner),
      adaptive_(adaptive),
      session_(session),
      scheduler_(scheduler)
{
}

AmbientCalibrationController::~AmbientCalibrationController()
{
    std::optional<TimerId> timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timer = monitorTimer_;
        monitorTimer_.reset();
    }
    // Cancelled outside the lock: a give-up that is running waits for the lock, and cancel waits for that callback.
    if (timer) {
        scheduler_.cancel(*timer);
    }
}

std::optional<CalibrationNotice> AmbientCalibrationController::notice() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return monitorNotice_ ? monitorNotice_ : notice_;
}

void AmbientCalibrationController::setProtectionHolds(std::function<bool()> protectionHolds)
{
    protectionHolds_ = std::move(protectionHolds);
}

void AmbientCalibrationController::begin(const std::string& monitorId, int point)
{
    if (point < 1 || point > 100) {
        return;
    }
    const AmbientCurve curve = curveOf(monitorId);
    // An existing point can always be re-measured; a new one needs room on the curve.
    if (!curve.point(point) && curve.points.size() >= AmbientCurve::kMaxPoints) {
        return;
    }
    notice_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitorNotice_.reset();
    }
    // Refused before any read: protection may hold the built-in screen at a dimmed level, and calibration captures
    // that level as its restore target.
    if (protectionHolds_ && protectionHolds_()) {
        notice_ = CalibrationNotice::ProtectionActive;
        return;
    }
    // Checked live: the follower may have paused since its last report.
    if (follow_.paused()) {
        notice_ = CalibrationNotice::BuiltInUnavailable;
        return;
    }
    if (captured_ && captured_->monitorId != monitorId) {
        end(CalibrationEndReason::Cancelled);
    }
    if (!captured_) {
        // The new session replaces a monitor-only record, so a pending monitor restore is dropped here, before the
        // file is read. Once it is dropped, nothing else touches the file: onSnapshot and the timer act only on a
        // pending restore. A refused start keeps the record, so the next launch still restores the monitor.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingMonitor_.reset();
        }
        const std::optional<CalibrationRecord> leftover = session_.pendingRecord();
        const bool replacing = leftover.has_value() && !leftover->adaptiveValid;
        if (!startSession(monitorId, replacing)) {
            return;
        }
    }
    // The point is a monitor level: move the monitor first, then set the Laptop slider.
    static_cast<void>(monitors_.setPercent(monitorId, static_cast<double>(point)));
    const SliderRange range = allowedRange(point, monitorId);
    const std::optional<AmbientCurvePoint> stored = curve.point(point);
    const double initial = stored ? stored->builtin : captured_->panelLevel;
    const double slider = clampTo(initial, range);
    if (!panel_.setPanelPercent(slider)) {
        notice_ = CalibrationNotice::BuiltInWriteFailed;
        finish();
        return;
    }
    active_ = ActiveCalibration{monitorId, point, slider};
}

SliderRange AmbientCalibrationController::allowedRange(int point, const std::string& monitorId)
{
    const AmbientCurve curve = curveOf(monitorId);
    const double value = static_cast<double>(point);
    // Points are ascending: the last point below gives the lower bound, the first point above the upper bound.
    double lower = 0.0;
    for (const AmbientCurvePoint& candidate : curve.points) {
        if (candidate.monitor < value) {
            lower = candidate.builtin;
        }
    }
    double upper = 100.0;
    for (const AmbientCurvePoint& candidate : curve.points) {
        if (candidate.monitor > value) {
            upper = candidate.builtin;
            break;
        }
    }
    return SliderRange{std::min(lower, upper), upper};
}

void AmbientCalibrationController::setSlider(double value)
{
    if (!active_) {
        return;
    }
    const double clamped = clampTo(value, allowedRange(active_->point, active_->monitorId));
    if (!panel_.setPanelPercent(clamped)) {
        notice_ = CalibrationNotice::BuiltInWriteFailed;
        finish();
        return;
    }
    active_->sliderValue = clamped;
}

bool AmbientCalibrationController::anchorReached(const std::string& monitorId)
{
    if (!active_ || active_->monitorId != monitorId) {
        return false;
    }
    const std::optional<double> observed = monitors_.observedPercent(monitorId);
    if (!observed) {
        return false;
    }
    return std::abs(*observed - static_cast<double>(active_->point)) <= 2.0;
}

bool AmbientCalibrationController::confirm()
{
    if (!active_) {
        return false;
    }
    AmbientCurveMap curves = settings_.curves();
    curves[active_->monitorId].set(active_->point, active_->sliderValue);
    return submitCurves(curves);
}

bool AmbientCalibrationController::removePoint(const std::string& monitorId, int point)
{
    if (active_ && active_->monitorId == monitorId && active_->point == point) {
        return false;
    }
    AmbientCurveMap curves = settings_.curves();
    curves[monitorId].remove(point);
    return submitCurves(curves);
}

void AmbientCalibrationController::end(CalibrationEndReason reason)
{
    if (reason == CalibrationEndReason::BuiltInUnavailable && captured_) {
        notice_ = CalibrationNotice::BuiltInUnavailable;
    }
    finish();
}

bool AmbientCalibrationController::recoverPending()
{
    const std::optional<CalibrationRecord> record = session_.pendingRecord();
    if (!record) {
        // Nothing was left, or the file was just quarantined. An unreadable file makes recover report the failure.
        if (session_.recover(adaptive_)) {
            return true;
        }
        notice_ = CalibrationNotice::RecoveryFailed;
        return false;
    }
    // The panel level and the adaptive values are restored at once. A refused panel write keeps the whole record
    // for the next launch, so nothing is lost.
    if (record->panelLevel && !panel_.setPanelPercent(*record->panelLevel)) {
        notice_ = CalibrationNotice::RecoveryFailed;
        return false;
    }
    // recover rewrites a record that names a monitor as monitor-only, so the adaptive values are never restored twice.
    if (record->adaptiveValid && !session_.recover(adaptive_)) {
        notice_ = CalibrationNotice::RecoveryFailed;
        return false;
    }
    if (record->monitor) {
        // The monitor restore waits for the monitor. The record keeps its part until then.
        holdMonitorRestore(record->monitor->monitorId, record->monitor->percent);
        onSnapshot();
    }
    return true;
}

void AmbientCalibrationController::onSnapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pendingMonitor_) {
        return;
    }
    // Still missing from the snapshot, or its brightness is unknown: wait for the next publish.
    if (!monitors_.observedPercent(pendingMonitor_->monitorId).has_value()) {
        return;
    }
    // A refused write keeps the restore; the next publish tries again until the give-up timer ends the wait.
    if (!monitors_.setPercent(pendingMonitor_->monitorId, pendingMonitor_->percent)) {
        return;
    }
    pendingMonitor_.reset();
    session_.end();
}

AmbientCurve AmbientCalibrationController::curveOf(const std::string& monitorId)
{
    const AmbientCurveMap curves = settings_.curves();
    const auto found = curves.find(monitorId);
    return found == curves.end() ? AmbientCurve{} : found->second;
}

bool AmbientCalibrationController::startSession(const std::string& monitorId, bool replacingPending)
{
    const std::optional<double> level = panel_.panelPercent();
    if (!level) {
        notice_ = CalibrationNotice::BuiltInUnreadable;
        return false;
    }
    Captured captured;
    captured.monitorId = monitorId;
    captured.panelLevel = *level;
    captured.monitorOriginal = monitors_.observedPercent(monitorId);

    // The record is written before any change, by the owner's persist-first step. A refused record means
    // nothing was changed. Levels outside 0..100 are left out of the record, so they never refuse it.
    bool persistAttempted = false;
    bool persisted = false;
    const AutoBrightnessOutcome outcome = owner_.acquire(
        AutoBrightnessHolder::calibration,
        [this, &captured, replacingPending, &persistAttempted, &persisted](const SavedAdaptiveState& state) {
            CalibrationRecord record;
            record.adaptive = state;
            if (isPercent(captured.panelLevel)) {
                record.panelLevel = captured.panelLevel;
            }
            if (captured.monitorOriginal && isPercent(*captured.monitorOriginal)) {
                record.monitor = SavedMonitorState{captured.monitorId, *captured.monitorOriginal};
            }
            persistAttempted = true;
            persisted = replacingPending ? session_.rewrite(record) : session_.begin(record);
            return persisted;
        });
    if (persistAttempted && !persisted) {
        // The owner took the holder before the refusal. It wrote nothing it owes, so releasing it only drops the holder.
        static_cast<void>(owner_.release(AutoBrightnessHolder::calibration));
        notice_ = CalibrationNotice::RecoveryFailed;
        return false;
    }
    switch (outcome) {
    case AutoBrightnessOutcome::notApplicable:
    case AutoBrightnessOutcome::turnedOff:
        break;
    case AutoBrightnessOutcome::notChanged:
    case AutoBrightnessOutcome::unknown:
        // The owner holds nothing. A record saved before a failed change is still on disk, so that notice applies.
        // notChanged and unknown keep the single failure mapping calibration has always used.
        notice_ = persisted ? CalibrationNotice::AutoBrightnessNotChanged : CalibrationNotice::AutoBrightnessUnknown;
        break;
    }
    captured_ = captured;
    follow_.setCalibrating(true);
    return true;
}

void AmbientCalibrationController::finish()
{
    if (!captured_) {
        return;
    }
    restoreState(*captured_);
    captured_.reset();
    active_.reset();
}

void AmbientCalibrationController::restoreState(const Captured& captured)
{
    // Order matters: panel level, adaptive brightness, monitor, then the pause flag. A failing step
    // does not stop the rest.
    static_cast<void>(panel_.setPanelPercent(captured.panelLevel));
    // The owner restores only when this holder turned the setting off, and skips the write when the user turned it on.
    const bool adaptiveRestored = owner_.release(AutoBrightnessHolder::calibration);
    bool monitorRestored = true;
    if (captured.monitorOriginal) {
        monitorRestored = monitors_.setPercent(captured.monitorId, *captured.monitorOriginal);
    }
    follow_.setCalibrating(false);
    if (!adaptiveRestored) {
        // The record is kept, with its monitor part, so the next launch retries both.
        if (!notice_) {
            notice_ = CalibrationNotice::AutoBrightnessNotChanged;
        }
        return;
    }
    if (monitorRestored) {
        session_.end();
        return;
    }
    // The adaptive values are back but the monitor is not. Keep only the monitor part, so a later launch never
    // restores the adaptive values again, and retry the monitor on each publish.
    CalibrationRecord monitorOnly;
    monitorOnly.adaptiveValid = false;
    monitorOnly.monitor = SavedMonitorState{captured.monitorId, *captured.monitorOriginal};
    if (!session_.rewrite(monitorOnly)) {
        if (!notice_) {
            notice_ = CalibrationNotice::RecoveryFailed;
        }
        return;
    }
    holdMonitorRestore(captured.monitorId, *captured.monitorOriginal);
}

void AmbientCalibrationController::holdMonitorRestore(const std::string& monitorId, double percent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingMonitor_ = PendingMonitor{monitorId, percent};
    if (monitorTimer_) {
        // The armed timer covers this restore too; it is not replaced.
        return;
    }
    const TimerId id = scheduler_.scheduleOnce(kMonitorRecoveryTimeout, kMonitorRecoveryTolerance,
                                               [this] { giveUpMonitorRecovery(); });
    if (id != kInvalidTimerId) {
        monitorTimer_ = id;
    }
}

void AmbientCalibrationController::giveUpMonitorRecovery()
{
    std::lock_guard<std::mutex> lock(mutex_);
    monitorTimer_.reset();
    if (!pendingMonitor_) {
        // Restored already, or a new session dropped it.
        return;
    }
    pendingMonitor_.reset();
    session_.end();
    monitorNotice_ = CalibrationNotice::MonitorRecoveryGaveUp;
}

bool AmbientCalibrationController::submitCurves(const AmbientCurveMap& curves)
{
    if (!isValidCurveMap(curves)) {
        return false;
    }
    const std::string json = buildCalibrationPayload(settings_.currentDraft(), curves);
    if (json.empty()) {
        return false;
    }
    return settings_.submit(json) == 0;
}

}  // namespace telaviva
