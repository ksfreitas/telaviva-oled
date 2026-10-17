#include "TestHarness.h"

#include "Platform/CameraSelection.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

telaviva::CameraInfo makeCamera(const std::string& id, bool infrared = false, bool builtIn = false)
{
    telaviva::CameraInfo camera;
    camera.id = id;
    camera.name = L"Test camera";
    camera.infrared = infrared;
    camera.builtIn = builtIn;
    return camera;
}

// Compares the fields that the functions under test could change.
bool sameCameras(const std::vector<telaviva::CameraInfo>& a, const std::vector<telaviva::CameraInfo>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].infrared != b[i].infrared ||
            a[i].builtIn != b[i].builtIn) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(camera_selection_history_prefers_most_recent_connected)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("rgb-a"), makeCamera("rgb-b")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"rgb-a", "rgb-b"});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("rgb-a"));
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_history_skips_disconnected_entry)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("rgb-b"), makeCamera("rgb-c")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"rgb-a", "rgb-b"});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("rgb-b"));
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_history_skips_infrared_entry)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("ir-front", true), makeCamera("rgb-a")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"ir-front", "rgb-a"});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("rgb-a"));
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_default_prefers_builtin_without_history)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("usb-a"), makeCamera("builtin", false, true)};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("builtin"));
    CHECK(choice.fallbackToDefault);
}

TEST(camera_selection_default_prefers_builtin_when_history_missing)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("usb-a"), makeCamera("builtin", false, true)};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"missing"});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("builtin"));
    CHECK(choice.fallbackToDefault);
}

TEST(camera_selection_default_first_non_infrared_without_builtin)
{
    const std::vector<telaviva::CameraInfo> connected = {
        makeCamera("ir-front", true), makeCamera("usb-a"), makeCamera("usb-b")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {});
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("usb-a"));
    CHECK(choice.fallbackToDefault);
}

TEST(camera_selection_only_infrared_gives_nullopt)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("ir-front", true, true)};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"ir-front"});
    CHECK(!choice.effective.has_value());
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_empty_connected_gives_nullopt)
{
    const telaviva::CameraChoice choice = telaviva::chooseCamera({}, {"rgb-a"});
    CHECK(!choice.effective.has_value());
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_select_drops_infrared_and_keeps_order)
{
    const std::vector<telaviva::CameraInfo> connected = {
        makeCamera("rgb-b"), makeCamera("ir-front", true), makeCamera("rgb-a")};
    const std::vector<telaviva::CameraInfo> result = telaviva::selectableCameras(connected);
    CHECK_EQ(result.size(), std::size_t(2));
    if (result.size() != 2) {
        return;
    }
    CHECK_EQ(result[0].id, std::string("rgb-b"));
    CHECK_EQ(result[1].id, std::string("rgb-a"));
}

// Locals keep the braced initializers out of the CHECK_EQ macro arguments.
TEST(camera_selection_lifecycle_id_list_joins_history)
{
    const std::vector<std::string> history = {"rgb-a", "rgb-b"};
    const std::string withoutSelected = telaviva::lifecycleCameraIdList(history, std::nullopt);
    CHECK_EQ(withoutSelected, std::string("rgb-a\nrgb-b"));

    // The selected id is ignored when history exists.
    const std::string withSelected = telaviva::lifecycleCameraIdList(history, std::optional<std::string>("rgb-c"));
    CHECK_EQ(withSelected, std::string("rgb-a\nrgb-b"));
}

TEST(camera_selection_lifecycle_id_list_uses_selected_without_history)
{
    const std::vector<std::string> empty;
    const std::string list = telaviva::lifecycleCameraIdList(empty, std::optional<std::string>("rgb-c"));
    CHECK_EQ(list, std::string("rgb-c"));
}

TEST(camera_selection_lifecycle_id_list_empty_without_either)
{
    const std::vector<std::string> empty;
    const std::string list = telaviva::lifecycleCameraIdList(empty, std::nullopt);
    CHECK_EQ(list, std::string());
}

TEST(camera_selection_inputs_are_not_modified)
{
    const std::vector<telaviva::CameraInfo> connected = {
        makeCamera("ir-front", true), makeCamera("usb-a"), makeCamera("builtin", false, true)};
    const std::vector<std::string> history = {"ir-front", "usb-a", "missing"};
    const std::optional<std::string> selected = std::string("usb-a");

    const std::vector<telaviva::CameraInfo> connectedBefore = connected;
    const std::vector<std::string> historyBefore = history;
    const std::optional<std::string> selectedBefore = selected;

    (void)telaviva::selectableCameras(connected);
    (void)telaviva::chooseCamera(connected, history);
    (void)telaviva::lifecycleCameraIdList(history, selected);

    CHECK(sameCameras(connected, connectedBefore));
    CHECK(history == historyBefore);
    CHECK(selected == selectedBefore);
}
