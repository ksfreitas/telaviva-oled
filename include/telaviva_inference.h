#ifndef TELAVIVA_INFERENCE_H
#define TELAVIVA_INFERENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TvInferenceHandle TvInferenceHandle;

typedef struct TvInferenceResult {
    int32_t status;
    float confidence;
} TvInferenceResult;

enum {
    TV_INFERENCE_ABSENT = 0,
    TV_INFERENCE_PRESENT = 1,
    TV_INFERENCE_INSUFFICIENT_LIGHT = 2,
    TV_INFERENCE_UNAVAILABLE = 3,
    TV_INFERENCE_ERROR_INVALID_ARGUMENT = -1,
    TV_INFERENCE_ERROR_RUNTIME = -2,
    TV_INFERENCE_ERROR_MODEL = -3,
    TV_INFERENCE_ERROR_PANIC = -4
};

enum {
    TV_INFERENCE_PROVIDER_CPU = 0,
    TV_INFERENCE_PROVIDER_COREML_CANDIDATE = 1
};

/* Paths must be absolute paths to app-bundled files. Returns 0 on success. Runtime is initialized once. */
int32_t tv_inference_create(const char *runtime_abs_path,
                            const char *model_abs_path,
                            TvInferenceHandle **out_handle);

/* Explicitly requests CPU or CoreML candidate; unavailable/failing CoreML falls back to CPU.
 * The app should request CoreML only for a device profile with measured energy benefit. */
int32_t tv_inference_create_with_provider(const char *runtime_abs_path,
                                          const char *model_abs_path,
                                          int32_t provider,
                                          TvInferenceHandle **out_handle);

/* Input is a top-down BGRA8 frame. Returns the result status (0 absent, 1 present,
 * 2 insufficient light, 3 unavailable); negative values are errors. No frame data
 * is retained or logged. */
int32_t tv_inference_detect_bgra(TvInferenceHandle *handle,
                                 const uint8_t *bytes,
                                 size_t len,
                                 uint32_t width,
                                 uint32_t height,
                                 size_t stride,
                                 TvInferenceResult *out_result);

/* Quality-only sample for low-light recovery. Does not create or run an inference session.
 * Returns 0 when sufficiently illuminated, 2 when insufficient, or a negative error. */
int32_t tv_inference_assess_light_bgra(const uint8_t *bytes,
                                       size_t len,
                                       uint32_t width,
                                       uint32_t height,
                                       size_t stride);

/* After successful create, reports whether the loaded runtime advertises CoreML:
 * 1 available, 0 unavailable, negative error. Capability discovery only; CPU remains selected. */
int32_t tv_inference_coreml_available(void);

/* Returns CPU (0) or CoreML candidate (1); this does not guarantee all model ops run there. */
int32_t tv_inference_selected_provider(const TvInferenceHandle *handle);

void tv_inference_destroy(TvInferenceHandle *handle);

#ifdef __cplusplus
}
#endif
#endif
