#include "MediaCaptureBackend.h"

#include "CameraFormatSelection.h"
#include "CameraStatus.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace WDE = winrt::Windows::Devices::Enumeration;
namespace WGI = winrt::Windows::Graphics::Imaging;
namespace WMC = winrt::Windows::Media::Capture;
namespace WMCF = winrt::Windows::Media::Capture::Frames;
namespace WMP = winrt::Windows::Media::MediaProperties;
namespace WSS = winrt::Windows::Storage::Streams;

namespace telaviva {

namespace {

// Runs cleanup when the scope ends, including when a WinRT call throws.
template <typename Fn>
struct ScopeExit {
    Fn fn;
    ~ScopeExit() { fn(); }
};

// True while this thread runs a callback under deliveryMutex. close() checks it so that
// it never waits for StopAsync from inside FrameArrived.
thread_local bool tInDelivery = false;

// Sets tInDelivery for one callback and restores the previous value, even if the callback throws.
struct DeliveryFlagScope {
    DeliveryFlagScope() : previous(tInDelivery) { tInDelivery = true; }
    ~DeliveryFlagScope() { tInDelivery = previous; }
    DeliveryFlagScope(const DeliveryFlagScope&) = delete;
    DeliveryFlagScope& operator=(const DeliveryFlagScope&) = delete;

    bool previous;
};

// Shared with the WinRT event handlers. Handlers hold weak_ptr and lock it per event,
// so a late event after close() finds no session and does nothing.
struct Session {
    Session(const IClock& clockRef, ICameraBackendCallbacks& callbacksRef, double fps, std::string id)
        : clock(clockRef), callbacks(callbacksRef), targetFps(fps), cameraId(std::move(id))
    {
    }

    const IClock& clock;
    ICameraBackendCallbacks& callbacks;
    const double targetFps;
    const std::string cameraId;

    // Serialises callbacks and guards lastRetainedSeconds. Recursive so that close()
    // can be called from inside onFrame on the same thread.
    std::recursive_mutex deliveryMutex;
    std::atomic<bool> alive{true};
    std::atomic<bool> devicePresent{true};
    std::atomic<bool> watcherReady{false};  // false while the watcher reports its initial Added burst
    double lastRetainedSeconds = negativeInfinitySeconds();

