#include "TestHarness.h"

#include "Platform/DdcTimingProfile.h"
#include "Platform/DdcTransaction.h"
#include "Platform/IDdcPort.h"
#include "Platform/ISleeper.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using telaviva::DdcError;
using telaviva::DdcReading;
using telaviva::DdcResult;
using telaviva::DdcTimingProfile;
using telaviva::DdcTransaction;
using telaviva::IDdcPort;
using telaviva::ISleeper;

namespace {

constexpr uint8_t kBrightness = 0x10;
constexpr uint32_t kMaximum = 100;
constexpr uint32_t kNoValue = 0xFFFFFFFFu;

// Ordered record of port calls and sleeps, so tests can assert interleaving.
struct Trace {
    std::vector<std::string> events;

    std::string joined() const
    {
        std::string out;
        for (const std::string& event : events) {
            if (!out.empty()) {
                out += " | ";
            }
            out += event;
        }
        return out;
    }
};

std::string millisList(const std::vector<std::chrono::milliseconds>& values)
{
    std::string out;
    for (const auto value : values) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(value.count());
    }
    return out;
}

// Device model: a successful write is stored and later read back. Scripted
// read and write results are consumed in order and override that model.
class FakePort final : public IDdcPort {
public:
    explicit FakePort(Trace& trace) : trace_(trace) {}

    void queueRead(bool ok, uint32_t current, DdcError error)
    {
        readSteps_.push_back(ReadStep{ok, current, error});
    }

    void queueWrite(bool ok, DdcError error)
    {
        writeSteps_.push_back(WriteStep{ok, error});
    }

    void setStored(uint8_t code, uint32_t value) { stored_[code] = value; }

    bool readVcp(uint8_t code, DdcReading& out, DdcError& error) override
    {
        trace_.events.push_back("read " + std::to_string(code));
        if (!readSteps_.empty()) {
            const ReadStep step = readSteps_.front();
            readSteps_.pop_front();
            if (!step.ok) {
                error = step.error;
                return false;
            }
            out.current = step.current;
            out.maximum = kMaximum;
            error = DdcError::None;
            return true;
        }
        out.current = stored_[code];
        out.maximum = kMaximum;
        error = DdcError::None;
        return true;
    }

    bool writeVcp(uint8_t code, uint32_t value, DdcError& error) override
    {
        trace_.events.push_back("write " + std::to_string(code) + "=" + std::to_string(value));
        if (!writeSteps_.empty()) {
            const WriteStep step = writeSteps_.front();
            writeSteps_.pop_front();
            if (!step.ok) {
                error = step.error;
                return false;
            }
        }
        stored_[code] = value;
        error = DdcError::None;
        return true;
    }

private:
    struct ReadStep {
        bool ok;
        uint32_t current;
        DdcError error;
    };

    struct WriteStep {
        bool ok;
        DdcError error;
    };

    Trace& trace_;
    std::deque<ReadStep> readSteps_;
    std::deque<WriteStep> writeSteps_;
    std::map<uint8_t, uint32_t> stored_;
};

class FakeSleeper final : public ISleeper {
public:
    explicit FakeSleeper(Trace& trace) : trace_(trace) {}

    void sleepFor(std::chrono::milliseconds duration) override
    {
        durations_.push_back(duration);
        trace_.events.push_back("sleep " + std::to_string(duration.count()));
    }

    const std::vector<std::chrono::milliseconds>& durations() const { return durations_; }

private:
    Trace& trace_;
    std::vector<std::chrono::milliseconds> durations_;
};

// Declaration order matters: the port and sleeper keep references to the trace.
struct Rig {
    Trace trace;
    FakePort port{trace};
    FakeSleeper sleeper{trace};
};

}  // namespace

TEST(ddc_transaction_set_writes_waits_then_reads_back)
{
    Rig rig;
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.setAndVerify(kBrightness, 60);
    CHECK(result.ok);
    CHECK_EQ(result.value.value_or(kNoValue), 60u);
    CHECK_EQ(result.error, std::string(""));
    CHECK_EQ(rig.trace.joined(), std::string("write 16=60 | sleep 60 | read 16"));
    CHECK_EQ(millisList(rig.sleeper.durations()), std::string("60"));
}

TEST(ddc_transaction_set_sleeps_exactly_the_profile_delay)
{
    Rig rig;
    DdcTimingProfile profile;
    profile.interTransactionDelay = 25ms;
    DdcTransaction tx(rig.port, rig.sleeper, profile);
    CHECK(tx.setAndVerify(kBrightness, 40).ok);
    CHECK_EQ(millisList(rig.sleeper.durations()), std::string("25"));
}

TEST(ddc_transaction_duplicate_set_writes_twice_and_verifies_once)
{
    Rig rig;
    DdcTimingProfile profile;
    profile.duplicateSet = true;
    DdcTransaction tx(rig.port, rig.sleeper, profile);
    const DdcResult result = tx.setAndVerify(kBrightness, 60);
    CHECK(result.ok);
    CHECK_EQ(result.value.value_or(kNoValue), 60u);
    CHECK_EQ(rig.trace.joined(),
             std::string("write 16=60 | sleep 10 | write 16=60 | sleep 60 | read 16"));
    CHECK_EQ(millisList(rig.sleeper.durations()), std::string("10,60"));
}

