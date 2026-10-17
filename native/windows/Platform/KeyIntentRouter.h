#pragma once
// Volume and mute key routing, ported from native/macos/TelaViva/HardwareKeyRouter.swift.
// Pure logic with no OS, hook or device calls. The caller feeds key events and the owner
// monitor's volume observation, submits the intents it takes as set_volume with the
// connection generation, and reports every taken intent back through intentFinished.
// Not synchronized and holds no thread: the owning key-hook thread serializes all calls.
#include "IClock.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {

// HardwareKeyRouter.swift:149-152 (defaultStep, minStep, maxStep, fineStep).
inline constexpr double kKeyDefaultStep = 5.0;
inline constexpr double kKeyMinStep = 1.0;
inline constexpr double kKeyMaxStep = 15.0;
inline constexpr double kKeyFineStep = 1.0;
// HardwareKeyRouter.swift:17 (HardwareKeyFeatureState.tolerance): readback distance that counts as reached.
inline constexpr double kKeyReadbackTolerance = 0.51;
// HardwareKeyRouter.swift:299 (rebaseSettleMs): rebases wait this long after one of our writes completes, in ms.
inline constexpr uint64_t kKeyRebaseSettleMs = 1000;
// HardwareKeyRouter.swift:301 (outstandingExpiryMs): an accepted write without completion stops blocking after this, in ms.
inline constexpr uint64_t kKeyOutstandingExpiryMs = 10000;
// Swift bounds: intents (:479, :824), in-flight keys (:825, :853), mute memory (:809),
// outstanding acceptances per key and in total (:531-532), early completions (:523).
inline constexpr std::size_t kKeyMaxIntents = 128;
inline constexpr std::size_t kKeyMaxInFlight = 128;
inline constexpr std::size_t kKeyMaxMuteMemory = 128;
inline constexpr std::size_t kKeyMaxOutstanding = 128;
inline constexpr std::size_t kKeyMaxEarlyCompleted = 64;

// Mute is a volume key of its own; it toggles the owner's volume instead of stepping it.
enum class VolumeKey { Up, Down, Mute };
enum class KeyPhase { Down, Up };

struct KeyEvent {
    VolumeKey key = VolumeKey::Up;
    KeyPhase phase = KeyPhase::Down;
    bool isRepeat = false;
    bool isFineStep = false;        // Shift+Option: one point whatever the configured step is
    bool modifiersEligible = true;  // any other modifier: the press passes through
};

// Latest observation of the owner monitor's volume. percent is NaN when the value is unknown.
struct VolumeObservation {
    std::string monitorId;
    uint64_t connectionGeneration = 0;
    double percent = 0.0;
    bool supported = false;
    bool awake = false;
    bool pending = false;  // the core lists a pending action for this monitor
    bool failed = false;   // the last command for this monitor failed
};

// Route inputs: hardware_keys_enabled, the owner monitor (its audio endpoint container id equals the
// monitor's; empty means no owner) and volume_key_step.
struct KeyRouteConfig {
    bool enabled = false;
    std::string ownerMonitorId;
    double step = kKeyDefaultStep;
};

// A volume write to submit as set_volume with connectionGeneration. Mute is a write to 0.
// revision and authorizationEpoch identify this write for isIntentCurrent and intentFinished.
struct KeyIntent {
    std::string monitorId;
    double targetVolume = 0.0;  // 0..100
    uint64_t generation = 0;    // connection generation
    uint64_t revision = 0;
    uint64_t authorizationEpoch = 0;
};

class KeyIntentRouter {
public:
    // The clock must outlive the router. Its steady reading drives the settle window and the outstanding expiry.
    explicit KeyIntentRouter(IClock& clock);

    KeyIntentRouter(const KeyIntentRouter&) = delete;
    KeyIntentRouter& operator=(const KeyIntentRouter&) = delete;

    // Stores enabled, owner and step, clamped to whole points in 1..15. Disabling clears the owner and all
    // pending state. Changing the owner forgets remembered mute levels. Ignored after shutdown().
    void configure(const KeyRouteConfig& config);

