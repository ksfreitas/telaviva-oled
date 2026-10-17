#include "LaptopPanel.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>

#include "JsonCodec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace telaviva {

namespace {

constexpr std::chrono::milliseconds kWmiWait{2000};
constexpr LONG kWqlWaitMs = 1000;
constexpr std::chrono::milliseconds kPollInterval{1000};
// Matches the macOS poll leeway (BuiltInBrightnessMonitor.pollLeeway). Wider than toleranceFor(1 s).
constexpr std::chrono::milliseconds kPollLeeway{500};

// Owned by the worker thread only. Released before the worker uninitializes COM.
void dropConnection(IWbemServices*& services)
{
    if (services != nullptr) {
        services->Release();
        services = nullptr;
    }
}

bool ensureConnected(IWbemServices*& services)
{
    if (services != nullptr) {
        return true;
    }
    IWbemLocator* locator = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                             reinterpret_cast<void**>(&locator));
    if (FAILED(created) || locator == nullptr) {
        return false;
    }
    BSTR resource = SysAllocString(L"ROOT\\WMI");
    const HRESULT connected =
        locator->ConnectServer(resource, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(resource);
    locator->Release();
    if (FAILED(connected) || services == nullptr) {
        services = nullptr;
        return false;
    }
    // Local connection under the process identity; the default proxy settings are enough.
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

// First object of a WQL query, or nullptr. The caller releases it.
IWbemClassObject* firstObject(IWbemServices* services, const wchar_t* wql)
{
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(wql);
    IEnumWbemClassObject* enumerator = nullptr;
    const HRESULT queried = services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(queried) || enumerator == nullptr) {
        return nullptr;
    }
    IWbemClassObject* object = nullptr;
    ULONG returned = 0;
    const HRESULT next = enumerator->Next(kWqlWaitMs, 1, &object, &returned);
    enumerator->Release();
    if (FAILED(next) || returned != 1 || object == nullptr) {
        if (object != nullptr) {
            object->Release();
        }
        return nullptr;
    }
    return object;
}

// WMI maps small integers to VT_I4 in practice, so accept the common integer variants.
std::optional<double> variantNumber(const VARIANT& value)
{
    switch (value.vt) {
    case VT_UI1:
        return static_cast<double>(value.bVal);
    case VT_I2:
        return static_cast<double>(value.iVal);
    case VT_I4:
        return static_cast<double>(value.lVal);
    case VT_UI2:
        return static_cast<double>(value.uiVal);
    case VT_UI4:
        return static_cast<double>(value.ulVal);
    case VT_I8:
        return static_cast<double>(value.llVal);
    default:
        return std::nullopt;
    }
}

std::optional<double> readCurrentBrightness(IWbemServices* services)
{
    IWbemClassObject* object = firstObject(services, L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    if (object == nullptr) {
        return std::nullopt;
    }
    VARIANT value;
    VariantInit(&value);
    std::optional<double> level;
    if (SUCCEEDED(object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr))) {
        level = variantNumber(value);
    }
    VariantClear(&value);
    object->Release();
    return level;
}

// Level is a uint8 array. Returns empty when the property is missing or has another element type.
std::vector<double> readLevels(IWbemClassObject* object)
{
    std::vector<double> levels;
    VARIANT value;
    VariantInit(&value);
    if (SUCCEEDED(object->Get(L"Level", 0, &value, nullptr, nullptr)) && (value.vt & VT_ARRAY) != 0 &&
        value.parray != nullptr) {
        SAFEARRAY* array = value.parray;
        LONG lower = 0;
        LONG upper = -1;
        if (SUCCEEDED(SafeArrayGetLBound(array, 1, &lower)) && SUCCEEDED(SafeArrayGetUBound(array, 1, &upper)) &&
            upper >= lower) {
            const VARTYPE element = static_cast<VARTYPE>(value.vt & VT_TYPEMASK);
            void* data = nullptr;
            if (SUCCEEDED(SafeArrayAccessData(array, &data))) {
                const LONG count = upper - lower + 1;
                if (element == VT_UI1) {
                    const BYTE* bytes = static_cast<const BYTE*>(data);
                    for (LONG i = 0; i < count; ++i) {
                        levels.push_back(static_cast<double>(bytes[i]));
                    }
                } else if (element == VT_I4) {
                    const LONG* values = static_cast<const LONG*>(data);
                    for (LONG i = 0; i < count; ++i) {
                        levels.push_back(static_cast<double>(values[i]));
                    }
                }
                SafeArrayUnaccessData(array);
            }
        }
    }
    VariantClear(&value);
    return levels;
}

// `instance` is one WmiMonitorBrightnessMethods object; its __PATH is the target of ExecMethod.
bool invokeSetBrightness(IWbemServices* services, IWbemClassObject* instance, int level)
{
    VARIANT pathValue;
    VariantInit(&pathValue);
    bool ok = false;
    if (SUCCEEDED(instance->Get(L"__PATH", 0, &pathValue, nullptr, nullptr)) && pathValue.vt == VT_BSTR &&
        pathValue.bstrVal != nullptr) {
        BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
        BSTR methodName = SysAllocString(L"WmiSetBrightness");
        IWbemClassObject* methodClass = nullptr;
        IWbemClassObject* inSignature = nullptr;
        IWbemClassObject* outSignature = nullptr;
        IWbemClassObject* inParams = nullptr;
        IWbemClassObject* outParams = nullptr;
        if (SUCCEEDED(services->GetObject(className, 0, nullptr, &methodClass, nullptr)) && methodClass != nullptr &&
            SUCCEEDED(methodClass->GetMethod(methodName, 0, &inSignature, &outSignature)) &&
            inSignature != nullptr && SUCCEEDED(inSignature->SpawnInstance(0, &inParams)) && inParams != nullptr) {
            VARIANT timeout;
            VariantInit(&timeout);
            timeout.vt = VT_I4;
            timeout.lVal = 1;
            VARIANT brightness;
            VariantInit(&brightness);
            brightness.vt = VT_I4;
            brightness.lVal = level;
            if (SUCCEEDED(inParams->Put(L"Timeout", 0, &timeout, 0)) &&
                SUCCEEDED(inParams->Put(L"Brightness", 0, &brightness, 0))) {
                ok = SUCCEEDED(services->ExecMethod(pathValue.bstrVal, methodName, 0, nullptr, inParams, &outParams,
                                                    nullptr));
            }
        }
        if (outParams != nullptr) {
            outParams->Release();
        }
        if (inParams != nullptr) {
            inParams->Release();
        }
        if (outSignature != nullptr) {
            outSignature->Release();
        }
        if (inSignature != nullptr) {
            inSignature->Release();
        }
        if (methodClass != nullptr) {
            methodClass->Release();
        }
        SysFreeString(methodName);
        SysFreeString(className);
    }
    VariantClear(&pathValue);
    return ok;
}

bool writeNearestLevel(IWbemServices* services, double target)
{
    IWbemClassObject* brightness = firstObject(services, L"SELECT Level FROM WmiMonitorBrightness");
    if (brightness == nullptr) {
        return false;
    }
    const std::vector<double> levels = readLevels(brightness);
    brightness->Release();
    if (levels.empty()) {
        return false;
    }
    double nearest = levels.front();
    for (const double level : levels) {
        if (std::fabs(level - target) < std::fabs(nearest - target)) {
            nearest = level;
        }
    }

    IWbemClassObject* methods = firstObject(services, L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (methods == nullptr) {
        return false;
    }
    const bool written = invokeSetBrightness(services, methods, static_cast<int>(std::lround(nearest)));
    methods->Release();
    return written;
}

}  // namespace

struct WmiPanelWorker {
    std::mutex mutex;
    std::condition_variable wake;
    std::function<void(IWbemServices*&)> job;  // guarded by mutex
    bool hasJob = false;                       // guarded by mutex
    bool stopping = false;                     // guarded by mutex
    // Set by a caller that queues work, cleared by the worker when the work ends. A timed-out
    // call keeps it set, so calls never pile up behind a stuck WMI request.
    std::atomic<bool> busy{false};
    std::thread worker;

