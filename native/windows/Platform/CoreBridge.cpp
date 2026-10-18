#include "CoreBridge.h"

#include "RevisionGate.h"

#include <windows.h>

#include <telaviva_core.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

// Shared with the core callback, which only records the revision and wakes the worker.
struct WorkerSignal {
    std::mutex mutex;
    std::condition_variable changed;
    bool revisionPending = false;
    bool stopping = false;
    uint64_t latestRevision = 0;
};

// The core calls this inside tv_submit, under the core mutex. It must not call
// back into the core, so it only touches the signal's own mutex.
extern "C" void onSnapshotReady(uint64_t revision, void* userdata)
{
    auto* state = static_cast<WorkerSignal*>(userdata);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->latestRevision = revision;
        state->revisionPending = true;
    }
    state->changed.notify_one();
}

// Frees the core buffer on every exit path, including a throwing copy.
struct SnapshotBuffer {
    TvBuffer buffer{nullptr, 0};

    ~SnapshotBuffer()
    {
        if (buffer.data != nullptr) {
            tv_buffer_free(&buffer);
        }
    }
};

// Same apartment setup as InferenceFailureStore.cpp; returns false when WinRT cannot start here.
bool ensureApartment()
{
    thread_local bool initialized = false;
    if (initialized) {
        return true;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        // The thread already has another apartment; parsing works there too.
        if (error.code() != static_cast<winrt::hresult>(RPC_E_CHANGED_MODE)) {
            return false;
        }
    }
    initialized = true;
    return true;
}

// Absent or non-string members give nullopt.
std::optional<std::string> stringField(const winrt::Windows::Data::Json::JsonObject& object, const wchar_t* name)
{
    if (!object.HasKey(name)) {
        return std::nullopt;
    }
    const winrt::Windows::Data::Json::IJsonValue value = object.GetNamedValue(name);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
        return std::nullopt;
    }
    return winrt::to_string(value.GetString());
}

std::optional<std::string> toUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return std::string{};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), size, nullptr, nullptr) <= 0) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> optionsJson(const CoreBridgeOptions& options)
{
    const std::optional<std::string> path = toUtf8(options.configPath);
    if (!path) {
        return std::nullopt;
    }
    return "{\"config_path\":" + jsonQuote(*path) + ",\"os_locale\":" + jsonQuote(options.osLocale) + "}";
}

}  // namespace

struct CoreBridge::Impl {
    WorkerSignal wake;
    // Serializes every tv_* call and guards core. Declared before core so core is
    // destroyed while the mutex is still alive.
    std::mutex coreMutex;
    std::unique_ptr<TvCore, decltype(&tv_destroy)> core{nullptr, &tv_destroy};

    std::mutex sinkMutex;
    std::vector<SnapshotSink> sinks;

    std::thread worker;
};

CoreBridge::CoreBridge(const CoreBridgeOptions& options) : impl_(std::make_unique<Impl>())
{
    // A mismatched ABI leaves the bridge closed, so the shell can show an error instead of crashing.
    if (tv_abi_version() != TV_CORE_ABI_VERSION) {
        return;
    }
    const std::optional<std::string> json = optionsJson(options);
    if (!json) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->coreMutex);
        TvCore* created = nullptr;
        const int32_t status = tv_create(TV_CORE_ABI_VERSION, reinterpret_cast<const uint8_t*>(json->data()),
                                         json->size(), &onSnapshotReady, &impl_->wake, &created);
        if (status == TV_OK && created != nullptr) {
            impl_->core.reset(created);
        }
    }
    if (!impl_->core) {
        return;
    }

    impl_->worker = std::thread([this] { runWorker(); });
}

CoreBridge::~CoreBridge()
{
    {
        std::lock_guard<std::mutex> lock(impl_->wake.mutex);
        impl_->wake.stopping = true;
    }
    impl_->wake.changed.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    // Destroys the core. Safe after shutdown() or without it.
    std::lock_guard<std::mutex> lock(impl_->coreMutex);
    impl_->core.reset();
}

uint32_t CoreBridge::abiVersion()
{
    return tv_abi_version();
}

std::string CoreBridge::coreVersion()
{
    SnapshotBuffer owned;
    if (tv_core_version(&owned.buffer) != TV_OK || owned.buffer.data == nullptr) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(owned.buffer.data), owned.buffer.len);
}