    // Stores the owner monitor's latest volume and reconciles pending intents with its readback.
    // Ignored after shutdown().
    void observe(const VolumeObservation& observation);

    // Returns true when the key event is consumed, false when it must reach Windows untouched.
    // The decision is latched per key, so the repeats and the release of a press get the same answer.
    bool handleKey(const KeyEvent& event);

    // Intents to submit now, in the order they were produced. Each one must be reported through
    // intentFinished, including an intent that is not submitted.
    std::vector<KeyIntent> takeIntents();

    // Submission result of an intent returned by takeIntents. A rejection rebases the next press to the readback.
    void intentFinished(const KeyIntent& intent, bool accepted);

    // Core progress of one of our writes; actionGeneration is the action generation from the snapshot.
    void actionAccepted(const std::string& monitorId, uint64_t generation, uint64_t actionGeneration);
    void actionCompleted(const std::string& monitorId, uint64_t generation, uint64_t actionGeneration);

    // A slider change, reported just before its manual set_volume is submitted, and that
    // submission's acceptance. Only the exact target carrying this revision may rebase.
    void manualControlChanged(const std::string& monitorId, uint64_t generation, double value, uint64_t revision);
    void manualControlAccepted(const std::string& monitorId, uint64_t generation, double value, uint64_t revision);

    // True while the intent still describes the write the router wants.
    bool isIntentCurrent(const KeyIntent& intent) const;

    // Stops all work: pending state and the owner are dropped, and later calls are ignored.
    // handleKey passes every key through afterwards.
    void shutdown();

    // Step arithmetic, exposed for tests. clampedStep maps non-finite input to kKeyDefaultStep.
    static double clampedStep(double value);
    static double steppedVolume(double value, bool increase, bool fine, double step);

private:
    using Key = std::pair<std::string, uint64_t>;  // monitor id, connection generation

    enum class LatchKind { PassThrough, Handled, Mute };

    struct Latch {
        LatchKind kind = LatchKind::PassThrough;
        Key key;
        bool increase = false;
        bool fine = false;
        bool cancelled = false;
    };

    struct Intent {
        double target = 0.0;
        uint64_t revision = 0;
        uint64_t authorizationEpoch = 0;
        std::optional<uint64_t> manualRevision;
        std::optional<double> lastObserved;
        bool scheduled = false;
    };

    static std::size_t index(VolumeKey key) { return static_cast<std::size_t>(key); }
    uint64_t nowMs() const;
    const VolumeObservation* displayFor(const Key& key) const;
    bool isOwner(const VolumeObservation& observation) const;
    const VolumeObservation* ownerDisplay() const;
    std::optional<double> observedValue(const Key& key) const;
    std::optional<double> valueFor(const Key& key) const;

    bool process(const KeyEvent& event);
    bool reserveStep(const Key& key, bool increase, bool fine, double value);
    bool reserveMute(const Key& key, double value);
    bool reserveWrite(const Key& key, double value, double next);
    void flush(const Key& key);
    void complete(const Key& key, uint64_t revision, uint64_t epoch, bool accepted);
    void reconcile();
    void pruneRebaseState(uint64_t now);
    void forgetRebaseState(const Key& key);
    void registerOutstanding(const Key& key, uint64_t actionGeneration, uint64_t now);
    void dropOldestOutstanding(const Key* only);
    void clearPending();

    IClock& clock_;
    bool enabled_ = false;
    bool stopped_ = false;
    std::string owner_;
    double step_ = kKeyDefaultStep;
    std::optional<VolumeObservation> observation_;
    // Indexed by VolumeKey. An engaged latch is a press in progress; an empty one is no press.
    std::array<std::optional<Latch>, 3> latches_;
    std::map<Key, Intent> intents_;
    std::map<Key, double> muteMemory_;
    std::set<Key> inFlight_;
    std::map<Key, std::map<uint64_t, uint64_t>> outstanding_;  // actionGeneration -> acceptance ms
    std::map<Key, uint64_t> rebaseBlockedUntil_;               // ms
    std::map<Key, std::set<uint64_t>> earlyCompleted_;
    std::vector<KeyIntent> ready_;
    uint64_t intentRevision_ = 0;
};

}  // namespace telaviva