    // Runs fn on the worker and waits up to kWmiWait. Returns fallback when busy, on timeout, or when fn throws.
    template <typename R>
    R call(std::function<R(IWbemServices*&)> fn, R fallback)
    {
        if (busy.exchange(true)) {
            return fallback;
        }
        auto task = std::make_shared<std::packaged_task<R(IWbemServices*&)>>(std::move(fn));
        std::future<R> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex);
            job = [task](IWbemServices*& services) { (*task)(services); };
            hasJob = true;
        }
        wake.notify_one();
        if (result.wait_for(kWmiWait) != std::future_status::ready) {
            return fallback;
        }
        try {
            return result.get();
        } catch (...) {
            return fallback;
        }
    }
};

namespace {

// Worker body. COM is initialized here and the connection is released before it is uninitialized.
void workerMain(WmiPanelWorker& impl)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWbemServices* services = nullptr;
    for (;;) {
        std::function<void(IWbemServices*&)> job;
        {
            std::unique_lock<std::mutex> lock(impl.mutex);
            impl.wake.wait(lock, [&impl] { return impl.hasJob || impl.stopping; });
            if (!impl.hasJob) {
                break;
            }
            job = std::move(impl.job);
            impl.job = nullptr;
            impl.hasJob = false;
        }
        job(services);
        impl.busy.store(false);
    }
    dropConnection(services);
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
}

