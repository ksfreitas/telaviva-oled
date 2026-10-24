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

namespace {

telaviva::CameraInfo namedCamera(const std::string& id, const std::wstring& name)
{
    telaviva::CameraInfo camera;
    camera.id = id;
    camera.name = name;
    return camera;
}

telaviva::CameraDeviceRecord deviceRecord(const std::string& id, const std::wstring& name)
{
    telaviva::CameraDeviceRecord device;
    device.id = id;
    device.name = name;
    return device;
}

telaviva::CameraSourceRecord sourceRecord(telaviva::CameraSourceKind kind, const std::string& deviceId)
{
    telaviva::CameraSourceRecord source;
    source.kind = kind;
    source.deviceId = deviceId;
    return source;
}

telaviva::CameraGroupRecord groupRecord(const std::wstring& name, const std::vector<telaviva::CameraSourceRecord>& sources)
{
    telaviva::CameraGroupRecord group;
    group.name = name;
    group.sources = sources;
    return group;
}

}  // namespace

TEST(camera_selection_dedupe_keeps_first_of_same_id)
{
    const std::vector<telaviva::CameraInfo> cameras = {
        namedCamera("dup", L"First"), namedCamera("other", L"Other"), namedCamera("DUP", L"Second")};
    const std::vector<telaviva::CameraInfo> result = telaviva::dedupeCameras(cameras, {});
    CHECK_EQ(result.size(), std::size_t(2));
    if (result.size() != 2) {
        return;
    }
    CHECK(result[0].name == L"First");
    CHECK_EQ(result[1].id, std::string("other"));
}

TEST(camera_selection_dedupe_prefers_device_name_over_earlier_duplicate)
{
    const std::vector<telaviva::CameraDeviceRecord> devices = {deviceRecord("rgb", L"Lenovo Performance RGB Camera")};
    const std::vector<telaviva::CameraInfo> cameras = {
        namedCamera("rgb", L"LenovoCam-Group"), namedCamera("rgb", L"Lenovo Performance RGB Camera")};
    const std::vector<telaviva::CameraInfo> result = telaviva::dedupeCameras(cameras, devices);
    CHECK_EQ(result.size(), std::size_t(1));
    if (result.size() != 1) {
        return;
    }
    CHECK(result[0].name == L"Lenovo Performance RGB Camera");
}

TEST(camera_selection_merge_ids_are_unique)
{
    const std::vector<telaviva::CameraDeviceRecord> devices = {
        deviceRecord("rgb", L"RGB"), deviceRecord("RGB", L"RGB")};
    const std::vector<telaviva::CameraInfo> result = telaviva::mergeCameraLists(devices, {});
    CHECK_EQ(result.size(), std::size_t(1));
}

TEST(camera_selection_merge_classifies_infrared_only_device)
{
    const std::vector<telaviva::CameraDeviceRecord> devices = {
        deviceRecord("ir-id", L"IR Cam"), deviceRecord("rgb-id", L"RGB Cam")};
    const std::vector<telaviva::CameraGroupRecord> groups = {
        groupRecord(L"IR Cam", {sourceRecord(telaviva::CameraSourceKind::Infrared, "ir-id")}),
        groupRecord(L"RGB Cam", {sourceRecord(telaviva::CameraSourceKind::Color, "rgb-id")})};
    const std::vector<telaviva::CameraInfo> result = telaviva::mergeCameraLists(devices, groups);
    CHECK_EQ(result.size(), std::size_t(2));
    if (result.size() != 2) {
        return;
    }
    CHECK(result[0].infrared);
    CHECK(!result[1].infrared);
}

TEST(camera_selection_merge_hello_group_is_not_infrared)
{
    // A group with both a color and an infrared source stays a color camera.
    const std::vector<telaviva::CameraDeviceRecord> devices = {deviceRecord("hello", L"Hello")};
    const std::vector<telaviva::CameraGroupRecord> groups = {groupRecord(L"Hello", {
        sourceRecord(telaviva::CameraSourceKind::Color, "hello"),
        sourceRecord(telaviva::CameraSourceKind::Infrared, "hello")})};
    const std::vector<telaviva::CameraInfo> result = telaviva::mergeCameraLists(devices, groups);
    CHECK_EQ(result.size(), std::size_t(1));
    if (result.size() != 1) {
        return;
    }
    CHECK(!result[0].infrared);
}