TEST(ddc_transaction_readback_mismatch_reports_observed_value_as_io)
{
    Rig rig;
    rig.port.queueRead(true, 58, DdcError::None);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.setAndVerify(kBrightness, 60);
    CHECK(!result.ok);
    CHECK_EQ(result.value.value_or(kNoValue), 58u);
    CHECK_EQ(result.error, std::string("io"));
    CHECK_EQ(rig.trace.joined(), std::string("write 16=60 | sleep 60 | read 16"));
}

TEST(ddc_transaction_write_failure_reports_error_without_readback)
{
    Rig rig;
    rig.port.queueWrite(false, DdcError::Io);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.setAndVerify(kBrightness, 60);
    CHECK(!result.ok);
    CHECK(!result.value.has_value());
    CHECK_EQ(result.error, std::string("io"));
    CHECK(rig.trace.joined().find("read") == std::string::npos);
}

TEST(ddc_transaction_write_unsupported_and_disconnected_map_without_retry)
{
    {
        Rig rig;
        rig.port.queueWrite(false, DdcError::Unsupported);
        DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
        const DdcResult result = tx.setAndVerify(kBrightness, 60);
        CHECK(!result.ok);
        CHECK_EQ(result.error, std::string("unsupported"));
        CHECK_EQ(rig.trace.joined(), std::string("write 16=60"));
    }
    {
        Rig rig;
        rig.port.queueWrite(false, DdcError::Disconnected);
        DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
        const DdcResult result = tx.setAndVerify(kBrightness, 60);
        CHECK(!result.ok);
        CHECK_EQ(result.error, std::string("disconnected"));
        CHECK_EQ(rig.trace.joined(), std::string("write 16=60"));
    }
}

TEST(ddc_transaction_read_returns_current_value)
{
    Rig rig;
    rig.port.setStored(kBrightness, 41);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.readVcp(kBrightness);
    CHECK(result.ok);
    CHECK_EQ(result.value.value_or(kNoValue), 41u);
    CHECK_EQ(result.error, std::string(""));
    CHECK_EQ(rig.trace.joined(), std::string("read 16"));
    CHECK(rig.sleeper.durations().empty());
}

TEST(ddc_transaction_read_retry_succeeds_after_first_delay_of_200ms)
{
    Rig rig;
    rig.port.queueRead(false, 0, DdcError::Io);
    rig.port.queueRead(true, 41, DdcError::None);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.readVcp(kBrightness);
    CHECK(result.ok);
    CHECK_EQ(result.value.value_or(kNoValue), 41u);
    CHECK_EQ(rig.trace.joined(), std::string("read 16 | sleep 200 | read 16"));
    CHECK_EQ(millisList(rig.sleeper.durations()), std::string("200"));
}

TEST(ddc_transaction_two_read_failures_back_off_200_then_1000_and_report_last_error)
{
    Rig rig;
    rig.port.queueRead(false, 0, DdcError::Io);
    rig.port.queueRead(false, 0, DdcError::Io);
    rig.port.queueRead(false, 0, DdcError::Io);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.readVcp(kBrightness);
    CHECK(!result.ok);
    CHECK(!result.value.has_value());
    CHECK_EQ(result.error, std::string("io"));
    CHECK_EQ(rig.trace.joined(),
             std::string("read 16 | sleep 200 | read 16 | sleep 1000 | read 16"));
    CHECK_EQ(millisList(rig.sleeper.durations()), std::string("200,1000"));
}

TEST(ddc_transaction_read_unsupported_is_not_retried)
{
    Rig rig;
    rig.port.queueRead(false, 0, DdcError::Unsupported);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.readVcp(kBrightness);
    CHECK(!result.ok);
    CHECK_EQ(result.error, std::string("unsupported"));
    CHECK_EQ(rig.trace.joined(), std::string("read 16"));
    CHECK(rig.sleeper.durations().empty());
}

TEST(ddc_transaction_read_disconnected_is_not_retried)
{
    Rig rig;
    rig.port.queueRead(false, 0, DdcError::Disconnected);
    DdcTransaction tx(rig.port, rig.sleeper, DdcTimingProfile{});
    const DdcResult result = tx.readVcp(kBrightness);
    CHECK(!result.ok);
    CHECK_EQ(result.error, std::string("disconnected"));
    CHECK_EQ(rig.trace.joined(), std::string("read 16"));
    CHECK(rig.sleeper.durations().empty());
}

TEST(ddc_transaction_default_profile_matches_macos_timing)
{
    const DdcTimingProfile profile;
    CHECK_EQ(profile.interTransactionDelay.count(), 60);
    CHECK(!profile.duplicateSet);
    CHECK_EQ(profile.duplicateGap.count(), 10);
    CHECK_EQ(millisList(profile.readRetryDelays), std::string("200,1000"));
    CHECK_EQ(profile.wakeGrace.count(), 2000);
}

TEST(ddc_transaction_gsm_manufacturer_profile_duplicates_set)
{
    CHECK(DdcTimingProfile::forManufacturer("GSM").duplicateSet);
    CHECK(!DdcTimingProfile::forManufacturer("DEL").duplicateSet);
    CHECK(!DdcTimingProfile::forManufacturer("").duplicateSet);
}