std::optional<UpdateOutcome> CoreBridge::evaluateUpdate(const std::string& installed, int status,
                                                        const std::string& body)
{
    SnapshotBuffer owned;
    const int32_t code = tv_update_evaluate(reinterpret_cast<const uint8_t*>(installed.data()), installed.size(),
                                            static_cast<uint32_t>(status),
                                            reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                            &owned.buffer);
    if (code != TV_OK || owned.buffer.data == nullptr) {
        return std::nullopt;
    }
    const std::string text(reinterpret_cast<const char*>(owned.buffer.data), owned.buffer.len);

    try {
        if (!ensureApartment()) {
            return std::nullopt;
        }
        winrt::Windows::Data::Json::JsonObject root;
        if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(std::string_view(text)), root)) {
            return std::nullopt;
        }
        const std::optional<std::string> outcomeStatus = stringField(root, L"status");
        if (!outcomeStatus) {
            return std::nullopt;
        }
        UpdateOutcome outcome;
        outcome.status = *outcomeStatus;
        outcome.remoteVersion = stringField(root, L"remote_version").value_or(std::string{});
        outcome.releaseUrl = stringField(root, L"release_url").value_or(std::string{});
        return outcome;
    } catch (...) {
        return std::nullopt;
    }
}

bool CoreBridge::isOpen() const
{
    std::lock_guard<std::mutex> lock(impl_->coreMutex);
    return static_cast<bool>(impl_->core);
}

int32_t CoreBridge::submit(const std::string& json)
{
    std::lock_guard<std::mutex> lock(impl_->coreMutex);
    if (!impl_->core) {
        return TV_CLOSED;
    }
    return tv_submit(impl_->core.get(), reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

Snapshot CoreBridge::snapshot()
{
    std::string text;
    {
        std::lock_guard<std::mutex> lock(impl_->coreMutex);
        if (!impl_->core) {
            throw std::runtime_error("core is not open");
        }
        SnapshotBuffer owned;
        if (tv_snapshot(impl_->core.get(), &owned.buffer) != TV_OK) {
            throw std::runtime_error("tv_snapshot failed");
        }
        if (owned.buffer.data != nullptr) {
            text.assign(reinterpret_cast<const char*>(owned.buffer.data), owned.buffer.len);
        }
    }
    // Parsing runs outside the core mutex so it never delays other tv_* calls.
    return parseSnapshot(text);
}

void CoreBridge::addSink(SnapshotSink sink)
{
    std::lock_guard<std::mutex> lock(impl_->sinkMutex);
    impl_->sinks.push_back(std::move(sink));
}

void CoreBridge::shutdown(std::chrono::milliseconds drainDeadline)
{
    {
        std::lock_guard<std::mutex> lock(impl_->coreMutex);
        if (!impl_->core) {
            return;
        }
        tv_shutdown(impl_->core.get());
    }

    // Restore actions may still be pending on the hardware side; give them a bounded window.
    const auto deadline = std::chrono::steady_clock::now() + drainDeadline;
    for (;;) {
        try {
            if (snapshot().pendingActions.empty()) {
                return;
            }
        } catch (const std::exception&) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void CoreBridge::runWorker()
{
    // Owned by this thread only, so it needs no lock.
    RevisionGate gate;
    for (;;) {
        uint64_t woken = 0;
        {
            std::unique_lock<std::mutex> lock(impl_->wake.mutex);
            impl_->wake.changed.wait(lock, [this] {
                return impl_->wake.stopping || impl_->wake.revisionPending;
            });
            if (impl_->wake.stopping) {
                return;
            }
            impl_->wake.revisionPending = false;
            woken = impl_->wake.latestRevision;
        }
        // Submits that land before the worker reads can leave a second wake for a revision already published.
        if (!gate.shouldFetch(woken)) {
            continue;
        }

        Snapshot next;
        try {
            next = snapshot();
        } catch (const std::exception&) {
            // The next revision signal retries; a single failed read must not stop the worker.
            continue;
        }
        // The fetch may be newer than the wake, or equal to a revision already published; only a newer one is sent.
        if (!gate.shouldPublish(next.revision)) {
            continue;
        }

        std::vector<SnapshotSink> sinks;
        {
            std::lock_guard<std::mutex> lock(impl_->sinkMutex);
            sinks = impl_->sinks;
        }
        for (const SnapshotSink& sink : sinks) {
            try {
                sink(next);
            } catch (...) {
                // A throwing sink must not terminate the worker thread.
            }
        }
        gate.markPublished(next.revision);
    }
}

}  // namespace telaviva