// Lets the follower see lid and display state from its own callbacks while the rest
// of the environment still comes from the real one.
class EnvironmentView final : public ILaptopEnvironment {
public:
    EnvironmentView(ILaptopEnvironment& base, std::optional<bool> lid, bool display)
        : base_(base), lidOpen_(lid), displayOn_(display)
    {
    }

    std::optional<DisplayPathInfo> internalDisplayPath() override { return base_.internalDisplayPath(); }
    std::optional<bool> lidOpen() override { return lidOpen_; }
    bool displayOn() override { return displayOn_; }
    std::optional<bool> hasBattery() override { return base_.hasBattery(); }
    BrightnessReadability panelBrightnessReadable() override { return base_.panelBrightnessReadable(); }

private:
    ILaptopEnvironment& base_;
    std::optional<bool> lidOpen_;
    bool displayOn_;
};

}  // namespace

WmiPanel::WmiPanel() : impl_(std::make_unique<WmiPanelWorker>())
{
    WmiPanelWorker* impl = impl_.get();
    impl->worker = std::thread([impl] { workerMain(*impl); });
}

WmiPanel::~WmiPanel()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

std::optional<double> WmiPanel::readPercent()
{
    return impl_->call<std::optional<double>>(
        [](IWbemServices*& services) -> std::optional<double> {
            if (!ensureConnected(services)) {
                return std::nullopt;
            }
            const std::optional<double> level = readCurrentBrightness(services);
            if (!level) {
                dropConnection(services);
                return std::nullopt;
            }
            if (*level < 0.0 || *level > 100.0) {
                return std::nullopt;
            }
            return level;
        },
        std::nullopt);
}

bool WmiPanel::setPercent(double percent)
{
    if (!std::isfinite(percent)) {
        return false;
    }
    const double target = std::clamp(percent, 0.0, 100.0);
    return impl_->call<bool>(
        [target](IWbemServices*& services) -> bool {
            if (!ensureConnected(services)) {
                return false;
            }
            const bool written = writeNearestLevel(services, target);
            if (!written) {
                dropConnection(services);
            }
            return written;
        },
        false);
}

// ---- LaptopPanelFollower -------------------------------------------------

namespace {

constexpr std::chrono::milliseconds kImmediate{0};

}  // namespace

LaptopPanelFollower::LaptopPanelFollower(ILaptopPanel& panel, ILaptopEnvironment& environment,
                                         ICoreSubmitter& submitter, IScheduler& scheduler, IClock& clock)
    : panel_(panel), environment_(environment), submitter_(submitter), scheduler_(scheduler), clock_(clock)
{
}

LaptopPanelFollower::~LaptopPanelFollower()
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        destroyed_ = true;
        stale = takeTimerLocked();
    }
    cancelTimer(stale);
    // A read that started before destruction still holds this object; wait for it.
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return readsInFlight_ == 0; });
}

void LaptopPanelFollower::setFollowEnabled(bool enabled)
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (evaluated_ && enabled == followEnabled_) {
            return;
        }
        evaluated_ = true;
        followEnabled_ = enabled;
        stale = reconcileLocked();
    }
    cancelTimer(stale);
}

void LaptopPanelFollower::onLidChanged(bool open)
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lidOpen_ = open;
        stale = reconcileLocked();
    }
    cancelTimer(stale);
}

void LaptopPanelFollower::onDisplayStatusChanged(bool on)
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        displayOn_ = on;
        stale = reconcileLocked();
    }
    cancelTimer(stale);
}

