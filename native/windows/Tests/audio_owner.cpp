#include "TestHarness.h"

#include "../Platform/AudioOwner.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* kContainerA = "{11111111-2222-3333-4444-aaaaaaaaaaaa}";
const char* kContainerB = "{11111111-2222-3333-4444-bbbbbbbbbbbb}";

class FakeAudioEndpoints final : public telaviva::IAudioEndpoints {
public:
    std::optional<telaviva::EndpointInfo> defaultRenderEndpoint() override { return endpoint; }

    void setChangeCallback(std::function<void()> onChange) override { callback = std::move(onChange); }

    // Simulates IMMNotificationClient::OnDefaultDeviceChanged.
    void fireChange()
    {
        if (callback) {
            callback();
        }
    }

    std::optional<telaviva::EndpointInfo> endpoint;
    std::function<void()> callback;
};

class FakeMonitorContainers final : public telaviva::IMonitorContainers {
public:
    std::optional<std::string> containerIdFor(const std::string& monitorId) override
    {
        const auto it = containers.find(monitorId);
        if (it == containers.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // A monitor missing from this map has no container id.
    std::map<std::string, std::string> containers;
};

std::string describe(const std::optional<std::string>& value)
{
    return value ? *value : std::string("none");
}

}  // namespace

TEST(audio_owner_container_match_gives_owner)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    containers.containers["mon-b"] = kContainerB;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a", "mon-b"});
    CHECK_EQ(describe(audio.owner()), std::string("mon-a"));
}

TEST(audio_owner_no_endpoint_container_gives_none)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", ""};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_no_default_endpoint_gives_none)
{
    FakeAudioEndpoints endpoints;
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_monitor_without_container_is_ignored)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    // mon-b has no container id. It cannot match, and mon-a still owns the output.
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-b", "mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("mon-a"));
}

TEST(audio_owner_only_monitors_without_container_gives_none)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a", "mon-b"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_no_match_gives_none)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerB;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_two_monitors_same_container_gives_none)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    containers.containers["mon-b"] = kContainerA;
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a", "mon-b"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_case_and_braces_still_match)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", "{11111111-2222-3333-4444-AAAAAAAAAAAA}"};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = "11111111-2222-3333-4444-aaaaaaaaaaaa";
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("mon-a"));
}

TEST(audio_owner_placeholder_container_is_none)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", "{00000000-0000-0000-FFFF-FFFFFFFFFFFF}"};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = "{00000000-0000-0000-ffff-ffffffffffff}";
    telaviva::AudioOwner audio(endpoints, containers, nullptr);
    audio.onMonitorsChanged({"mon-a"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
}

TEST(audio_owner_normalize_container_id)
{
    CHECK_EQ(telaviva::normalizeContainerId("{ABCDEF01-2345-6789-ABCD-EF0123456789}"),
             std::string("abcdef01-2345-6789-abcd-ef0123456789"));
    CHECK_EQ(telaviva::normalizeContainerId(""), std::string(""));
}

TEST(audio_owner_default_output_change_republishes)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    containers.containers["mon-b"] = kContainerB;
    std::vector<std::string> published;
    telaviva::AudioOwner audio(endpoints, containers, [&published](std::optional<std::string> owner) {
        published.push_back(describe(owner));
    });
    audio.onMonitorsChanged({"mon-a", "mon-b"});
    CHECK_EQ(published.size(), std::size_t(1));

    // The default output moves to mon-b's container.
    endpoints.endpoint = telaviva::EndpointInfo{"render-2", kContainerB};
    endpoints.fireChange();
    CHECK_EQ(published.size(), std::size_t(2));
    if (published.size() == 2) {
        CHECK_EQ(published[1], std::string("mon-b"));
    }

    // A change that leaves the owner as it was publishes nothing.
    endpoints.fireChange();
    CHECK_EQ(published.size(), std::size_t(2));

    // The output moves to a device with no container, so the owner clears.
    endpoints.endpoint = telaviva::EndpointInfo{"render-3", ""};
    endpoints.fireChange();
    CHECK_EQ(published.size(), std::size_t(3));
    if (published.size() == 3) {
        CHECK_EQ(published[2], std::string("none"));
    }
}

TEST(audio_owner_owner_disappears_when_monitor_removed)
{
    FakeAudioEndpoints endpoints;
    endpoints.endpoint = telaviva::EndpointInfo{"render-1", kContainerA};
    FakeMonitorContainers containers;
    containers.containers["mon-a"] = kContainerA;
    containers.containers["mon-b"] = kContainerB;
    std::vector<std::string> published;
    telaviva::AudioOwner audio(endpoints, containers, [&published](std::optional<std::string> owner) {
        published.push_back(describe(owner));
    });
    audio.onMonitorsChanged({"mon-a", "mon-b"});
    CHECK_EQ(describe(audio.owner()), std::string("mon-a"));

    // mon-a is unplugged; the endpoint still points at its container.
    audio.onMonitorsChanged({"mon-b"});
    CHECK_EQ(describe(audio.owner()), std::string("none"));
    CHECK_EQ(published.size(), std::size_t(2));
    if (published.size() == 2) {
        CHECK_EQ(published[1], std::string("none"));
    }
}
