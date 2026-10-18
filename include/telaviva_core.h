#ifndef TELAVIVA_CORE_H
#define TELAVIVA_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TV_CORE_ABI_VERSION 1u
typedef struct TvCore TvCore;
typedef struct TvCameraLifecycle TvCameraLifecycle;
typedef struct TvControls TvControls;
typedef struct TvBuffer { uint8_t *data; size_t len; } TvBuffer;
typedef void (*TvSnapshotReady)(uint64_t revision, void *userdata);

enum { TV_OK=0, TV_INVALID_ARGUMENT=1, TV_ABI_MISMATCH=2, TV_CLOSED=3, TV_INTERNAL=4, TV_SIZE_LIMIT=5 };

uint32_t tv_abi_version(void);
/* options is UTF-8 JSON: {"config_path":"...","os_locale":"pt-BR"}; NULL/0 selects defaults. */
int32_t tv_create(uint32_t abi_version, const uint8_t *options, size_t options_len,
                  TvSnapshotReady callback, void *userdata, TvCore **out_core);
/* Submit a UTF-8 JSON event/command; maximum input length is 64 KiB.
 * After tv_shutdown, the null and size checks still run first, then TV_CLOSED is returned for
 * every input except a JSON object whose "type" is "hardware_result", which is still passed to
 * the core. Malformed JSON also returns TV_CLOSED after shutdown. */
int32_t tv_submit(TvCore *core, const uint8_t *json, size_t json_len);
/* Copies a revisioned UTF-8 JSON snapshot into a Rust-owned buffer (maximum 8 MiB).
 * pending_actions are dispatched asynchronously by the native host. Automatic
 * ownership checks use kind="read_brightness", value=null, and a raw-unit target;
 * report the raw readback through hardware_result.value. The core then decides
 * whether a matching set_brightness action is still appropriate. */
int32_t tv_snapshot(TvCore *core, TvBuffer *out_buffer);
/* Stops accepting commands, emits best-effort owned-value restore actions, and drains callbacks. */
int32_t tv_shutdown(TvCore *core);
void tv_destroy(TvCore *core);
/* Frees and zeroes a buffer returned by tv_snapshot or tv_camera_lifecycle_step. */
void tv_buffer_free(TvBuffer *buffer);

/* Camera session timing, additive to ABI version 1. The camera lifecycle reads no clock: the
 * shell passes "now" in seconds as a double, from its own steady (monotonic) clock, with every
 * time-dependent event. presence_observation.captured_at_ms is a different clock: it is Unix
 * epoch milliseconds from the wall clock. Events are
 * UTF-8 JSON objects with a "type" member: reconcile {mode, capture_mode, generation, camera_id,
 * always_on}, begin_acquisition {now}, acquired_frame, begin_light_assessment {now},
 * begin_dark_settle {now}, is_current_assessment {token}, frame {now}, timed_out {now},
 * cancel_deadlines, accepts_callback {generation}. Each result is {"value": ..., "state": {...}}. */
/* Creates a lifecycle in the inactive state (mode "disabled", no timers). Returns
 * TV_INVALID_ARGUMENT when out_lifecycle is NULL. */
int32_t tv_camera_lifecycle_create(TvCameraLifecycle **out_lifecycle);
/* Applies one JSON event and copies its result into out_buffer. The maximum input length is
 * 64 KiB; a longer input returns TV_SIZE_LIMIT. Malformed JSON, an unknown type or an unknown
 * field returns TV_INVALID_ARGUMENT. Both failures leave the lifecycle unchanged and write no
 * buffer. A handle poisoned by an earlier panic returns TV_INTERNAL. Free a written buffer with
 * tv_buffer_free. */
int32_t tv_camera_lifecycle_step(TvCameraLifecycle *lifecycle, const uint8_t *json,
                                 size_t json_len, TvBuffer *out_buffer);
/* Destroys a lifecycle handle; NULL is ignored. */
void tv_camera_lifecycle_destroy(TvCameraLifecycle *lifecycle);

/* Controls machine, additive to ABI version 1. Events are UTF-8 JSON objects with a "type"
 * member and the same status and buffer rules as the camera lifecycle. */
/* Creates an empty controls machine. Returns TV_INVALID_ARGUMENT when out_controls is NULL. */
int32_t tv_controls_create(TvControls **out_controls);
/* Applies one UTF-8 JSON event (at most 64 KiB) and copies the JSON result into out_buffer.
 * A longer input returns TV_SIZE_LIMIT. Malformed JSON or an invalid event returns
 * TV_INVALID_ARGUMENT. Both failures leave the machine unchanged and write no buffer. A handle
 * poisoned by an earlier panic returns TV_INTERNAL. Free a written buffer with tv_buffer_free. */
int32_t tv_controls_step(TvControls *controls, const uint8_t *json, size_t json_len,
                         TvBuffer *out_buffer);
/* Destroys a controls handle; NULL is ignored. */
void tv_controls_destroy(TvControls *controls);

/* Core version and update evaluation, additive to ABI version 1. Both are pure: no network and
 * no state. Free every written buffer with tv_buffer_free. */
/* Writes the core version (UTF-8, no terminator) to out. Free with tv_buffer_free. */
int32_t tv_core_version(TvBuffer *out);
/* Compares the installed version with a GitHub releases/latest answer and writes the
 * UpdateOutcome JSON to out. Free with tv_buffer_free. Pure: no network, no state.
 * body may be NULL only when body_len is 0. Returns TV_SIZE_LIMIT when body_len exceeds 1 MiB
 * or installed_len exceeds 64 bytes; out is written only on TV_OK. */
int32_t tv_update_evaluate(const uint8_t *installed, size_t installed_len,
                           uint32_t http_status,
                           const uint8_t *body, size_t body_len,
                           TvBuffer *out);

#ifdef __cplusplus
}
#endif
#endif
