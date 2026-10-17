/**
 * Palworld VR Injector — OpenXR Compositor Header (C++)
 */

#pragma once

#include <cstdint>
#include <chrono>

/* Opaque handle struct — wraps a pointer internally */
struct vrh_compositor_handle_t {
    void* opaque_ptr;
};

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Configuration Constants
 * -------------------------------------------------------------------------- */

#define VRHOOK_MAX_VR_DISPLAYS     2
#define VRHOOK_SWAPCHAIN_IMAGE_COUNT   3
#define VR_OUTPUT_WIDTH             2448
#define VR_OUTPUT_HEIGHT            1236
#define VR_REFRESH_HZ               90.0f

/* ---------------------------------------------------------------------------
 * Function Prototypes
 * -------------------------------------------------------------------------- */

vrh_compositor_handle_t vrh_create_compositor(void);
void vrh_destroy_compositor(vrh_compositor_handle_t comp);
vrh_compositor_handle_t vrh_begin_frame(vrh_compositor_handle_t comp);
void vrh_end_frame(vrh_compositor_handle_t comp);
bool vrh_compositor_is_active(vrh_compositor_handle_t comp);
uint32_t vrh_get_current_swapchain_index(vrh_compositor_handle_t comp);

#ifdef __cplusplus
}
#endif
