/**
 * Palworld VR Injector — Hook Layer (C/C++)
 * 
 * This library hooks into the DirectX/Vulkan rendering path used by Palworld
 * running under Wine/Proton on Linux. It intercepts:
 *   - IDXGIFactory::EnumAdapters() / CreateOutput() → inject virtual display adapter
 *   - D3D12 device creation → redirect to Vulkan swap chain
 *   - UE5 FTextureResource2D/FRenderTarget2D APIs → route through OpenXR
 * 
 * Architecture:
 *   ┌─────────────────┐     ┌──────────────────┐
 *   │  Palworld.exe   │────▶│  vrhook.so       │
 *   │  (Wine/Proton)  │     │  (LD_PRELOAD)    │
 *   │                  │     │                   │
 *   │  [D3D12 calls]  │◀───▶│  Hook layer:     │
 *   │                  │     │   • DXGI adapter  │
 *   │                  │     │   • Render target │
 *   │                  │     │   • Swap chain    │
 *   └─────────────────┘     │   • OpenXR init   │
 *                           └──────────────────┘
 *                                   │
 *                                   ▼
 *                          ┌──────────────────┐
 *                          │  Oasis Driver    │
 *                          │  (virtual display)│
 *                          └──────────────────┘
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * ABI Versioning
 * -------------------------------------------------------------------------- */

#define VRHOOK_ABI_VERSION_MAJOR   1
#define VRHOOK_ABI_VERSION_MINOR   0
#define VRHOOK_ABI_VERSION_PATCH   0

/* ABI compatibility check macro. The stub library should define this
 * when loaded into the target process to confirm version match. */
#ifndef VRHOOK_TARGET_ABI_VERSION
#  error "Target library does not support VRHook ABI v1.0.0"
#endif

/* ---------------------------------------------------------------------------
 * Core VR Context Opaque Handle
 * -------------------------------------------------------------------------- */

typedef struct _vrhook_context_t* vrh_context_handle_t;

/**
 * Initialize the VR hook subsystem for the current process.
 * 
 * This is called once per process on first use (e.g., from LD_PRELOAD's
 * constructor or an application entry point). It sets up:
 *   - DXGI adapter enumeration override
 *   - Virtual display device registration
 *   - Swap chain creation hooking
 *
 * Returns a handle to the context, or NULL on failure.
 */
vrh_context_handle_t vrh_create_context(void);

/**
 * Destroy the VR hook context and release all resources.
 */
void vrh_destroy_context(vrh_context_handle_t ctx);

/* ---------------------------------------------------------------------------
 * Adapter / Output Enumeration Hooking
 * -------------------------------------------------------------------------- */

/**
 * Enumerate connected displays (including virtual ones).
 * 
 * Called by DXGI after a display adapter is queried. The hook injects
 * the virtual display device created by the Oasis driver here.
 * 
 * @param pAdapter   The DXGI adapter whose outputs are being enumerated.
 * @param pfEnum     OUT: Receives TRUE if enumeration should continue,
 *                    FALSE if it should stop (e.g., after injecting virtual output).
 */
void vrh_enum_outputs(DXGI_ADAPTER_DESC* pAdapter, bool* pfEnum);

/**
 * Create a display output for the virtual VR headset.
 * 
 * @param pAdapter      The target adapter to attach the output to.
 * @param pOutputDesc   OUT: Receives the DXGI_OUTPUT_DESC for the virtual display.
 * @return TRUE on success, FALSE otherwise.
 */
bool vrh_create_virtual_output(DXGI_ADAPTER_DESC* pAdapter, DXGI_OUTPUT_DESC* pOutputDesc);

/**
 * Set the desired refresh rate and color space for the virtual output.
 */
void vrh_set_output_properties(
    DXGI_OUTPUT_DESC* pOutputDesc,
    UINT32 desiredRefreshRateHz,       /* e.g., 90 or 120 Hz */
    DXGI_COLOR_SPACE_TYPE colorSpace   /* e.g., DXGI_COLOR_SPACE_RGB_FULL_G22LL_CHLI_SRGB */
);

/* ---------------------------------------------------------------------------
 * Render Target / Swap Chain Hooking
 * -------------------------------------------------------------------------- */

/**
 * Initialize the OpenXR VR context and swap chain.
 * 
 * Called when the game requests a new render target (via D3D12CreateSwapChain,
 * or via UE5's FRenderTarget2D constructor). The hook intercepts these calls
 * and creates a Vulkan-backed swap chain that composes frames through OpenXR.
 * 
 * @param pDevice       The D3D12 device for which to create the swap chain.
 * @param pSwapDesc     IN: Desired back buffer format, width/height, count.
 * @return TRUE on success, FALSE otherwise.
 */
bool vrh_create_swap_chain(
    IDXGIAdapter* pAdapter,          /* The adapter owning the device */
    const DXGI_SWAP_CHAIN_DESC1*     pSwapDesc  /* Buffer format, size, etc. */
);

/**
 * Signal that a frame has been rendered and is ready for VR composition.
 * 
 * The hook composes the rendered frames (stereo if applicable) into the
 * virtual output buffer using OpenXR's XR_ComposeLayersKHR or Vulkan swap chain.
 * 
 * @param pSwapChain     The swap chain whose back buffers have been rendered.
 * @param frameIndex     Which frame in the sequence (0..pNumBackBuffers-1).
 */
void vrh_flip_frame(IDXGISwapChain* pSwapChain, UINT32 frameIndex);

/**
 * Tear down the VR swap chain and cleanup resources.
 */
void vrh_destroy_swap_chain(IDXGISwapChain* pSwapChain);

/* ---------------------------------------------------------------------------
 * Utility / Debugging
 * -------------------------------------------------------------------------- */

/**
 * Get the current hook state (for debugging).
 * 
 * @return A human-readable string describing whether the hook is active,
 *         partially initialized, or in a failed state.
 */
const char* vrh_get_status(void);

/**
 * Enable/disable debug logging from the hook layer.
 * 
 * Useful during development to see which DXGI/D3D12 calls are intercepted.
 */
void vrh_set_debug_logging(bool enabled);