TEST(camera_selection_merge_drops_group_without_device)
{
    const std::vector<telaviva::CameraDeviceRecord> devices = {deviceRecord("rgb", L"RGB")};
    const std::vector<telaviva::CameraGroupRecord> groups = {
        groupRecord(L"RGB", {sourceRecord(telaviva::CameraSourceKind::Color, "rgb")}),
        groupRecord(L"Virtual", {sourceRecord(telaviva::CameraSourceKind::Color, "rgb")}),
        groupRecord(L"Orphan", {sourceRecord(telaviva::CameraSourceKind::Color, "orphan")})};
    const std::vector<telaviva::CameraInfo> result = telaviva::mergeCameraLists(devices, groups);
    CHECK_EQ(result.size(), std::size_t(1));
    if (result.size() != 1) {
        return;
    }
    CHECK_EQ(result[0].id, std::string("rgb"));
    CHECK(result[0].name == L"RGB");
}

TEST(camera_selection_choose_group_prefers_device_name)
{
    const std::vector<telaviva::CameraGroupRecord> groups = {
        groupRecord(L"LenovoCam-Group", {sourceRecord(telaviva::CameraSourceKind::Color, "rgb")}),
        groupRecord(L"Lenovo Performance RGB Camera", {sourceRecord(telaviva::CameraSourceKind::Color, "rgb")})};
    CHECK_EQ(telaviva::chooseGroupForDevice(groups, "rgb", L"Lenovo Performance RGB Camera"), std::size_t(1));
    CHECK_EQ(telaviva::chooseGroupForDevice(groups, "missing", L"Lenovo Performance RGB Camera"), telaviva::kNoCameraGroup);
}

TEST(camera_selection_merge_duplicate_devices_keep_unique_first)
{
    // Layer 1: the merge over duplicated device records still yields unique ids and the first entry.
    const std::vector<telaviva::CameraDeviceRecord> devices = {
        deviceRecord("Dup", L"First"), deviceRecord("dup", L"Second"), deviceRecord("other", L"Other")};
    const std::vector<telaviva::CameraInfo> result = telaviva::mergeCameraLists(devices, {});
    CHECK_EQ(result.size(), std::size_t(2));
    if (result.size() != 2) {
        return;
    }
    CHECK_EQ(result[0].id, std::string("Dup"));
    CHECK(result[0].name == L"First");
    CHECK_EQ(result[1].id, std::string("other"));
}

TEST(camera_selection_picker_pipeline_has_unique_ids)
{
    // Layer 3: the page pipeline (selectable, then dedupe) never yields two items with one id, and
    // applying the dedupe again changes nothing.
    telaviva::CameraInfo infraredCamera = namedCamera("ir", L"IR");
    infraredCamera.infrared = true;
    const std::vector<telaviva::CameraInfo> backend = {
        namedCamera("rgb", L"LenovoCam-Group"), infraredCamera, namedCamera("RGB", L"Lenovo Performance RGB Camera")};
    const std::vector<telaviva::CameraInfo> once = telaviva::dedupeCameras(telaviva::selectableCameras(backend), {});
    CHECK_EQ(once.size(), std::size_t(1));
    if (once.size() != 1) {
        return;
    }
    CHECK_EQ(once[0].id, std::string("rgb"));
    CHECK(once[0].name == L"LenovoCam-Group");

    const std::vector<telaviva::CameraInfo> twice = telaviva::dedupeCameras(once, {});
    CHECK_EQ(twice.size(), std::size_t(1));
}