    WMC::MediaCapture capture{nullptr};
    WMCF::MediaFrameReader reader{nullptr};
    WDE::DeviceWatcher watcher{nullptr};
    winrt::event_token captureFailedToken{};
    winrt::event_token frameArrivedToken{};
    winrt::event_token watcherAddedToken{};
    winrt::event_token watcherRemovedToken{};
    winrt::event_token watcherUpdatedToken{};
    winrt::event_token watcherCompletedToken{};
};

// Every callback goes through here, so none runs after alive is cleared.
template <typename Fn>
void withDelivery(Session& s, Fn&& fn)
{
    std::lock_guard<std::recursive_mutex> lock(s.deliveryMutex);
    if (s.alive.load()) {
        DeliveryFlagScope inDelivery;
        fn();
    }
}

template <typename Fn>
void bestEffort(Fn&& fn)
{
    try {
        fn();
    } catch (const winrt::hresult_error&) {
        // Teardown is best effort; the WinRT objects are released with the session.
    }
}

void notifyDevicesChanged(const std::weak_ptr<Session>& weak)
{
    if (const std::shared_ptr<Session> s = weak.lock()) {
        if (s->watcherReady.load()) {
            withDelivery(*s, [&] { s->callbacks.onDevicesChanged(); });
        }
    }
}

std::wstring wideText(const winrt::hstring& text)
{
    return std::wstring(text.c_str(), text.size());
}

bool isBuiltInDevice(const WDE::DeviceInformation& device)
{
    if (!device) {
        return false;
    }
    const WDE::EnclosureLocation location = device.EnclosureLocation();
    if (!location) {
        return false;
    }
    const WDE::Panel panel = location.Panel();
    return panel == WDE::Panel::Front || panel == WDE::Panel::Back;
}

bool isBuiltInPanel(const WMCF::MediaFrameSourceInfo& info)
{
    return isBuiltInDevice(info.DeviceInformation());
}

// VideoCapture devices as the Camera app lists them. Empty on failure, so the caller falls back to groups.
std::vector<CameraDeviceRecord> readVideoCaptureDevices()
{
    std::vector<CameraDeviceRecord> devices;
    try {
        for (const WDE::DeviceInformation& device : WDE::DeviceInformation::FindAllAsync(WDE::DeviceClass::VideoCapture).get()) {
            CameraDeviceRecord record;
            record.id = winrt::to_string(device.Id());
            record.name = wideText(device.Name());
            record.builtIn = isBuiltInDevice(device);
            devices.push_back(std::move(record));
        }
    } catch (const winrt::hresult_error&) {
        devices.clear();
    }
    return devices;
}

// Frame source groups, empty on failure. Index i of the result matches the group records built from it.
std::vector<WMCF::MediaFrameSourceGroup> readFrameSourceGroups()
{
    std::vector<WMCF::MediaFrameSourceGroup> groups;
    try {
        for (const WMCF::MediaFrameSourceGroup& group : WMCF::MediaFrameSourceGroup::FindAllAsync().get()) {
            groups.push_back(group);
        }
    } catch (const winrt::hresult_error&) {
        groups.clear();
    }
    return groups;
}

CameraGroupRecord groupRecord(const WMCF::MediaFrameSourceGroup& group)
{
    CameraGroupRecord record;
    record.name = wideText(group.DisplayName());
    for (const WMCF::MediaFrameSourceInfo& info : group.SourceInfos()) {
        CameraSourceRecord source;
        const WMCF::MediaFrameSourceKind kind = info.SourceKind();
        if (kind == WMCF::MediaFrameSourceKind::Color) {
            source.kind = CameraSourceKind::Color;
        } else if (kind == WMCF::MediaFrameSourceKind::Infrared) {
            source.kind = CameraSourceKind::Infrared;
        }
        const WDE::DeviceInformation device = info.DeviceInformation();
        if (device) {
            source.deviceId = winrt::to_string(device.Id());
        }
        record.sources.push_back(std::move(source));
    }
    return record;
}

// Name of the device with this id, empty when it cannot be resolved. Used to pick the matching group.
std::wstring deviceNameFor(const std::string& cameraId)
{
    try {
        const WDE::DeviceInformation device = WDE::DeviceInformation::CreateFromIdAsync(winrt::to_hstring(cameraId)).get();
        if (!device) {
            return {};
        }
        return wideText(device.Name());
    } catch (const winrt::hresult_error&) {
        return {};
    }
}

// The DeviceInformation id of the first source that has one, so it matches the watcher ids.
// Falls back to the group id. Only the group-based fallback list uses it; the device list is primary.
std::string stableCameraId(const WMCF::MediaFrameSourceGroup& group)
{
    for (const WMCF::MediaFrameSourceInfo& info : group.SourceInfos()) {
        const WDE::DeviceInformation device = info.DeviceInformation();
        if (device) {
            return winrt::to_string(device.Id());
        }
    }
    return winrt::to_string(group.Id());
}

// Resolves a camera id to its group. Device ids (the listed ids) are matched through the group sources
// first; the group-derived ids from the fallback list are matched last.
WMCF::MediaFrameSourceGroup findGroup(const std::string& cameraId)
{
    const std::vector<WMCF::MediaFrameSourceGroup> groups = readFrameSourceGroups();
    std::vector<CameraGroupRecord> records;
    records.reserve(groups.size());
    for (const WMCF::MediaFrameSourceGroup& group : groups) {
        records.push_back(groupRecord(group));
    }

    std::size_t index = chooseGroupForDevice(records, cameraId, deviceNameFor(cameraId));
    if (index == kNoCameraGroup) {
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (stableCameraId(groups[i]) == cameraId) {
                index = i;
                break;
            }
        }
    }
    return index == kNoCameraGroup ? WMCF::MediaFrameSourceGroup{nullptr} : groups[index];
}

// Group-based list for when the device list is unavailable. Duplicate ids are removed by the caller.
std::vector<CameraInfo> groupCameras(const std::vector<WMCF::MediaFrameSourceGroup>& groups)
{
    std::vector<CameraInfo> cameras;
    for (const WMCF::MediaFrameSourceGroup& group : groups) {
        bool hasColor = false;
        bool hasInfrared = false;
        bool builtIn = false;
        for (const WMCF::MediaFrameSourceInfo& info : group.SourceInfos()) {
            const WMCF::MediaFrameSourceKind kind = info.SourceKind();
            if (kind == WMCF::MediaFrameSourceKind::Color) {
                hasColor = true;
                builtIn = builtIn || isBuiltInPanel(info);
            } else if (kind == WMCF::MediaFrameSourceKind::Infrared) {
                hasInfrared = true;
            }
        }
        // Depth-only or other groups cannot give the presence pipeline frames.
        if (!hasColor && !hasInfrared) {
            continue;
        }
        CameraInfo camera;
        camera.id = stableCameraId(group);
        camera.name = wideText(group.DisplayName());
        camera.infrared = isInfraredOnlySource(hasColor, hasInfrared);
        camera.builtIn = builtIn && !camera.infrared;
        cameras.push_back(std::move(camera));
    }
    return cameras;
}

// Copies a BGRA8 bitmap into tightly packed rows. The bitmap is only read, never closed.
bool copyBgra(const WGI::SoftwareBitmap& source, CameraFrame& out)
{
    WGI::SoftwareBitmap bitmap = source;
    bool converted = false;
    if (bitmap.BitmapPixelFormat() != WGI::BitmapPixelFormat::Bgra8) {
        // Alpha is ignored by the detector, so the ignore mode avoids unpremultiply work.
        bitmap = WGI::SoftwareBitmap::Convert(source, WGI::BitmapPixelFormat::Bgra8, WGI::BitmapAlphaMode::Ignore);
        converted = true;
    }
    ScopeExit closeConverted{[&] {
        if (converted) {
            bitmap.Close();
        }
    }};

    const int32_t width = bitmap.PixelWidth();
    const int32_t height = bitmap.PixelHeight();
    if (width <= 0 || height <= 0) {
        return false;
    }
    const uint32_t byteCount = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4u;
    WSS::Buffer buffer(byteCount);
    buffer.Length(byteCount);
    bitmap.CopyToBuffer(buffer);
    if (buffer.Length() != byteCount) {
        return false;
    }
    out.bgra.resize(byteCount);
    WSS::DataReader::FromBuffer(buffer).ReadBytes(winrt::array_view<uint8_t>(out.bgra));
    out.width = width;
    out.height = height;
    out.stride = width * 4;
    return true;
}

void handleFrameArrived(Session& s, const WMCF::MediaFrameReader& reader)
{
    withDelivery(s, [&] {
        const WMCF::MediaFrameReference frame = reader.TryAcquireLatestFrame();
        if (!frame) {
            return;
        }
        const double now = s.clock.steadySeconds();
        // Frames above the target rate are released at once so the reader's pool keeps turning.
        if (!shouldRetainFrame(s.lastRetainedSeconds, now, s.targetFps)) {
            frame.Close();
            return;
        }

        CameraFrame cameraFrame;
        cameraFrame.steadySeconds = now;
        bool copied = false;
        try {
            const WMCF::VideoMediaFrame video = frame.VideoMediaFrame();
            const WGI::SoftwareBitmap bitmap = video ? video.SoftwareBitmap() : WGI::SoftwareBitmap{nullptr};
            copied = bitmap && copyBgra(bitmap, cameraFrame);
        } catch (const winrt::hresult_error&) {
            copied = false;
        }
        // The copy is complete, so the frame can be released before the callback runs.
        frame.Close();

        if (!copied) {
            CameraFailure failure;
            failure.conversionFailed = true;
            failure.devicePresent = s.devicePresent.load();
            s.callbacks.onFailure(failure);
            return;
        }
        s.lastRetainedSeconds = now;
        s.callbacks.onFrame(cameraFrame);
    });
}

// Returns nullopt when capture is running. On failure the caller must call teardown().
std::optional<CameraFailure> startCapture(const std::shared_ptr<Session>& session, const std::string& cameraId)
{
    Session& s = *session;
    const std::weak_ptr<Session> weak = session;

    // Defaults describe a present device whose start failed. Each branch overrides them.
    CameraFailure failure;
    failure.startFailed = true;
    try {
        const WMCF::MediaFrameSourceGroup group = findGroup(cameraId);
        if (!group) {
            failure.devicePresent = false;
            failure.startFailed = false;
            return failure;
        }

        WMCF::MediaFrameSourceInfo colorInfo{nullptr};
        for (const WMCF::MediaFrameSourceInfo& info : group.SourceInfos()) {
            if (info.SourceKind() == WMCF::MediaFrameSourceKind::Color) {
                colorInfo = info;
                break;
            }
        }
        if (!colorInfo) {
            // Infrared-only groups are hidden by the selection layer and never opened here.
            failure.devicePresent = false;
            failure.startFailed = false;
            return failure;
        }

        WMC::MediaCaptureInitializationSettings settings;
        settings.SourceGroup(group);
        // Shared read-only lets other apps keep the camera, and the frame reader does not need exclusive access.
        settings.SharingMode(WMC::MediaCaptureSharingMode::SharedReadOnly);
        settings.StreamingCaptureMode(WMC::StreamingCaptureMode::Video);
        settings.MemoryPreference(WMC::MediaCaptureMemoryPreference::Cpu);
        s.capture = WMC::MediaCapture();
        s.capture.InitializeAsync(settings).get();

        s.captureFailedToken = s.capture.Failed([weak](const WMC::MediaCapture&, const WMC::MediaCaptureFailedEventArgs& args) {
            if (const std::shared_ptr<Session> live = weak.lock()) {
                CameraFailure report;
                report.hresult = static_cast<uint32_t>(args.Code());
                report.devicePresent = live->devicePresent.load();
                report.startFailed = true;
                withDelivery(*live, [&] { live->callbacks.onFailure(report); });
            }
        });

        const auto sources = s.capture.FrameSources();
        const winrt::hstring sourceId = colorInfo.Id();
        if (!sources.HasKey(sourceId)) {
            failure.devicePresent = false;
            failure.startFailed = false;
            return failure;
        }
        const WMCF::MediaFrameSource source = sources.Lookup(sourceId);

        const auto formats = source.SupportedFormats();
        std::vector<CameraFormatCandidate> candidates;
        candidates.reserve(formats.Size());
        for (const WMCF::MediaFrameFormat& format : formats) {
            CameraFormatCandidate candidate;
            const WMCF::VideoMediaFrameFormat video = format.VideoFormat();
            if (video) {
                candidate.width = video.Width();
                candidate.height = video.Height();
                candidate.subtype = winrt::to_string(format.Subtype());
                const auto rate = format.FrameRate();
                candidate.fps = rate.Denominator() == 0
                                    ? 0.0
                                    : static_cast<double>(rate.Numerator()) / static_cast<double>(rate.Denominator());
            }
            candidates.push_back(std::move(candidate));
        }
        const std::size_t index = chooseCameraFormat(candidates);
        if (index == kNoCameraFormat) {
            // No format the reader can use: reported as the device not delivering frames.
            failure.devicePresent = false;
            failure.startFailed = false;
            return failure;
        }
        source.SetFormatAsync(formats.GetAt(static_cast<uint32_t>(index))).get();

        s.reader = s.capture.CreateFrameReaderAsync(source, WMP::MediaEncodingSubtypes::Bgra8()).get();
        s.reader.AcquisitionMode(WMCF::MediaFrameReaderAcquisitionMode::Realtime);
        s.frameArrivedToken = s.reader.FrameArrived([weak](const WMCF::MediaFrameReader& sender, const WMCF::MediaFrameArrivedEventArgs&) {
            if (const std::shared_ptr<Session> live = weak.lock()) {
                handleFrameArrived(*live, sender);
            }
        });

        s.watcher = WDE::DeviceInformation::CreateWatcher(WDE::DeviceClass::VideoCapture);
        s.watcherAddedToken = s.watcher.Added([weak](const WDE::DeviceWatcher&, const WDE::DeviceInformation&) {
            notifyDevicesChanged(weak);
        });
        s.watcherRemovedToken = s.watcher.Removed([weak](const WDE::DeviceWatcher&, const WDE::DeviceInformationUpdate& update) {
            if (const std::shared_ptr<Session> live = weak.lock()) {
                if (winrt::to_string(update.Id()) == live->cameraId) {
                    live->devicePresent.store(false);
                }
            }
            notifyDevicesChanged(weak);
        });
        s.watcherUpdatedToken = s.watcher.Updated([weak](const WDE::DeviceWatcher&, const WDE::DeviceInformationUpdate&) {
            notifyDevicesChanged(weak);
        });
        s.watcherCompletedToken = s.watcher.EnumerationCompleted([weak](const WDE::DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) {
            if (const std::shared_ptr<Session> live = weak.lock()) {
                live->watcherReady.store(true);
            }
        });

        const WMCF::MediaFrameReaderStartStatus status = s.reader.StartAsync().get();
        if (status != WMCF::MediaFrameReaderStartStatus::Success) {
            failure.startFailed = true;
            return failure;
        }
        s.watcher.Start();
        return std::nullopt;
    } catch (const winrt::hresult_error& error) {
        // E_ACCESSDENIED from InitializeAsync maps to permission in classifyCameraFailure.
        failure.hresult = static_cast<uint32_t>(error.code());
        failure.startFailed = true;
        return failure;
    }
}

// Clears alive, then passes through the delivery lock so no callback runs once this returns.
// The recursive lock is also safe on the delivery thread; other threads wait for the running callback.
void markDead(Session& s)
{
    s.alive.store(false);
    { std::lock_guard<std::recursive_mutex> barrier(s.deliveryMutex); }
}

// Stops and releases the WinRT objects. StopAsync waits for in-flight handlers, so this must not run on the delivery thread.
void release(Session& s)
{
    if (s.watcher) {
        bestEffort([&] {
            s.watcher.Added(s.watcherAddedToken);
            s.watcher.Removed(s.watcherRemovedToken);
            s.watcher.Updated(s.watcherUpdatedToken);
            s.watcher.EnumerationCompleted(s.watcherCompletedToken);
            s.watcher.Stop();
        });
    }
    if (s.reader) {
        bestEffort([&] {
            s.reader.FrameArrived(s.frameArrivedToken);
            s.reader.StopAsync().get();
            s.reader.Close();
        });
    }
    if (s.capture) {
        bestEffort([&] {
            s.capture.Failed(s.captureFailedToken);
            s.capture.Close();
        });
    }
}

// For a session that never delivered frames (open() failure), so the inline form is safe.
void teardown(Session& s)
{
    markDead(s);
    release(s);
}

}  // namespace

