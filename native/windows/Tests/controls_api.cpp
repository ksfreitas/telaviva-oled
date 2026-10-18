#include "TestHarness.h"

#include "Platform/ControlsApi.h"

#include <cstddef>
#include <string>

using telaviva::CoreControlsApi;

namespace {

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

const char* const kConfigure =
    R"({"type":"configure","now":1.0,"brightness_step":5,"volume_step":5,"keys_enabled":true})";

}  // namespace

TEST(controls_api_round_trip_returns_effects_and_next_deadline)
{
    CoreControlsApi api;
    const std::string reply = api.step(kConfigure);
    CHECK(!reply.empty());
    CHECK(contains(reply, "\"effects\""));
    CHECK(contains(reply, "\"next_deadline\""));
}

TEST(controls_api_malformed_event_returns_empty_text)
{
    CoreControlsApi api;
    CHECK(api.step("not json").empty());
}

TEST(controls_api_oversize_event_returns_empty_text)
{
    CoreControlsApi api;
    // 70000 bytes, above the 64 KiB input limit; the size check runs before any parsing.
    const std::string padding(70000, 'a');
    const std::string oversize = "{\"type\":\"tick\",\"now\":1.0,\"pad\":\"" + padding + "\"}";
    CHECK(api.step(oversize).empty());
}

TEST(controls_api_instances_are_independent)
{
    CoreControlsApi first;
    CoreControlsApi second;
    CHECK(!first.step(kConfigure).empty());
    CHECK(!second.step(R"({"type":"tick","now":2.0})").empty());
    CHECK(!first.step(R"({"type":"tick","now":2.0})").empty());
}

TEST(controls_api_destroy_after_use_does_not_crash)
{
    {
        CoreControlsApi api;
        CHECK(!api.step(kConfigure).empty());
    }
    CoreControlsApi again;
    CHECK(!again.step(kConfigure).empty());
}