void LaptopPanelFollower::onCalibrationChanged(bool calibrating)
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (calibrating_ == calibrating) {
            return;
        }
        calibrating_ = calibrating;
        stale = reconcileLocked();
    }
    cancelTimer(stale);
}

bool LaptopPanelFollower::canCalibrate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return followEnabled_ && available_;
}

LaptopReason LaptopPanelFollower::reason() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reason_;
}

TimerId LaptopPanelFollower::takeTimerLocked()
{
    const TimerId id = armed_ ? timerId_ : kInvalidTimerId;
    armed_ = false;
    timerId_ = kInvalidTimerId;
    // Bumped even when nothing was armed, so a read that started before this change is dropped.
    ++generation_;
    return id;
}

void LaptopPanelFollower::armLocked(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance)
{
    if (armed_) {
        return;
    }
    const uint64_t generation = ++generation_;
    const TimerId id = scheduler_.scheduleOnce(delay, tolerance, [this, generation] { onTick(generation); });
    armed_ = id != kInvalidTimerId;
    timerId_ = id;
}

LaptopAvailability LaptopPanelFollower::evaluateLocked()
{
    EnvironmentView view(environment_, lidOpen_, displayOn_);
    return detectLaptop(view);
}

bool LaptopPanelFollower::monitoringLocked() const
{
    return followEnabled_ && available_ && !calibrating_;
}

void LaptopPanelFollower::publishLocked(const Report& report)
{
    if (submitted_ && *submitted_ == report) {
        return;
    }
    submitted_ = report;
    std::optional<double> brightness;
    if (report.brightness) {
        brightness = static_cast<double>(*report.brightness);
    }
    submitter_.submit(buildBuiltinDisplay(report.available, brightness, report.calibrating));
}

// Re-evaluates availability and starts, stops or resumes polling. Caller holds mutex_.
TimerId LaptopPanelFollower::reconcileLocked()
{
    const LaptopAvailability availability = evaluateLocked();
    available_ = availability.available;
    reason_ = availability.reason;

    const bool monitor = monitoringLocked();
    const TimerId stale = monitor ? kInvalidTimerId : takeTimerLocked();
    if (!available_) {
        lastLevel_.reset();
        publishLocked(Report{false, std::nullopt, calibrating_});
    } else if (monitor) {
        // Start or resume: the first read is immediate, on the scheduler thread.
        armLocked(kImmediate, kImmediate);
    } else {
        Report report{true, std::nullopt, calibrating_};
        if (calibrating_) {
            report.brightness = lastLevel_;
        }
        publishLocked(report);
    }
    return stale;
}

void LaptopPanelFollower::onTick(uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (destroyed_ || !armed_ || generation != generation_) {
            return;
        }
        armed_ = false;
        timerId_ = kInvalidTimerId;
        ++readsInFlight_;
    }
    // The read may wait on WMI, so the lock is released; setters never wait for it.
    const std::optional<double> level = panel_.readPercent();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!destroyed_ && generation == generation_) {
        applyReadLocked(level);
    }
    --readsInFlight_;
    idle_.notify_all();
}

void LaptopPanelFollower::applyReadLocked(std::optional<double> level)
{
    if (!monitoringLocked()) {
        return;
    }
    if (level && std::isfinite(*level)) {
        // Whole percent, as the macOS report. Only a changed whole percent is submitted.
        const int whole = static_cast<int>(std::lround(std::clamp(*level, 0.0, 100.0)));
        lastLevel_ = whole;
        publishLocked(Report{true, whole, false});
        armLocked(kPollInterval, kPollLeeway);
        return;
    }
    // A failed read keeps the last value and is never reported as zero. The
    // availability check decides whether the panel is gone.
    const LaptopAvailability availability = evaluateLocked();
    available_ = availability.available;
    reason_ = availability.reason;
    if (!available_) {
        lastLevel_.reset();
        publishLocked(Report{false, std::nullopt, calibrating_});
        return;
    }
    publishLocked(Report{true, lastLevel_, calibrating_});
    armLocked(kPollInterval, kPollLeeway);
}

void LaptopPanelFollower::cancelTimer(TimerId id)
{
    if (id != kInvalidTimerId) {
        scheduler_.cancel(id);
    }
}

}  // namespace telaviva