struct MediaCaptureBackend::Impl {
    explicit Impl(const IClock& clockRef) : clock(clockRef) {}

    const IClock& clock;
    std::mutex stateMutex;  // guards session; never held while a callback runs
    std::shared_ptr<Session> session;
};

MediaCaptureBackend::MediaCaptureBackend(const IClock& clock) : impl_(std::make_unique<Impl>(clock)) {}

MediaCaptureBackend::~MediaCaptureBackend()
{
    close();
}

std::vector<CameraInfo> MediaCaptureBackend::enumerate()
{
    // The device list is what other apps show. Groups only classify infrared devices, so a
    // virtual group that shares a device id never adds a second entry.
    const std::vector<CameraDeviceRecord> devices = readVideoCaptureDevices();
    const std::vector<WMCF::MediaFrameSourceGroup> groups = readFrameSourceGroups();
    if (!devices.empty()) {
        std::vector<CameraGroupRecord> records;
        records.reserve(groups.size());
        for (const WMCF::MediaFrameSourceGroup& group : groups) {
            records.push_back(groupRecord(group));
        }
        // Applied to the final list as well: no two entries may share an id, whatever the merge produced.
        return dedupeCameras(mergeCameraLists(devices, records), devices);
    }
    // No device list: an empty result means nothing is selectable. Duplicate ids are removed.
    return dedupeCameras(groupCameras(groups), devices);
}

bool MediaCaptureBackend::open(const std::string& cameraId, double targetFps, ICameraBackendCallbacks& callbacks)
{
    close();

    const auto session = std::make_shared<Session>(impl_->clock, callbacks, targetFps, cameraId);
    if (const std::optional<CameraFailure> failure = startCapture(session, cameraId)) {
        teardown(*session);
        callbacks.onFailure(*failure);
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->session = session;
    return true;
}

void MediaCaptureBackend::close()
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        session = std::move(impl_->session);
    }
    if (session) {
        markDead(*session);
        // StopAsync must not be awaited from FrameArrived, so release on a detached thread
        // when close() is called from a callback. The lambda keeps the Session alive.
        if (tInDelivery) {
            std::thread([session] { release(*session); }).detach();
        } else {
            release(*session);
        }
    }
}

bool MediaCaptureBackend::isOpen() const
{
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->session && impl_->session->alive.load();
}

}  // namespace telaviva
