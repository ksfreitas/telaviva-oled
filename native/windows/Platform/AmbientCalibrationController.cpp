#include "AmbientCalibrationController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace telaviva {
namespace {

// Clamps to the range. The range is ordered (lower <= upper) by construction, so zero width is valid.
double clampTo(double value, const SliderRange& range)
{
    return std::min(std::max(value, range.lower), range.upper);
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
                                                           AdaptiveBrightness& adaptive, CalibrationSession& session)
    : panel_(panel),
      settings_(settings),
      follow_(follow),
      monitors_(monitors),
      adaptive_(adaptive),
      session_(session)
{
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
    // Checked live: the follower may have paused since its last report.
    if (follow_.paused()) {
        notice_ = CalibrationNotice::BuiltInUnavailable;
        return;
    }
    if (captured_ && captured_->monitorId != monitorId) {
        end(CalibrationEndReason::Cancelled);
    }
    if (!captured_ && !startSession(monitorId)) {
        return;
    }
    // The point is a monitor level: move the monitor first, then set the Laptop slider.
    monitors_.setPercent(monitorId, static_cast<double>(point));
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
    if (session_.recover(adaptive_)) {
        return true;
    }
    notice_ = CalibrationNotice::RecoveryFailed;
    return false;
}

AmbientCurve AmbientCalibrationController::curveOf(const std::string& monitorId)
{
    const AmbientCurveMap curves = settings_.curves();
    const auto found = curves.find(monitorId);
    return found == curves.end() ? AmbientCurve{} : found->second;
}

bool AmbientCalibrationController::startSession(const std::string& monitorId)
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

    // The record is written before any change. A refused record means nothing was changed.
    bool persistAttempted = false;
    bool persisted = false;
    const SuspendResult result = adaptive_.suspend(
        captured.adaptive, [this, &persistAttempted, &persisted](const SavedAdaptiveState& state) {
            persistAttempted = true;
            persisted = session_.begin(state);
            return persisted;
        });
    if (persistAttempted && !persisted) {
        notice_ = CalibrationNotice::RecoveryFailed;
        return false;
    }
    switch (result) {
    case SuspendResult::NotApplicable:
    case SuspendResult::WasOff:
        break;
    case SuspendResult::Suspended:
        captured.adaptiveHeld = true;
        break;
    case SuspendResult::Failed:
        if (persisted) {
            // The record exists and the write was rolled back, so the saved values are restored at the end.
            captured.adaptiveHeld = true;
            notice_ = CalibrationNotice::AutoBrightnessNotChanged;
        } else {
            // Nothing was recorded or written: the setting could not be read.
            notice_ = CalibrationNotice::AutoBrightnessUnknown;
        }
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
    bool adaptiveRestored = true;
    if (captured.adaptiveHeld) {
        adaptiveRestored = adaptive_.restore(captured.adaptive);
    }
    if (captured.monitorOriginal) {
        monitors_.setPercent(captured.monitorId, *captured.monitorOriginal);
    }
    follow_.setCalibrating(false);
    if (adaptiveRestored) {
        session_.end();
    } else {
        // The record is kept so the next launch retries the restore.
        if (!notice_) {
            notice_ = CalibrationNotice::AutoBrightnessNotChanged;
        }
    }
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
