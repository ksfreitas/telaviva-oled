#ifndef TELAVIVA_CORE_H
#define TELAVIVA_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TV_CORE_ABI_VERSION 1u
typedef struct TvCore TvCore;
typedef struct TvBuffer { uint8_t *data; size_t len; } TvBuffer;
typedef void (*TvSnapshotReady)(uint64_t revision, void *userdata);

enum { TV_OK=0, TV_INVALID_ARGUMENT=1, TV_ABI_MISMATCH=2, TV_CLOSED=3, TV_INTERNAL=4, TV_SIZE_LIMIT=5 };

uint32_t tv_abi_version(void);
/* options is UTF-8 JSON: {"config_path":"...","os_locale":"pt-BR"}; NULL/0 selects defaults. */
int32_t tv_create(uint32_t abi_version, const uint8_t *options, size_t options_len,
                  TvSnapshotReady callback, void *userdata, TvCore **out_core);
/* Submit a UTF-8 JSON event/command; maximum input length is 64 KiB. */
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
/* Frees and zeroes a buffer returned by tv_snapshot. */
void tv_buffer_free(TvBuffer *buffer);

#ifdef __cplusplus
}
#endif
#endif