TEST(camera_selection_dedupe_name_match_survives_across_layers)
{
    // Layer 1 and 2: the entry whose name matches the device name survives, and a second dedupe keeps it.
    const std::vector<telaviva::CameraDeviceRecord> devices = {deviceRecord("rgb", L"Lenovo Performance RGB Camera")};
    const std::vector<telaviva::CameraInfo> cameras = {
        namedCamera("rgb", L"LenovoCam-Group"), namedCamera("rgb", L"Lenovo Performance RGB Camera")};
    const std::vector<telaviva::CameraInfo> once = telaviva::dedupeCameras(cameras, devices);
    CHECK_EQ(once.size(), std::size_t(1));
    if (once.size() != 1) {
        return;
    }
    CHECK(once[0].name == L"Lenovo Performance RGB Camera");
    const std::vector<telaviva::CameraInfo> twice = telaviva::dedupeCameras(once, {});
    CHECK_EQ(twice.size(), std::size_t(1));
    CHECK(twice[0].name == L"Lenovo Performance RGB Camera");
}

TEST(camera_selection_find_index_exact_match)
{
    const std::vector<telaviva::CameraInfo> cameras = {namedCamera("a", L"A"), namedCamera("b", L"B")};
    const std::optional<std::string> stored = std::string("b");
    const std::optional<std::size_t> found = telaviva::findCameraIndex(cameras, stored);
    CHECK(found.has_value());
    if (found) {
        CHECK_EQ(*found, std::size_t(1));
    }
    const std::optional<std::size_t> upper = telaviva::findCameraIndex(cameras, std::optional<std::string>("B"));
    CHECK(upper.has_value());
}

TEST(camera_selection_find_index_missing_id_gives_nullopt)
{
    const std::vector<telaviva::CameraInfo> cameras = {namedCamera("a", L"A")};
    const std::optional<std::size_t> missing = telaviva::findCameraIndex(cameras, std::optional<std::string>("gone"));
    CHECK(!missing.has_value());
    const std::optional<std::size_t> none = telaviva::findCameraIndex(cameras, std::nullopt);
    CHECK(!none.has_value());
}

// Lid closed (FR-020): built-in cameras are never chosen, and the remembered choice is only skipped.
TEST(camera_selection_lid_closed_history_skips_builtin_for_usb)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("builtin", false, true), makeCamera("usb-a")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"builtin", "usb-a"}, true);
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("usb-a"));
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_lid_closed_history_of_builtin_only_gives_nullopt)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("builtin", false, true)};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {"builtin"}, true);
    CHECK(!choice.effective.has_value());
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_lid_closed_default_skips_builtin_without_history)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("builtin", false, true), makeCamera("usb-a")};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {}, true);
    CHECK(choice.effective.has_value());
    if (!choice.effective) {
        return;
    }
    CHECK_EQ(choice.effective->id, std::string("usb-a"));
    CHECK(choice.fallbackToDefault);
}

TEST(camera_selection_lid_closed_only_builtin_gives_nullopt)
{
    const std::vector<telaviva::CameraInfo> connected = {makeCamera("builtin", false, true)};
    const telaviva::CameraChoice choice = telaviva::chooseCamera(connected, {}, true);
    CHECK(!choice.effective.has_value());
    CHECK(!choice.fallbackToDefault);
}

TEST(camera_selection_lid_closed_selectable_hides_builtin)
{
    const std::vector<telaviva::CameraInfo> connected = {
        makeCamera("builtin", false, true), makeCamera("ir-front", true), makeCamera("usb-a")};
    const std::vector<telaviva::CameraInfo> closed = telaviva::selectableCameras(connected, true);
    CHECK_EQ(closed.size(), std::size_t(1));
    if (closed.size() != 1) {
        return;
    }
    CHECK_EQ(closed[0].id, std::string("usb-a"));

    // With the lid open the built-in camera is listed again, in order.
    const std::vector<telaviva::CameraInfo> open = telaviva::selectableCameras(connected, false);
    CHECK_EQ(open.size(), std::size_t(2));
    if (open.size() == 2) {
        CHECK_EQ(open[0].id, std::string("builtin"));
        CHECK_EQ(open[1].id, std::string("usb-a"));
    }
}
